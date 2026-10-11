#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "../until.h"

#define REGION_PAGES 4096u
#define PROBE_PAGES 256u
#define LINES (PAGE_SIZE/LINE_SIZE)
#define BATCH 64u
enum scenario { UNTRAINED, LOCAL_N, LOCAL_NM, REMOTE_SAME, REMOTE_OTHER,
                REMOTE_ONLY, NO_TRIGGER };
static const char *const names[]={"untrained","local_n","local_nm","remote_same",
                                  "remote_other","remote_only","no_trigger"};
struct options {
    size_t cpu_a,cpu_b,n,m,d,other,rounds,threshold,hold_us,scrub_loads;
    uint64_t seed;
    enum scenario scenario;
    const char *measure;
    int describe;
};
struct pair { uint8_t *x,*y; };
struct token { _Alignas(128) atomic_uint value; };
struct counters { int leader,child; uint64_t c0,c1; };
struct experiment {
    struct options o;
    struct token prepare,ready,go,done,finish;
    struct pair *a,*b;
    uint8_t *memory;
    uint64_t freq;
    size_t a_n,a_m,b_m;
    struct counters ca,cb;
    int observed_a,observed_b;
    uint64_t b_train_ticks,phase_ticks;
    size_t overruns;
};
static void die(const char *message) { perror(message); exit(1); }
static uint64_t random_next(uint64_t *s)
{
    uint64_t z=(*s+=0x9e3779b97f4a7c15ULL);
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27))*0x94d049bb133111ebULL;
    return z^(z>>31);
}
static inline uint64_t now(void)
{
    uint64_t t;
    asm volatile("isb; mrs %0,cntvct_el0; isb" : "=r"(t) :: "memory");
    return t;
}
static void pause_nops(size_t n)
{ for(size_t i=0;i<n;i++) asm volatile("nop" ::: "memory"); }
static void bind_cpu(size_t cpu)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set);
    if(sched_setaffinity(0,sizeof(set),&set)) die("affinity");
    if(sched_getcpu()!=(int)cpu) { errno=EXDEV; die("CPU verification"); }
}
static void publish(struct token *t,unsigned round)
{ atomic_store_explicit(&t->value,round,memory_order_release); }
static void await_token(struct experiment *e,struct token *t,unsigned round)
{
    uint64_t start=now(); size_t spins=0;
    while(atomic_load_explicit(&t->value,memory_order_acquire)!=round) {
        asm volatile("yield" ::: "memory");
        if((++spins&65535)==0 && now()-start>e->freq*10) {
            errno=ETIMEDOUT; die("worker handshake");
        }
    }
}
__attribute__((noinline,noclone)) static void train_load(void *p)
{ asm volatile("ldr x1,[%0]" :: "r"(p) : "x1","memory"); }
__attribute__((noinline,noclone)) static void trigger_load(void *p)
{ asm volatile("ldr x17,[%0]" :: "r"(p) : "x17","memory"); }
__attribute__((noinline,noclone)) static void scrub_load(void *p)
{ asm volatile("ldr x2,[%0]" :: "r"(p) : "x2","memory"); }
static void train(struct pair *seq,size_t count)
{
    for(size_t i=0;i<count;i++) {
        /* Each demand load is kept cold, using the validated learning form. */
        flush(seq[i].x); mfence(); train_load(seq[i].x); pause_nops(100);
        flush(seq[i].y); mfence(); train_load(seq[i].y); pause_nops(100);
    }
    mfence();
}
static void build_pairs(struct pair *seq,size_t count,uint8_t *base,size_t d,uint64_t seed)
{
    size_t pages[REGION_PAGES];
    for(size_t i=0;i<REGION_PAGES;i++) pages[i]=i;
    for(size_t i=REGION_PAGES;i>1;i--) {
        size_t j=random_next(&seed)%i,v=pages[i-1]; pages[i-1]=pages[j]; pages[j]=v;
    }
    for(size_t i=0;i<count;i++) {
        size_t line=random_next(&seed)%(LINES-d);
        seq[i].x=base+pages[i]*PAGE_SIZE+line*LINE_SIZE;
        seq[i].y=seq[i].x+d*LINE_SIZE;
    }
}
static void scrub(struct experiment *e,int remote,size_t round)
{
    uint8_t *base=e->memory+(2u+(unsigned)remote)*REGION_PAGES*PAGE_SIZE;
    uint64_t seed=e->o.seed+round*977u+remote*103u;
    cpp_rctx();
    for(size_t i=0;i<e->o.scrub_loads;i++) {
        size_t page=(i*977u+round)%REGION_PAGES;
        uint8_t *p=base+page*PAGE_SIZE+(random_next(&seed)%LINES)*LINE_SIZE;
        flush(p); mfence(); scrub_load(p);
    }
    mfence();
}
static int event_open(uint64_t event,int group)
{
    struct perf_event_attr a={0};
    a.size=sizeof(a); a.type=PERF_TYPE_RAW; a.config=event;
    a.disabled=1; a.exclude_kernel=1; a.exclude_hv=1; a.pinned=group<0;
    a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
    return syscall(__NR_perf_event_open,&a,0,-1,group,0);
}
static void counters_open(struct counters *c,const char *measure)
{
    c->leader=c->child=-1;
    if(!strcmp(measure,"cache")) return;
    c->leader=event_open(!strcmp(measure,"demand")?0x8144:0x8154,-1);
    if(c->leader<0) die("perf leader");
    c->child=event_open(!strcmp(measure,"demand")?0x814c:0x8155,c->leader);
    if(c->child<0) die("perf child");
}
static void measured_train(struct counters *c,struct pair *seq,size_t count)
{
    if(!count) return;
    if(c->leader>=0 && (ioctl(c->leader,PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP) ||
                       ioctl(c->leader,PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP))) die("perf enable");
    train(seq,count);
    if(c->leader>=0) {
        struct { uint64_t nr,enabled,running,v[2]; } r;
        if(ioctl(c->leader,PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP) ||
           read(c->leader,&r,sizeof(r))!=sizeof(r) || r.nr!=2 ||
           !r.running || r.enabled!=r.running) { errno=EIO; die("perf not fully scheduled"); }
        c->c0+=r.v[0]; c->c1+=r.v[1];
    }
}
static void counters_close(struct counters *c)
{ if(c->child>=0) close(c->child); if(c->leader>=0) close(c->leader); }
static void *worker_b(void *argument)
{
    struct experiment *e=argument;
    bind_cpu(e->o.cpu_b); e->observed_b=sched_getcpu();
    counters_open(&e->cb,e->o.measure);
    for(unsigned round=1;round<=e->o.rounds;round++) {
        await_token(e,&e->prepare,round);
        scrub(e,1,round);
        publish(&e->ready,round);
        await_token(e,&e->go,round);
        uint64_t start=now();
        measured_train(&e->cb,e->b,e->b_m);
        uint64_t elapsed=now()-start;
        e->b_train_ticks+=elapsed;
        uint64_t hold=(uint64_t)(((__uint128_t)e->freq*e->o.hold_us+999999)/1000000);
        if(elapsed>hold) e->overruns++;
        while(now()-start<hold) asm volatile("yield" ::: "memory");
        e->phase_ticks+=now()-start;
        publish(&e->done,round);
        await_token(e,&e->finish,round);
    }
    counters_close(&e->cb); return NULL;
}
static void usage(void)
{
    puts("--cpu-a N --cpu-b N --train-a-pairs N --train-b-pairs N --offset-lines D\n"
         "--other-offset D --rounds N --threshold-ns N --hold-us N --scrub-loads N\n"
         "--seed N --measure cache|demand|prefetch --describe\n"
         "--case untrained|local_n|local_nm|remote_same|remote_other|remote_only|no_trigger");
}
static int parse(int argc,char **argv,struct options *o)
{
    static const struct option opts[]={
        {"cpu-a",1,0,'a'},{"cpu-b",1,0,'b'},{"train-a-pairs",1,0,'n'},
        {"train-b-pairs",1,0,'m'},{"offset-lines",1,0,'d'},{"other-offset",1,0,'o'},
        {"rounds",1,0,'r'},{"threshold-ns",1,0,'t'},{"hold-us",1,0,'w'},
        {"scrub-loads",1,0,'s'},{"seed",1,0,'S'},{"measure",1,0,'M'},
        {"case",1,0,'c'},{"describe",0,0,'D'},{"help",0,0,'h'},{0,0,0,0}
    };
    int c;
    while((c=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        if(c=='h') { usage(); exit(0); }
        if(c=='D') { o->describe=1; continue; }
        if(c=='M') { o->measure=optarg; continue; }
        if(c=='c') {
            size_t i; for(i=0;i<sizeof(names)/sizeof(*names);i++) if(!strcmp(optarg,names[i])) break;
            if(i==sizeof(names)/sizeof(*names)) return -1;
            o->scenario=(enum scenario)i; continue;
        }
        if(c=='?' || !optarg || *optarg=='-') return -1;
        char *end; errno=0; unsigned long long v=strtoull(optarg,&end,0);
        if(errno || end==optarg || *end || v>SIZE_MAX) return -1;
        switch(c) {
        case 'a': o->cpu_a=v; break; case 'b': o->cpu_b=v; break;
        case 'n': o->n=v; break; case 'm': o->m=v; break;
        case 'd': o->d=v; break; case 'o': o->other=v; break;
        case 'r': o->rounds=v; break; case 't': o->threshold=v; break;
        case 'w': o->hold_us=v; break; case 's': o->scrub_loads=v; break;
        case 'S': o->seed=v; break; default: return -1;
        }
    }
    return optind!=argc || o->cpu_a>=CPU_SETSIZE || o->cpu_b>=CPU_SETSIZE ||
        o->cpu_a==o->cpu_b || o->n>REGION_PAGES || o->m>REGION_PAGES ||
        !o->d || o->d>=LINES || !o->other || o->other>=LINES || o->other==o->d ||
        !o->rounds || o->rounds>10000 || !o->threshold || !o->hold_us || o->hold_us>100000 ||
        o->scrub_loads>16384 || (strcmp(o->measure,"cache") && strcmp(o->measure,"demand") &&
        strcmp(o->measure,"prefetch")) ? -1:0;
}
int main(int argc,char **argv)
{
    struct experiment *e=aligned_alloc(128,(sizeof(*e)+127)&~(size_t)127);
    if(!e) die("experiment allocation");
    memset(e,0,sizeof(*e));
    e->o=(struct options){6,7,300,300,16,13,30,80,2000,4096,20260913,REMOTE_SAME,"cache",0};
    if(parse(argc,argv,&e->o)) { usage(); free(e); return 2; }
    e->a_n=e->o.scenario==UNTRAINED || e->o.scenario==REMOTE_ONLY ? 0:e->o.n;
    e->a_m=e->o.scenario==LOCAL_NM?e->o.m:0;
    e->b_m=e->o.scenario>=REMOTE_SAME?e->o.m:0;
    size_t b_d=e->o.scenario==REMOTE_OTHER?e->o.other:e->o.d;
    if(e->o.describe) {
        printf("case=%s cpu_a=%zu cpu_b=%zu a_pairs=%zu b_pairs=%zu b_offset=%zu probe_cpu=%zu\n",
               names[e->o.scenario],e->o.cpu_a,e->o.cpu_b,e->a_n+e->a_m,e->b_m,b_d,e->o.cpu_a);
        free(e); return 0;
    }
    if(strcmp(e->o.measure,"cache") && access("/sys/module/prefetch_pmu_event",F_OK)==0) {
        fputs("Unload prefetch_pmu_event before perf measurement\n",stderr); free(e); return 1;
    }
    if(sysconf(_SC_PAGESIZE)!=PAGE_SIZE) { fputs("Requires 4KiB pages\n",stderr); return 1; }
    bind_cpu(e->o.cpu_a); e->observed_a=sched_getcpu();
    asm volatile("mrs %0,cntfrq_el0" : "=r"(e->freq));
    size_t bytes=(4u*REGION_PAGES+PROBE_PAGES)*PAGE_SIZE;
    e->memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if(e->memory==MAP_FAILED) die("mmap");
    /* Materialize distinct private physical pages before either training phase. */
    for(size_t i=0;i<bytes;i+=PAGE_SIZE) *(volatile uint64_t *)(e->memory+i)=i/PAGE_SIZE+1;
    e->a=calloc(e->o.n+1,sizeof(*e->a)); e->b=calloc(e->o.m+1,sizeof(*e->b));
    if(!e->a || !e->b) die("sequence allocation");
    atomic_init(&e->prepare.value,0); atomic_init(&e->ready.value,0);
    atomic_init(&e->go.value,0); atomic_init(&e->done.value,0); atomic_init(&e->finish.value,0);
    counters_open(&e->ca,e->o.measure);
    pthread_t thread; int error=pthread_create(&thread,NULL,worker_b,e);
    if(error) { errno=error; die("pthread_create"); }
    uint64_t probes=0,hits=0,latency=0;
    size_t local_overruns=0;
    for(unsigned round=1;round<=e->o.rounds;round++) {
        uint64_t seed=e->o.seed+round*977;
        build_pairs(e->a,e->o.n,e->memory,e->o.d,seed);
        build_pairs(e->b,e->o.m,e->memory+REGION_PAGES*PAGE_SIZE,b_d,seed^0x91af);
        uint8_t *triggers[BATCH],*targets[BATCH];
        for(size_t i=0;i<BATCH;i++) {
            size_t page=4u*REGION_PAGES+(i*167+round)%PROBE_PAGES;
            triggers[i]=e->memory+page*PAGE_SIZE+(random_next(&seed)%(LINES-e->o.d))*LINE_SIZE;
            targets[i]=triggers[i]+e->o.d*LINE_SIZE;
            flush(triggers[i]); flush(targets[i]);
        }
        mfence(); publish(&e->prepare,round);
        scrub(e,0,round); await_token(e,&e->ready,round);
        measured_train(&e->ca,e->a,e->a_n);
        uint64_t start=now(); publish(&e->go,round);
        measured_train(&e->ca,e->b,e->a_m);
        if(e->a_m && (now()-start)*1000000/e->freq>e->o.hold_us) local_overruns++;
        await_token(e,&e->done,round);
        if(!strcmp(e->o.measure,"cache")) {
            if(e->o.scenario!=NO_TRIGGER) for(size_t i=0;i<BATCH;i++) trigger_load(triggers[i]);
            mfence(); pause_nops(2000);
            for(size_t j=0;j<BATCH;j++) {
                size_t i=(j*17+round)%BATCH;
                mfence(); uint64_t t=now(); mLoad_inline(targets[i]); mfence();
                uint64_t dt=now()-t;
                hits+=(double)dt*1e9/e->freq<=e->o.threshold; probes++; latency+=dt;
            }
        }
        publish(&e->finish,round);
    }
    error=pthread_join(thread,NULL); if(error) { errno=error; die("pthread_join"); }
    counters_close(&e->ca);
    double na=2.0*(e->a_n+e->a_m)*e->o.rounds,nb=2.0*e->b_m*e->o.rounds;
    printf("case\tmeasure\tcpu_a\tcpu_b\tobserved_a\tobserved_b\tn\tm\toffset\tother_offset\tseed\trounds\thold_us\tscrub_loads\tprobes\thits\thit_pct\tavg_ns\ta_loads\tb_loads\ta_event0\ta_event1\tb_event0\tb_event1\ta_event0_per_load\ta_event1_per_load\tb_event0_per_load\tb_event1_per_load\tb_train_us\tphase_us\tremote_overruns\tlocal_overruns\n");
    printf("%s\t%s\t%zu\t%zu\t%d\t%d\t%zu\t%zu\t%zu\t%zu\t%llu\t%zu\t%zu\t%zu\t%llu\t%llu\t%.5f\t%.3f\t%.0f\t%.0f\t%llu\t%llu\t%llu\t%llu\t%.6f\t%.6f\t%.6f\t%.6f\t%.3f\t%.3f\t%zu\t%zu\n",
        names[e->o.scenario],e->o.measure,e->o.cpu_a,e->o.cpu_b,e->observed_a,e->observed_b,
        e->o.n,e->o.m,e->o.d,e->o.other,(unsigned long long)e->o.seed,e->o.rounds,e->o.hold_us,e->o.scrub_loads,
        (unsigned long long)probes,(unsigned long long)hits,probes?100.0*hits/probes:0,
        probes?(double)latency*1e9/e->freq/probes:0,na,nb,
        (unsigned long long)e->ca.c0,(unsigned long long)e->ca.c1,
        (unsigned long long)e->cb.c0,(unsigned long long)e->cb.c1,
        na?e->ca.c0/na:0,na?e->ca.c1/na:0,nb?e->cb.c0/nb:0,nb?e->cb.c1/nb:0,
        (double)e->b_train_ticks*1e6/e->freq/e->o.rounds,
        (double)e->phase_ticks*1e6/e->freq/e->o.rounds,e->overruns,local_overruns);
    munmap(e->memory,bytes); free(e->a); free(e->b); free(e); return 0;
}
