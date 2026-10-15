#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include "../until.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_event.h"
#include "../kernel-modules/prefetch-pmu-event/prefetch_pmu_ioctl.h"

/* Independent A/B learning experiment. Each region owns different pages. */
#define REGION_PAGES 4096u
#define LINES (PAGE_SIZE / LINE_SIZE)
#define PCS 16u
#define BATCH 64u
typedef void (*load_fn)(void *);
#define ACCESS(N, R) \
    __attribute__((noinline, noclone)) static void pc##N(void *p) \
    { asm volatile("ldr " #R ", [%0]" :: "r"(p) : #R, "memory"); }
ACCESS(0,x1) ACCESS(1,x2) ACCESS(2,x3) ACCESS(3,x4)
ACCESS(4,x5) ACCESS(5,x6) ACCESS(6,x7) ACCESS(7,x8)
ACCESS(8,x9) ACCESS(9,x10) ACCESS(10,x11) ACCESS(11,x12)
ACCESS(12,x13) ACCESS(13,x14) ACCESS(14,x15) ACCESS(15,x16)
static load_fn const pcs[PCS] = {pc0,pc1,pc2,pc3,pc4,pc5,pc6,pc7,
                                pc8,pc9,pc10,pc11,pc12,pc13,pc14,pc15};
__attribute__((noinline, noclone)) static void trigger(void *p)
{ asm volatile("ldr x17, [%0]" :: "r"(p) : "x17", "memory"); }
static uint64_t rng_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static uint64_t ticks(void)
{
    uint64_t t;
    asm volatile("dsb sy; isb; mrs %0, cntvct_el0; isb" : "=r"(t) :: "memory");
    return t;
}
static uint64_t frequency;
static double ns(uint64_t t) { return (double)t * 1e9 / frequency; }
static void delay(size_t count)
{ for (size_t i = 0; i < count; ++i) asm volatile("nop" ::: "memory"); }
struct opts {
    const char *pattern, *pc, *flush, *measure;
    size_t initial, loads, pages, offset, probe_offset, rounds, cpu, gap, threshold;
    uint64_t seed;
    int no_trigger, describe;
    const char *resident;
    size_t evict_kb;
    int skip_b;
    int shuffle_b;
    size_t b_limit;
};
struct entry { void *address; load_fn load; };
static int oneof(const char *s, const char *a, const char *b, const char *c, const char *d)
{ return !strcmp(s,a) || !strcmp(s,b) || !strcmp(s,c) || !strcmp(s,d); }
static void usage(void)
{
    puts("--pattern pair|stride|chain|random --pc single|rotate|random\n"
         "--flush batch|pair|each|fence --measure cache|pmu|timing|demand\n"
         "--initial-pairs N --loads N --pages N --offset N --probe-offset N\n"
         "--rounds N --cpu N --gap N --threshold-ns N --seed N\n"
         "--no-trigger --describe --skip-b\n"
         "--resident cold|demand|swl1|swl2|hw-next|hw-stream|hw-offset --evict-kb N\n"
         "hw-offset-consumed: consume HW-supplied targets once before A/B;\n"
         "hw-offset-demand: same +8 primer, but direct demand target warming.\n"
         "--shuffle-b --b-limit N (even number of executed B loads)\n"
         "--measure access (L2 demand accesses/misses) or prefetch (L1/L2 HW requests)\n"
         "--measure supply counts HW requests during target preparation only.\n"
         "loads is an even count of demand loads, NOT pair count.\n"
         "pair/chain/random use pages; stride uses a linear virtual stream.\n"
         "pair flushes two upcoming addresses; each flushes just the next one;\n"
         "fence uses batch flushing plus a barrier before each load.");
}
static int parse(int argc, char **argv, struct opts *o)
{
    static const struct option args[] = {
        {"pattern",1,0,'p'}, {"pc",1,0,'c'}, {"flush",1,0,'f'},
        {"measure",1,0,'m'}, {"initial-pairs",1,0,'a'}, {"loads",1,0,'l'},
        {"pages",1,0,'g'}, {"offset",1,0,'d'}, {"probe-offset",1,0,'q'},
        {"rounds",1,0,'r'}, {"cpu",1,0,'u'}, {"gap",1,0,'w'},
        {"threshold-ns",1,0,'t'}, {"seed",1,0,'s'}, {"no-trigger",0,0,'n'},
        {"describe",0,0,'v'}, {"help",0,0,'h'},
        {"resident",1,0,'R'}, {"evict-kb",1,0,'E'}, {"skip-b",0,0,'S'},
        {"shuffle-b",0,0,'J'}, {"b-limit",1,0,'L'}, {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", args, NULL)) != -1) {
        if(c=='h') { usage(); exit(0); }
        if(c=='n') { o->no_trigger=1; continue; }
        if(c=='v') { o->describe=1; continue; }
        if(c=='S') { o->skip_b=1; continue; }
        if(c=='J') { o->shuffle_b=1; continue; }
        if(c=='R') { o->resident=optarg; continue; }
        if(c=='p') { o->pattern=optarg; continue; }
        if(c=='c') { o->pc=optarg; continue; }
        if(c=='f') { o->flush=optarg; continue; }
        if(c=='m') { o->measure=optarg; continue; }
        if(c=='?' || !optarg || *optarg=='-') return -1;
        char *end; errno=0;
        unsigned long long v=strtoull(optarg,&end,0);
        if(errno || end==optarg || *end || v>SIZE_MAX) return -1;
        switch(c) {
        case 'a': o->initial=v; break; case 'l': o->loads=v; break;
        case 'g': o->pages=v; break; case 'd': o->offset=v; break;
        case 'q': o->probe_offset=v; break; case 'r': o->rounds=v; break;
        case 'u': o->cpu=v; break; case 'w': o->gap=v; break;
        case 't': o->threshold=v; break; case 's': o->seed=v; break;
        case 'E': o->evict_kb=v; break;
        case 'L': o->b_limit=v; break;
        default: return -1;
        }
    }
    if(!o->probe_offset) o->probe_offset=o->offset;
    return optind!=argc || !o->offset || o->offset>=LINES ||
        o->probe_offset>=LINES || !o->pages || o->pages>REGION_PAGES ||
        !o->rounds || o->rounds>100000 || o->cpu>=CPU_SETSIZE ||
        o->initial>REGION_PAGES || o->loads>8192 || o->loads%2 ||
        (!strcmp(o->pattern,"stride") && o->loads*o->offset>REGION_PAGES*LINES) ||
        o->gap>100000 || !o->threshold ||
        !oneof(o->pattern,"pair","stride","chain","random") ||
        !oneof(o->pc,"single","rotate","random","single") ||
        !oneof(o->flush,"batch","pair","each","fence") ||
        o->evict_kb>1024 ||
        (o->b_limit!=SIZE_MAX && (o->b_limit>o->loads || o->b_limit%2)) ||
        (!oneof(o->resident,"cold","demand","swl1","swl2") &&
         strcmp(o->resident,"hw-next") && strcmp(o->resident,"hw-stream") &&
         strcmp(o->resident,"hw-offset") && strcmp(o->resident,"hw-offset-consumed") &&
         strcmp(o->resident,"hw-offset-demand")) ||
        (!oneof(o->measure,"cache","pmu","timing","demand") &&
         strcmp(o->measure,"access") && strcmp(o->measure,"prefetch") &&
         strcmp(o->measure,"supply")) ? -1:0;
}
static int demand_open(uint64_t event,int group)
{
    struct perf_event_attr attr={0};
    attr.size=sizeof(attr); attr.type=PERF_TYPE_RAW; attr.config=event;
    attr.disabled=1; attr.exclude_kernel=1; attr.exclude_hv=1;
    attr.pinned=group<0;
    attr.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|
                     PERF_FORMAT_TOTAL_TIME_RUNNING;
    return syscall(__NR_perf_event_open,&attr,0,-1,group,0);
}
static void perf_start(int fd)
{
    if(ioctl(fd,PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP) ||
       ioctl(fd,PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP)) {
        perror("perf enable"); exit(1);
    }
}
static void perf_stop(int fd,uint64_t *c0,uint64_t *c1)
{
    struct { uint64_t count,enabled,running,value[2]; } result;
    if(ioctl(fd,PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP) ||
       read(fd,&result,sizeof(result))!=sizeof(result) || result.count!=2 ||
       !result.running || result.running!=result.enabled) {
        fputs("PMU unavailable or multiplexed; no valid result\n",stderr); exit(1);
    }
    *c0+=result.value[0]; *c1+=result.value[1];
}
static void build(struct entry *seq, size_t count, uint8_t *base,
                  const struct opts *o, uint64_t seed, int initial)
{
    size_t page_order[REGION_PAGES];
    uint64_t pc_seed=seed ^ 0xda942042e4dd58b5ULL;
    size_t pages=initial?REGION_PAGES:o->pages;
    for(size_t i=0;i<pages;i++) page_order[i]=i;
    for(size_t i=pages-1;i>0;i--) {
        size_t j=rng_next(&seed)%(i+1), v=page_order[i];
        page_order[i]=page_order[j]; page_order[j]=v;
    }
    size_t start=0;
    size_t chain_len=1+(LINES-1)/o->offset;
    for(size_t i=0;i<count;i++) {
        size_t page, line;
        if(initial || !strcmp(o->pattern,"pair")) {
            if(!(i%2)) start=rng_next(&seed)%(LINES-o->offset);
            page=page_order[(i/2)%pages]; line=start+(i%2)*o->offset;
        } else if(!strcmp(o->pattern,"stride")) {
            page=(i*o->offset)/LINES; line=(i*o->offset)%LINES;
        } else if(!strcmp(o->pattern,"chain")) {
            page=page_order[(i/chain_len)%pages]; line=(i%chain_len)*o->offset;
        } else {
            page=page_order[i%pages]; line=rng_next(&seed)%LINES;
        }
        seq[i].address=base+page*PAGE_SIZE+line*LINE_SIZE;
        size_t pc=0;
        if(initial || !strcmp(o->pc,"rotate")) pc=(i*5)%PCS;
        else if(!strcmp(o->pc,"random")) pc=rng_next(&pc_seed)%PCS;
        seq[i].load=pcs[pc];
    }
}
static void preflush(struct entry *seq,size_t count)
{ for(size_t i=0;i<count;i++) flush(seq[i].address); mfence(); }
__attribute__((noinline,noclone)) static void supply_load(void *p)
{ asm volatile("ldr x18, [%0]" :: "r"(p) : "x18","memory"); }
static void prepare_resident(struct entry *seq,size_t count,const struct opts *o,uint64_t seed,uint8_t *region)
{
    if(!strcmp(o->resident,"cold")) return;
    size_t *order=malloc((count+1)*sizeof(*order));
    uint8_t *is_target=calloc(REGION_PAGES*LINES,1);
    if(!order || !is_target) { perror("prepare order"); exit(1); }
    for(size_t i=0;i<count;i++) is_target[((uint8_t *)seq[i].address-region)/LINE_SIZE]=1;
    if(!strncmp(o->resident,"hw-offset",9)) {
        struct opts source=*o; source.offset=8;
        struct entry *training=calloc(3000,sizeof(*training));
        if(!training) exit(1);
        build(training,3000,region-REGION_PAGES*PAGE_SIZE,&source,seed^0x8899,1);
        cpp_rctx();
        for(size_t i=0;i<3000;i+=2) {
            flush(training[i].address); flush(training[i+1].address); mfence();
            training[i].load(training[i].address); delay(100);
            training[i+1].load(training[i+1].address); delay(100);
        }
        mfence(); free(training);
    }
    for(size_t i=0;i<count;i++) order[i]=i;
    for(size_t i=count;i>1;i--) {
        size_t j=rng_next(&seed)%i, v=order[i-1]; order[i-1]=order[j]; order[j]=v;
    }
    /* Shuffled suppliers avoid presenting the deliberate X -> X+D order. */
    for(size_t i=0;i<count;i++) {
        void *p=seq[order[i]].address;
        if(!strcmp(o->resident,"demand") || !strcmp(o->resident,"hw-offset-demand")) supply_load(p);
        else if(!strcmp(o->resident,"swl1"))
            asm volatile("prfm pldl1keep, [%0]" :: "r"(p) : "memory");
        else if(!strcmp(o->resident,"swl2"))
            asm volatile("prfm pldl2keep, [%0]" :: "r"(p) : "memory");
        else {
            if(!strncmp(o->resident,"hw-offset",9)) {
                uint8_t *q=(uint8_t *)p-8*LINE_SIZE;
                if(q<region || !is_target[(q-region)/LINE_SIZE]) {
                    flush(q); mfence(); supply_load(q); delay(1000);
                }
                continue;
            }
            size_t prefix=!strcmp(o->resident,"hw-stream")?8:1;
            for(size_t k=prefix;k>0;k--) {
                uint8_t *q=(uint8_t *)p-k*LINE_SIZE;
                /* Never demand-load a B target while attempting HW supply. */
                if(q>=region && is_target[(q-region)/LINE_SIZE]) continue;
                supply_load(q); delay(100);
            }
            delay(1000);
        }
        delay(100);
    }
    mfence(); delay(4000);
    if(!strcmp(o->resident,"hw-offset-consumed")) {
        for(size_t i=0;i<count;i++) { supply_load(seq[order[i]].address); delay(100); }
        mfence();
    }
    free(order); free(is_target);
}
static void evict_l1(uint8_t *scratch,size_t kb)
{
    size_t lines=kb*1024/LINE_SIZE;
    /* Linked random order removes the predictable stride of the eviction stream. */
    volatile uint8_t *p=scratch;
    for(size_t i=0;i<lines;i++) p=*(volatile uint8_t * volatile *)p;
    asm volatile("" :: "r"(p) : "memory"); mfence();
}
static void run(struct entry *seq,size_t count,const char *fl,size_t gap,
                int timed,uint64_t *elapsed,uint64_t *hits,size_t threshold)
{
    for(size_t i=0;i<count;i++) {
        if(!strcmp(fl,"each")) { flush(seq[i].address); mfence(); }
        else if(!strcmp(fl,"pair") && !(i%2)) {
            flush(seq[i].address); flush(seq[i+1].address); mfence();
        } else if(!strcmp(fl,"fence")) mfence();
        uint64_t a=0;
        if(timed) a=ticks();
        seq[i].load(seq[i].address);
        if(timed) { uint64_t dt=ticks()-a; *elapsed+=dt; *hits+=ns(dt)<=threshold; }
        delay(gap);
    }
    mfence();
}
int main(int argc,char **argv)
{
    struct opts o={"pair","single","pair","cache",300,600,4096,16,0,30,6,100,80,1,0,0,"cold",0,0,0,SIZE_MAX};
    if(parse(argc,argv,&o)) { usage(); return 2; }
    size_t executed=o.skip_b?0:o.b_limit==SIZE_MAX?o.loads:o.b_limit;
    if(o.describe) {
        uint8_t *region=malloc(REGION_PAGES*PAGE_SIZE);
        struct entry *seq=calloc(o.loads+1,sizeof(*seq));
        if(!region || !seq) return 1;
        build(seq,o.loads,region,&o,o.seed+37,0);
        uint64_t hash=14695981039346656037ULL;
        size_t delta_count=0; unsigned pc_mask=0;
        for(size_t i=0;i<o.loads;i++) {
            size_t position=(uint8_t *)seq[i].address-region;
            if(position>=REGION_PAGES*PAGE_SIZE) return 1;
            hash=(hash^position)*1099511628211ULL;
            if(i && (uint8_t *)seq[i].address-(uint8_t *)seq[i-1].address==
                    (ptrdiff_t)(o.offset*LINE_SIZE)) delta_count++;
            for(size_t j=0;j<PCS;j++) if(seq[i].load==pcs[j]) pc_mask|=1u<<j;
        }
        printf("initial_loads=%zu stage_b_loads=%zu adjacent_d=%zu address_hash=%llu pc_mask=%u executed_loads=%zu\n",
               2*o.initial,o.loads,delta_count,(unsigned long long)hash,pc_mask,executed);
        free(seq); free(region); return 0;
    }
    cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(o.cpu,&cpus);
    if(sched_setaffinity(0,sizeof(cpus),&cpus)) { perror("affinity"); return 1; }
    if(sysconf(_SC_PAGESIZE)!=PAGE_SIZE) { fputs("Requires 4KiB pages\n",stderr); return 1; }
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    const size_t bytes=(2*REGION_PAGES+256)*PAGE_SIZE+1024*1024;
    uint8_t *mem=mmap(NULL,bytes,PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0);
    if(mem==MAP_FAILED) { perror("mmap"); return 1; }
    /* Explicit writes ensure private backing, not shared zero-page mappings. */
    for(size_t i=0;i<bytes;i+=PAGE_SIZE) *(volatile uint8_t *)(mem+i)=1;
    uint8_t *scratch=mem+(2*REGION_PAGES+256)*PAGE_SIZE;
    size_t scratch_lines=o.evict_kb*1024/LINE_SIZE;
    size_t *scratch_order=malloc((scratch_lines+1)*sizeof(*scratch_order));
    if(!scratch_order) return 1;
    uint64_t evict_seed=o.seed^0x8199;
    for(size_t i=0;i<scratch_lines;i++) scratch_order[i]=i;
    for(size_t i=scratch_lines;i>1;i--) {
        size_t j=rng_next(&evict_seed)%i,v=scratch_order[i-1];
        scratch_order[i-1]=scratch_order[j]; scratch_order[j]=v;
    }
    for(size_t i=0;i<scratch_lines;i++)
        *(uint8_t **)(scratch+scratch_order[i]*LINE_SIZE)=scratch+scratch_order[(i+1)%scratch_lines]*LINE_SIZE;
    free(scratch_order);
    struct entry *a=calloc(2*o.initial+1,sizeof(*a)), *b=calloc(o.loads+1,sizeof(*b));
    if(!a || !b) { perror("calloc"); return 1; }
    int pmu=!strcmp(o.measure,"pmu"), timed=!strcmp(o.measure,"timing"), fd=-1;
    int demand=!strcmp(o.measure,"demand"), access_measure=!strcmp(o.measure,"access");
    int supply=!strcmp(o.measure,"supply");
    int prefetch=!strcmp(o.measure,"prefetch") || supply, perf=demand||access_measure||prefetch;
    int demand_fd=-1, demand_child=-1;
    uint64_t event0=access_measure?0x8148:prefetch?0x8154:0x8144;
    uint64_t event1=prefetch?0x8155:0x814c;
    if(pmu) {
        fd=open(PREFETCH_PMU_DEVICE_PATH,O_RDONLY|O_CLOEXEC);
        if(fd<0 || ioctl(fd,PREFETCH_PMU_IOC_REARM)) { perror("PMU rearm"); return 1; }
    }
    if(perf) {
        if(access("/sys/module/prefetch_pmu_event",F_OK)==0) {
            fputs("Unload prefetch_pmu_event before using perf; shared PMU ownership conflict\n",stderr); return 1;
        }
        demand_fd=demand_open(event0,-1);
        demand_child=demand_open(event1,demand_fd);
        if(demand_fd<0 || demand_child<0) { perror("demand PMU open"); return 1; }
    }
    uint64_t hits=0,total=0,elapsed=0,train_ticks=0,train_hits=0,l1=0,l2=0;
    uint64_t l1miss=0,l2miss=0;
    for(size_t round=0;round<o.rounds;round++) {
        uint64_t seed=o.seed+977*round;
        build(a,2*o.initial,mem,&o,seed,1);
        build(b,o.loads,mem+REGION_PAGES*PAGE_SIZE,&o,seed+37,0);
        void *triggers[BATCH], *targets[BATCH];
        for(size_t i=0;i<BATCH;i++) {
            size_t page=2*REGION_PAGES+(i*167+round)%256;
            size_t line=rng_next(&seed)%(LINES-o.probe_offset);
            triggers[i]=mem+page*PAGE_SIZE+line*LINE_SIZE;
            targets[i]=(uint8_t *)triggers[i]+o.probe_offset*LINE_SIZE;
            flush(triggers[i]); flush(targets[i]);
        }
        preflush(a,2*o.initial); preflush(b,o.loads);
        if(supply) perf_start(demand_fd);
        prepare_resident(b,o.loads,&o,seed^0x3456,mem+REGION_PAGES*PAGE_SIZE);
        if(supply) perf_stop(demand_fd,&l1miss,&l2miss);
        if(o.shuffle_b) {
            uint64_t shuffle_seed=seed^0x1729;
            for(size_t i=o.loads;i>1;i--) {
                size_t j=rng_next(&shuffle_seed)%i;
                struct entry v=b[i-1]; b[i-1]=b[j]; b[j]=v;
            }
        }
        cpp_rctx();
        run(a,2*o.initial,"pair",o.gap,0,&train_ticks,&train_hits,o.threshold);
        evict_l1(scratch,o.evict_kb);
        struct prefetch_pmu_snapshot before={0}, after={0};
        if(pmu) before=prefetch_pmu_read();
        if(perf && !supply) perf_start(demand_fd);
        run(b,executed,o.flush,o.gap,timed,&train_ticks,&train_hits,o.threshold);
        if(pmu) {
            after=prefetch_pmu_read();
            l1+=prefetch_pmu_delta(after.l1_hwprf,before.l1_hwprf);
            l2+=prefetch_pmu_delta(after.l2_prefetch,before.l2_prefetch);
        }
        if(perf && !supply) perf_stop(demand_fd,&l1miss,&l2miss);
        /* Cache pass alone probes endpoints. PMU/timing never probe them. */
        if(!pmu && !timed && !perf) {
            if(!o.no_trigger) for(size_t i=0;i<BATCH;i++) trigger(triggers[i]);
            mfence(); delay(2000);
            for(size_t j=0;j<BATCH;j++) {
                size_t i=(j*17+round)%BATCH;
                uint64_t start=ticks(); mLoad_inline(targets[i]);
                uint64_t dt=ticks()-start;
                hits+=ns(dt)<=o.threshold; total++; elapsed+=dt;
            }
        }
    }
    printf("pattern\tpc\tflush\tmeasure\tinitial_pairs\tloads\tpages\toffset\tprobe_offset\tseed\tno_trigger\tprobes\thits\thit_pct\tavg_ns\ttrain_hit_pct\ttrain_avg_ns\tl1_events\tl2_events\tl1_per_load\tl2_per_load\tl1_demand_miss\tl2_demand_miss\tl1_miss_per_load\tl2_miss_per_load\tresident\tevict_kb\tskip_b\tevent0\tevent1\tevent0_count\tevent1_count\tevent0_per_load\tevent1_per_load\texecuted_loads\tshuffle_b\n");
    double n=(double)o.rounds*(supply?o.loads:executed);
    printf("%s\t%s\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%llu\t%d\t%llu\t%llu\t%.4f\t%.3f\t%.4f\t%.3f\t%llu\t%llu\t%.6f\t%.6f\t%llu\t%llu\t%.6f\t%.6f\t%s\t%zu\t%d\t0x%llx\t0x%llx\t%llu\t%llu\t%.6f\t%.6f\t%zu\t%d\n",
           o.pattern,o.pc,o.flush,o.measure,o.initial,o.loads,o.pages,o.offset,o.probe_offset,
           (unsigned long long)o.seed,o.no_trigger,(unsigned long long)total,
           (unsigned long long)hits,total?100.0*hits/total:0,total?ns(elapsed)/total:0,
           n?100.0*train_hits/n:0,n?ns(train_ticks)/n:0,
           (unsigned long long)l1,(unsigned long long)l2,n?l1/n:0,n?l2/n:0,
           (unsigned long long)(demand?l1miss:0),(unsigned long long)(demand?l2miss:0),
           n&&demand?l1miss/n:0,n&&demand?l2miss/n:0,o.resident,o.evict_kb,o.skip_b,
           (unsigned long long)(perf?event0:0),(unsigned long long)(perf?event1:0),
           (unsigned long long)l1miss,(unsigned long long)l2miss,n?l1miss/n:0,n?l2miss/n:0,
           executed,o.shuffle_b);
    if(fd>=0) close(fd);
    if(demand_child>=0) close(demand_child);
    if(demand_fd>=0) close(demand_fd);
    free(a); free(b); munmap(mem,bytes); return 0;
}
