
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "../store-stride-prefetcher/until.h"
#ifndef ROUNDS
#define ROUNDS 500
#endif
#ifndef TRAIN_REGIONS
#define TRAIN_REGIONS 32
#endif
#ifndef REGION_POOL_PAGES
#define REGION_POOL_PAGES 8191
#endif
#ifndef CODE_SIZE
#define CODE_SIZE 262144
#endif
#ifndef CACHE_HIT_THRESHOLD_TICKS
#define CACHE_HIT_THRESHOLD_TICKS 100
#endif
#ifndef MAX_GROUP_COUNT
#define MAX_GROUP_COUNT 64
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif
#ifndef PREFETCH_WAIT_NS
#define PREFETCH_WAIT_NS 10000
#endif
#define LINES_PER_REGION (PAGE_SIZE / LINE_SIZE)
#define REGION_POOL_SIZE ((size_t)REGION_POOL_PAGES * PAGE_SIZE)
static const int footprint_offsets[] = {0,15,2,23,9,-3,11};
static uint8_t *region_pool,*code_pool;
typedef void (*stub_fn_t)(void *);
static void flush_page(uint8_t *page){ for(size_t line=0; line<LINES_PER_REGION; line++) flush(page+line*LINE_SIZE); mfence(); }
static size_t page_index(uint64_t round,size_t position){ return (size_t)((round*151u+position*73u)%REGION_POOL_PAGES); }
static inline __attribute__((always_inline)) void wait_for_prefetch(void){ for(int i=0;i<PREFETCH_WAIT_NOPS;i++) nop(); if(PREFETCH_WAIT_NS>0){ struct timespec ts={.tv_sec=0,.tv_nsec=PREFETCH_WAIT_NS}; (void)nanosleep(&ts,NULL);} }
static int init_region_pool(void){ region_pool=mmap(NULL,REGION_POOL_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0); if(region_pool==MAP_FAILED){fprintf(stderr,"mmap data failed: %s\n",strerror(errno)); return -1;} memset(region_pool,0xff,REGION_POOL_SIZE); for(size_t off=0;off<REGION_POOL_SIZE;off+=LINE_SIZE) mLoad_inline(region_pool+off); mfence(); return 0; }
static int init_code_pool(void){ code_pool=mmap(NULL,CODE_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(code_pool==MAP_FAILED){fprintf(stderr,"mmap code failed: %s\n",strerror(errno)); return -1;} uint32_t *p=(uint32_t*)code_pool; for(size_t i=0;i<CODE_SIZE/4;i++) p[i]=0xd503201f; for(size_t off=0;off+8<=CODE_SIZE;off+=8){ uint32_t *q=(uint32_t*)(code_pool+off); q[0]=0x39400001; q[1]=0xd65f03c0; } __builtin___clear_cache((char*)code_pool,(char*)code_pool+CODE_SIZE); if(mprotect(code_pool,CODE_SIZE,PROT_READ|PROT_EXEC)!=0){fprintf(stderr,"mprotect code failed: %s\n",strerror(errno)); return -1;} return 0; }
static void cleanup(void){ if(region_pool&&region_pool!=MAP_FAILED) munmap(region_pool,REGION_POOL_SIZE); if(code_pool&&code_pool!=MAP_FAILED) munmap(code_pool,CODE_SIZE); }
static int valid_offset(int off){ return off>=0 && off+8<=CODE_SIZE && (off%8)==0; }
static stub_fn_t fn_at(int off){ return (stub_fn_t)(void*)(code_pool+off); }
static void train_shifted(uint64_t round,size_t start_pos,int base_line,stub_fn_t fn){ for(size_t region=0; region<TRAIN_REGIONS; region++){ uint8_t *page=region_pool+page_index(round,start_pos+region)*PAGE_SIZE; flush_page(page); for(size_t i=0;i<sizeof(footprint_offsets)/sizeof(footprint_offsets[0]);i++){ int line=base_line+footprint_offsets[i]; fn(page+(size_t)line*LINE_SIZE); nops(); } mfence(); } }
static void run_case(const char *name,int do_group,int a_off,int stride,int count,int addend,int base_line,int probe_line,uint64_t *sum,uint64_t *hits){
    *sum=0; *hits=0;
    stub_fn_t a_fn=fn_at(a_off);
    for(uint64_t round=0; round<ROUNDS; round++){
        cpp_rctx();
        train_shifted(round,0,base_line,a_fn);
        if(do_group){
            for(int k=1;k<=count;k++){
                int off=a_off+addend+k*stride;
                if(!valid_offset(off)) break;
                train_shifted(round,(size_t)k*(TRAIN_REGIONS+1),base_line,fn_at(off));
            }
        }
        uint8_t *target=region_pool+page_index(round,TRAIN_REGIONS)*PAGE_SIZE;
        flush_page(target);
        a_fn(target+(size_t)base_line*LINE_SIZE);
        nops();
        a_fn(target+(size_t)(base_line+15)*LINE_SIZE);
        nops();
        wait_for_prefetch();
        uint8_t *probe_addr=target+(size_t)probe_line*LINE_SIZE;
        uint64_t start=timestamp();
        mLoad_inline(probe_addr);
        uint64_t elapsed=timestamp()-start;
        *sum+=elapsed;
        *hits+=(elapsed<=CACHE_HIT_THRESHOLD_TICKS);
        flush(probe_addr);
    }
    printf("# %s avg=%llu hits=%llu/%d\n",name,(unsigned long long)(*sum/ROUNDS),(unsigned long long)*hits,ROUNDS);
}
int main(int argc,char **argv){ if(argc!=5 && argc!=6){fprintf(stderr,"usage: %s <a_pc_offset> <stride_bytes> <count> <probe_delta_from_second> [addend_bytes]\n",argv[0]); return 2;} int a_off=atoi(argv[1]), stride=atoi(argv[2]), count=atoi(argv[3]), probe_delta=atoi(argv[4]); int addend=(argc==6)?atoi(argv[5]):0; const int base_line=20, probe_line=35+probe_delta; if(!valid_offset(a_off)||!valid_offset(a_off+addend+stride)||stride<=0||(stride%8)||count<0||count>MAX_GROUP_COUNT||probe_line<0||probe_line>=(int)LINES_PER_REGION||probe_line==base_line||probe_line==35||(size_t)(MAX_GROUP_COUNT+3)*(TRAIN_REGIONS+1)>=REGION_POOL_PAGES){fprintf(stderr,"invalid parameters\n"); return 2;} if(init_region_pool()!=0||init_code_pool()!=0) return 1; uint64_t bs,bh,gs,gh; run_case("baseline_a_only",0,a_off,stride,count,addend,base_line,probe_line,&bs,&bh); run_case("after_group",1,a_off,stride,count,addend,base_line,probe_line,&gs,&gh); printf("RESULT a_off=%d stride=%d count=%d addend=%d a_pc=0x%llx first_group_pc=0x%llx probe_delta=%d baseline_avg=%llu after_group_avg=%llu baseline_hit_rate=%.4f after_group_hit_rate=%.4f\n",a_off,stride,count,addend,(unsigned long long)(uintptr_t)fn_at(a_off),(unsigned long long)(uintptr_t)fn_at(a_off+addend+stride),probe_delta,(unsigned long long)(bs/ROUNDS),(unsigned long long)(gs/ROUNDS),(double)bh/(double)ROUNDS,(double)gh/(double)ROUNDS); cleanup(); return 0; }
