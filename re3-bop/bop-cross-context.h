/* Shared implementation keeps the thread/process experiment identical except
 * for context creation. No EL1 training, migration, or reset during handoff. */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "../until.h"

#define CC_PAGES 4096u
#define CC_BATCH 64u
#define CC_PROBE_PAGES 256u
#define CC_LINES (PAGE_SIZE/LINE_SIZE)
#ifndef CC_L2_PREFETCH_EVENT
#define CC_L2_PREFETCH_EVENT 0x8155
#endif
enum cc_case { UNTRAINED, LOCAL_N, LOCAL_NM, SWITCH_NONE, SWITCH_LOCAL_NM,
    SPLIT_SAME, SPLIT_OTHER, B_ONLY, NO_TRIGGER, RETAIN_LOCAL, RETAIN_SWITCH,
    RETAIN_PEER, UNTRAINED_PEER, PEER_LOCAL, SPLIT_PEER };
static const char *const cc_names[]={"untrained","local_n","local_nm","switch_none",
    "switch_local_nm","split_same","split_other","b_only","no_trigger",
    "retain_local","retain_switch","retain_peer","untrained_peer","peer_local","split_peer"};
struct cc_options {
    size_t cpu,n,m,full,d,other,rounds,threshold,batch;
    uint64_t seed;
    enum cc_case scenario;
    const char *measure;
    int describe;
};
struct cc_plan { size_t a_pairs,b_pairs,b_d; int peer_probe,switching,trigger; };
struct cc_pair { uint8_t *x,*y; };
struct cc_stats {
    long pid,tid; int cpu;
    uint64_t loads,event0,event1,probes,hits,ticks,faults,train_ticks,handoff_ticks;
};
struct cc_shared {
    struct cc_options o;
    struct cc_plan p;
    int to_b[2],to_a[2];
    uint64_t freq;
    struct cc_stats b;
};
struct cc_local {
    struct cc_shared *s;
    uint8_t *mem;
    struct cc_pair train[CC_PAGES];
    uint8_t *x[CC_BATCH],*y[CC_BATCH];
    struct cc_stats stats;
    int leader,child,role;
    long faults_before;
};
static void cc_die(const char *what) { perror(what); exit(1); }
static uint64_t cc_now(void)
{
    uint64_t t; asm volatile("isb; mrs %0,cntvct_el0; isb":"=r"(t)::"memory"); return t;
}
static uint64_t cc_rand(uint64_t *s)
{
    uint64_t z=(*s+=0x9e3779b97f4a7c15ULL);
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL; z=(z^(z>>27))*0x94d049bb133111ebULL;
    return z^(z>>31);
}
static void cc_nops(size_t n) { for(size_t i=0;i<n;i++) asm volatile("nop":::"memory"); }
__attribute__((noinline,noclone)) static void cc_train_load(void *p)
{ asm volatile("ldr x1,[%0]"::"r"(p):"x1","memory"); }
__attribute__((noinline,noclone)) static void cc_trigger_load(void *p)
{ asm volatile("ldr x17,[%0]"::"r"(p):"x17","memory"); }
/* Reuse the buffer warmed by the initial handshake. Inlining otherwise leaves
 * distinct ready/done byte slots on fork-inherited, potentially COW stack pages. */
__attribute__((noinline,noclone)) static void cc_send(int fd,char c)
{
    ssize_t n; do { n=write(fd,&c,1); } while(n<0 && errno==EINTR);
    if(n!=1) cc_die("pipe write");
}
static void cc_receive(int fd,char expected)
{
    char c=0; ssize_t n; do { n=read(fd,&c,1); } while(n<0 && errno==EINTR);
    if(n!=1 || c!=expected) { errno=EPROTO; cc_die("pipe read/protocol"); }
}
static struct cc_plan cc_plan_for(struct cc_options o)
{
    struct cc_plan p={0,0,o.d,0,1,1};
    switch(o.scenario) {
    case UNTRAINED: break;
    case LOCAL_N: p.a_pairs=o.n; p.switching=0; break;
    case LOCAL_NM: p.a_pairs=o.n+o.m; p.switching=0; break;
    case SWITCH_NONE: p.a_pairs=o.n; break;
    case SWITCH_LOCAL_NM: p.a_pairs=o.n+o.m; break;
    case SPLIT_SAME: p.a_pairs=o.n; p.b_pairs=o.m; break;
    case SPLIT_OTHER: p.a_pairs=o.n; p.b_pairs=o.m; p.b_d=o.other; break;
    case B_ONLY: p.b_pairs=o.m; break;
    case NO_TRIGGER: p.a_pairs=o.n; p.b_pairs=o.m; p.trigger=0; break;
    case RETAIN_LOCAL: p.a_pairs=o.full; p.switching=0; break;
    case RETAIN_SWITCH: p.a_pairs=o.full; break;
    case RETAIN_PEER: p.a_pairs=o.full; p.peer_probe=1; break;
    case UNTRAINED_PEER: p.peer_probe=1; break;
    case PEER_LOCAL: p.b_pairs=o.full; p.peer_probe=1; break;
    case SPLIT_PEER: p.a_pairs=o.n; p.b_pairs=o.m; p.peer_probe=1; break;
    }
    return p;
}
/* Keep one reusable stack frame: an inlined first getrusage can itself COW
 * a fork-inherited main stack page after the kernel has sampled ru_minflt. */
__attribute__((noinline,noclone)) static long cc_faults(void)
{
    struct rusage r;
    /* A warmup at another call depth need not touch this exact stack page.
     * Materialize the complete copyout destination BEFORE the kernel samples. */
    volatile unsigned char *scratch=(volatile unsigned char *)&r;
    for(size_t i=0;i<sizeof(r);i++) scratch[i]=0;
    if(getrusage(RUSAGE_THREAD,&r)) cc_die("getrusage");
    return r.ru_minflt+r.ru_majflt;
}
static int cc_event(uint64_t event,int group)
{
    struct perf_event_attr a={0}; a.size=sizeof(a); a.type=PERF_TYPE_RAW; a.config=event;
    a.disabled=1; a.pinned=group<0; a.exclude_kernel=1; a.exclude_hv=1;
    a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
    return syscall(__NR_perf_event_open,&a,0,-1,group,0);
}
static struct cc_local *cc_init(struct cc_shared *s,int role)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(s->o.cpu,&set);
    if(sched_setaffinity(0,sizeof(set),&set)) cc_die("affinity");
    struct cc_local *l=calloc(1,sizeof(*l)); if(!l) cc_die("local allocation");
    l->s=s; l->role=role; l->leader=l->child=-1;
    l->stats.pid=getpid(); l->stats.tid=syscall(__NR_gettid); l->stats.cpu=sched_getcpu();
    if(l->stats.cpu!=(int)s->o.cpu) { errno=EXDEV; cc_die("CPU mismatch"); }
    size_t bytes=(CC_PAGES+CC_PROBE_PAGES)*PAGE_SIZE;
    l->mem=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if(l->mem==MAP_FAILED) cc_die("mmap");
    /* mmap occurs independently in each context; writes materialize private PFNs. */
    for(size_t i=0;i<bytes;i+=PAGE_SIZE) *(volatile uint64_t *)(l->mem+i)=i/PAGE_SIZE+1+role;
    if(strcmp(s->o.measure,"cache")) {
        int demand=!strcmp(s->o.measure,"demand");
        l->leader=cc_event(demand?0x8144:0x8154,-1);
        if(l->leader<0) cc_die("perf leader");
        l->child=cc_event(demand?0x814c:CC_L2_PREFETCH_EVENT,l->leader);
        if(l->child<0) cc_die("perf child");
    }
    cc_train_load(l->mem); cc_trigger_load(l->mem);
    (void)cc_faults();
    return l;
}
static void cc_build(struct cc_pair *seq,size_t count,uint8_t *base,size_t d,uint64_t seed)
{
    size_t pages[CC_PAGES];
    for(size_t i=0;i<CC_PAGES;i++) pages[i]=i;
    for(size_t i=CC_PAGES;i>1;i--) {
        size_t j=cc_rand(&seed)%i,t=pages[i-1]; pages[i-1]=pages[j]; pages[j]=t;
    }
    for(size_t i=0;i<count;i++) {
        seq[i].x=base+pages[i]*PAGE_SIZE+(cc_rand(&seed)%(CC_LINES-d))*LINE_SIZE;
        seq[i].y=seq[i].x+d*LINE_SIZE;
    }
}
static void cc_prepare(struct cc_local *l,size_t round)
{
    struct cc_shared *s=l->s;
    uint64_t seed=s->o.seed+round*977;
    size_t count=l->role?s->p.b_pairs:s->p.a_pairs;
    size_t d=l->role?s->p.b_d:s->o.d;
    cc_build(l->train,count,l->mem,d,seed^(l->role?0x91af:0));
    for(size_t i=0;i<s->o.batch;i++) {
        size_t page=CC_PAGES+(i*167+round)%CC_PROBE_PAGES;
        l->x[i]=l->mem+page*PAGE_SIZE+(cc_rand(&seed)%(CC_LINES-s->o.d))*LINE_SIZE;
        l->y[i]=l->x[i]+s->o.d*LINE_SIZE;
        flush(l->x[i]); flush(l->y[i]);
    }
    mfence();
}
static void cc_train(struct cc_local *l,struct cc_pair *seq,size_t count)
{
    if(!count) return;
    if(l->leader>=0 && (ioctl(l->leader,PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP) ||
        ioctl(l->leader,PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP))) cc_die("perf enable");
    uint64_t start=cc_now();
    for(size_t i=0;i<count;i++) {
        /* Match test-bop.c: start with both lines cold, but do not evict a Y
         * prefetch triggered by the subsequent X load. */
        flush(seq[i].x); mfence(); flush(seq[i].y); mfence();
        cc_train_load(seq[i].x); cc_nops(100);
        cc_train_load(seq[i].y); cc_nops(100);
    }
    mfence(); l->stats.train_ticks+=cc_now()-start; l->stats.loads+=2*count;
    if(l->leader>=0) {
        struct { uint64_t nr,enabled,running,v[2]; } r;
        if(ioctl(l->leader,PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP) ||
            read(l->leader,&r,sizeof(r))!=sizeof(r) || r.nr!=2 || !r.running || r.enabled!=r.running)
            { errno=EIO; cc_die("perf group not fully scheduled"); }
        l->stats.event0+=r.v[0]; l->stats.event1+=r.v[1];
    }
}
static void cc_probe(struct cc_local *l,size_t round)
{
    if(strcmp(l->s->o.measure,"cache")) return;
    if(l->s->p.trigger) for(size_t i=0;i<l->s->o.batch;i++) cc_trigger_load(l->x[i]);
    mfence(); cc_nops(2000);
    for(size_t j=0;j<l->s->o.batch;j++) {
        size_t i=(j*17+round)%l->s->o.batch;
        mfence(); uint64_t start=cc_now(); mLoad_inline(l->y[i]); mfence();
        uint64_t elapsed=cc_now()-start;
        l->stats.hits+=(double)elapsed*1e9/l->s->freq<=l->s->o.threshold;
        l->stats.probes++; l->stats.ticks+=elapsed;
    }
}
static void cc_cleanup(struct cc_local *l)
{
    if(l->leader>=0) close(l->leader);
    if(l->child>=0) close(l->child);
    munmap(l->mem,(CC_PAGES+CC_PROBE_PAGES)*PAGE_SIZE); free(l);
}
static void *cc_worker(void *argument)
{
    struct cc_shared *s=argument;
    if(CONTEXT_PROCESS) { close(s->to_b[1]); close(s->to_a[0]); }
    struct cc_local *l=cc_init(s,1);
    cc_send(s->to_a[1],'I');
    for(size_t round=1;round<=s->o.rounds;round++) {
        cc_receive(s->to_b[0],'P'); cc_prepare(l,round);
        /* B prepares before A resets its prediction context and trains. */
        l->faults_before=cc_faults(); cc_send(s->to_a[1],'R');
        cc_receive(s->to_b[0],s->p.switching?'G':'S');
        if(s->p.switching) {
            cc_train(l,l->train,s->p.b_pairs);
            if(s->p.peer_probe) cc_probe(l,round);
        }
        cc_send(s->to_a[1],'D');
        /* Do not let final-round teardown run before A's probe. */
        cc_receive(s->to_b[0],'F');
        l->stats.faults+=cc_faults()-l->faults_before;
    }
    s->b=l->stats; cc_cleanup(l);
    return NULL;
}
static void cc_usage(void)
{
    puts("--cpu N --train-a-pairs N --train-b-pairs N --full-pairs N --offset-lines D\n"
         "--other-offset D --rounds N --probe-batch N --threshold-ns N --seed N\n"
         "--measure cache|demand|prefetch --case NAME --describe");
    for(size_t i=0;i<sizeof(cc_names)/sizeof(*cc_names);i++) puts(cc_names[i]);
}
static int cc_parse(int argc,char **argv,struct cc_options *o)
{
    static const struct option opts[]={
        {"cpu",1,0,'c'},{"train-a-pairs",1,0,'n'},{"train-b-pairs",1,0,'m'},
        {"full-pairs",1,0,'f'},{"offset-lines",1,0,'d'},{"other-offset",1,0,'o'},
        {"rounds",1,0,'r'},{"probe-batch",1,0,'b'},{"threshold-ns",1,0,'t'},
        {"seed",1,0,'S'},{"measure",1,0,'M'},{"case",1,0,'C'},
        {"describe",0,0,'D'},{"help",0,0,'h'},{0,0,0,0}};
    int c;
    while((c=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        if(c=='h') { cc_usage(); exit(0); }
        if(c=='D') { o->describe=1; continue; }
        if(c=='M') { o->measure=optarg; continue; }
        if(c=='C') {
            size_t i; for(i=0;i<sizeof(cc_names)/sizeof(*cc_names);i++) if(!strcmp(optarg,cc_names[i])) break;
            if(i==sizeof(cc_names)/sizeof(*cc_names)) return -1;
            o->scenario=(enum cc_case)i; continue;
        }
        if(c=='?' || !optarg || *optarg=='-') return -1;
        char *end; errno=0; unsigned long long v=strtoull(optarg,&end,0);
        if(errno || end==optarg || *end || v>SIZE_MAX) return -1;
        switch(c) {
        case 'c': o->cpu=v; break; case 'n': o->n=v; break; case 'm': o->m=v; break;
        case 'f': o->full=v; break; case 'd': o->d=v; break; case 'o': o->other=v; break;
        case 'r': o->rounds=v; break; case 'b': o->batch=v; break;
        case 't': o->threshold=v; break;
        case 'S': o->seed=v; break; default: return -1;
        }
    }
    return optind!=argc || o->cpu>=CPU_SETSIZE || o->n>CC_PAGES || o->m>CC_PAGES ||
        o->full>CC_PAGES || !o->d || o->d>=CC_LINES || !o->other || o->other>=CC_LINES ||
        o->d==o->other || !o->rounds || o->rounds>10000 || !o->batch || o->batch>CC_BATCH ||
        !o->threshold ||
        ((o->scenario==LOCAL_NM || o->scenario==SWITCH_LOCAL_NM) && o->n>CC_PAGES-o->m) ||
        (strcmp(o->measure,"cache") && strcmp(o->measure,"demand") && strcmp(o->measure,"prefetch"))?-1:0;
}
int main(int argc,char **argv)
{
    struct cc_options o={6,300,300,1500,16,13,30,80,64,20260913,SPLIT_SAME,"cache",0};
    if(cc_parse(argc,argv,&o)) { cc_usage(); return 2; }
    struct cc_plan p=cc_plan_for(o);
    if(o.describe) {
        printf("mode=%s case=%s a_pairs=%zu b_pairs=%zu b_offset=%zu probe=%s switch=%d probe_batch=%zu l2_prefetch_event=%#x\n",
            CONTEXT_PROCESS?"process":"thread",cc_names[o.scenario],p.a_pairs,
            p.b_pairs,p.b_d,p.peer_probe?"b":"a",p.switching,o.batch,CC_L2_PREFETCH_EVENT); return 0;
    }
    if(sysconf(_SC_PAGESIZE)!=PAGE_SIZE) { fputs("Requires 4KiB pages\n",stderr); return 1; }
    if(strcmp(o.measure,"cache") && access("/sys/module/prefetch_pmu_event",F_OK)==0) {
        fputs("Unload prefetch_pmu_event before perf measurement\n",stderr); return 1;
    }
    alarm(120); /* Bound protocol failures in both executables. */
    struct cc_shared *s=mmap(NULL,sizeof(*s),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(s==MAP_FAILED) cc_die("shared mmap");
    memset(s,0,sizeof(*s)); s->o=o; s->p=p;
    asm volatile("mrs %0,cntfrq_el0":"=r"(s->freq));
    if(pipe(s->to_b) || pipe(s->to_a)) cc_die("pipe");
    pthread_t thread; pid_t child=-1;
    if(CONTEXT_PROCESS) {
        child=fork(); if(child<0) cc_die("fork");
        if(!child) { alarm(120); cc_worker(s); _exit(0); }
        close(s->to_b[0]); close(s->to_a[1]);
    } else {
        int error=pthread_create(&thread,NULL,cc_worker,s);
        if(error) { errno=error; cc_die("pthread_create"); }
    }
    struct cc_local *l=cc_init(s,0); cc_receive(s->to_a[0],'I');
    for(size_t round=1;round<=o.rounds;round++) {
        cc_prepare(l,round); cc_send(s->to_b[1],'P'); cc_receive(s->to_a[0],'R');
        l->faults_before=cc_faults(); cpp_rctx();
        cc_train(l,l->train,p.a_pairs);
        if(p.switching) {
            uint64_t start=cc_now(); cc_send(s->to_b[1],'G'); cc_receive(s->to_a[0],'D');
            l->stats.handoff_ticks+=cc_now()-start;
        }
        if(!p.peer_probe) cc_probe(l,round);
        l->stats.faults+=cc_faults()-l->faults_before;
        /* In no-switch controls, release B only after the probe is complete. */
        if(!p.switching) { cc_send(s->to_b[1],'S'); cc_receive(s->to_a[0],'D'); }
        cc_send(s->to_b[1],'F');
    }
    if(CONTEXT_PROCESS) {
        int status; if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))
            { errno=ECHILD; cc_die("child failed"); }
    } else {
        int error=pthread_join(thread,NULL); if(error) { errno=error; cc_die("pthread_join"); }
        close(s->to_b[0]); close(s->to_a[1]);
    }
    struct cc_stats a=l->stats,b=s->b,q=p.peer_probe?b:a;
    printf("mode\tcase\tmeasure\tcpu_a\tcpu_b\tpid_a\tpid_b\ttid_a\ttid_b\tn\tm\tfull\toffset\tseed\trounds\tprobe_batch\tprobe\tprobes\thits\thit_pct\tavg_ns\ta_loads\tb_loads\ta_event0\ta_event1\tb_event0\tb_event1\ta_event0_per_load\ta_event1_per_load\tb_event0_per_load\tb_event1_per_load\tcritical_faults_a\tcritical_faults_b\ta_train_us\tb_train_us\thandoff_us\n");
    printf("%s\t%s\t%s\t%d\t%d\t%ld\t%ld\t%ld\t%ld\t%zu\t%zu\t%zu\t%zu\t%llu\t%zu\t%zu\t%s\t%llu\t%llu\t%.5f\t%.3f\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%.6f\t%.6f\t%.6f\t%.6f\t%llu\t%llu\t%.3f\t%.3f\t%.3f\n",
        CONTEXT_PROCESS?"process":"thread",cc_names[o.scenario],o.measure,a.cpu,b.cpu,
        a.pid,b.pid,a.tid,b.tid,o.n,o.m,o.full,o.d,(unsigned long long)o.seed,o.rounds,o.batch,p.peer_probe?"b":"a",
        (unsigned long long)q.probes,(unsigned long long)q.hits,q.probes?100.0*q.hits/q.probes:0,
        q.probes?(double)q.ticks*1e9/s->freq/q.probes:0,
        (unsigned long long)a.loads,(unsigned long long)b.loads,
        (unsigned long long)a.event0,(unsigned long long)a.event1,
        (unsigned long long)b.event0,(unsigned long long)b.event1,
        a.loads?(double)a.event0/a.loads:0,a.loads?(double)a.event1/a.loads:0,
        b.loads?(double)b.event0/b.loads:0,b.loads?(double)b.event1/b.loads:0,
        (unsigned long long)a.faults,(unsigned long long)b.faults,
        (double)a.train_ticks*1e6/s->freq/o.rounds,(double)b.train_ticks*1e6/s->freq/o.rounds,
        (double)a.handoff_ticks*1e6/s->freq/o.rounds);
    cc_cleanup(l); close(s->to_b[1]); close(s->to_a[0]); munmap(s,sizeof(*s)); return 0;
}
