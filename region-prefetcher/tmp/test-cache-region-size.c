
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
#define ROUNDS 2000
#endif
#ifndef TRAIN_REGIONS
#define TRAIN_REGIONS 32
#endif
#ifndef REGION_POOL_PAGES
#define REGION_POOL_PAGES 4099
#endif
#ifndef CACHE_HIT_THRESHOLD_TICKS
#define CACHE_HIT_THRESHOLD_TICKS 100
#endif
#ifndef PREFETCH_WAIT_NOPS
#define PREFETCH_WAIT_NOPS 1000
#endif
#ifndef PREFETCH_WAIT_NS
#define PREFETCH_WAIT_NS 10000
#endif
#define LINES_PER_REGION (PAGE_SIZE / LINE_SIZE)
#define REGION_POOL_SIZE ((size_t)REGION_POOL_PAGES * PAGE_SIZE)
static const int footprint_offsets[] = {0, 15, 2, 23, 9, -3, 11};
static uint8_t *region_pool;
__attribute__((noinline)) static void sms_load(void *addr){ asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0"); }
__attribute__((noinline)) static void control_load(void *addr){ asm volatile("ldrb w0, [%0]\n\t" :: "r"(addr) : "memory", "w0"); }
static void flush_page(uint8_t *page){ for(size_t line=0; line<LINES_PER_REGION; line++) flush(page+line*LINE_SIZE); mfence(); }
static size_t page_index(uint64_t round, size_t position){ return (size_t)((round*151u + position*73u) % REGION_POOL_PAGES); }
static inline __attribute__((always_inline)) void wait_for_prefetch(void){ for(int i=0;i<PREFETCH_WAIT_NOPS;i++) nop(); if(PREFETCH_WAIT_NS>0){ struct timespec ts={.tv_sec=0,.tv_nsec=PREFETCH_WAIT_NS}; (void)nanosleep(&ts,NULL);} }
static int valid_shifted_footprint(int base_line){ for(size_t i=0;i<sizeof(footprint_offsets)/sizeof(footprint_offsets[0]);i++){ int line=base_line+footprint_offsets[i]; if(line<0 || line>=(int)LINES_PER_REGION) return 0;} return 1; }
static int init_pool(void){ region_pool=mmap(NULL,REGION_POOL_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0); if(region_pool==MAP_FAILED){fprintf(stderr,"mmap failed: %s\n",strerror(errno)); return -1;} memset(region_pool,0xff,REGION_POOL_SIZE); for(size_t off=0; off<REGION_POOL_SIZE; off+=LINE_SIZE) mLoad_inline(region_pool+off); mfence(); return 0; }
static void cleanup_pool(void){ if(region_pool && region_pool!=MAP_FAILED) munmap(region_pool, REGION_POOL_SIZE); }
static void train_shifted(uint64_t round, int base_line){ for(size_t region=0; region<TRAIN_REGIONS; region++){ uint8_t *page=region_pool+page_index(round,region)*PAGE_SIZE; flush_page(page); for(size_t i=0;i<sizeof(footprint_offsets)/sizeof(footprint_offsets[0]);i++){ int line=base_line+footprint_offsets[i]; sms_load(page+(size_t)line*LINE_SIZE); nops(); } mfence(); } }
typedef void (*load_fn_t)(void *);
static void run_case(const char *name, load_fn_t trigger, int base_line, int probe_line, uint64_t *sum, uint64_t *hits){ *sum=0; *hits=0; for(uint64_t round=0; round<ROUNDS; round++){ cpp_rctx(); train_shifted(round,base_line); uint8_t *target=region_pool+page_index(round,TRAIN_REGIONS)*PAGE_SIZE; flush_page(target); trigger(target+(size_t)base_line*LINE_SIZE); nops(); trigger(target+(size_t)(base_line+15)*LINE_SIZE); nops(); wait_for_prefetch(); uint8_t *probe_addr=target+(size_t)probe_line*LINE_SIZE; uint64_t start=timestamp(); mLoad_inline(probe_addr); uint64_t elapsed=timestamp()-start; *sum += elapsed; *hits += (elapsed <= CACHE_HIT_THRESHOLD_TICKS); flush(probe_addr);} printf("# %s avg=%llu hits=%llu/%d\n",name,(unsigned long long)(*sum/ROUNDS),(unsigned long long)*hits,ROUNDS); }
int main(int argc, char **argv){ if(argc!=3){fprintf(stderr,"usage: %s <base_line> <probe_line>\n",argv[0]); return 2;} int base_line=atoi(argv[1]); int probe_line=atoi(argv[2]); if(!valid_shifted_footprint(base_line) || probe_line<0 || probe_line>=(int)LINES_PER_REGION || probe_line==base_line || probe_line==base_line+15){ fprintf(stderr,"invalid base/probe\n"); return 2;} if(init_pool()!=0) return 1; uint64_t ts,th,cs,ch; run_case("trained",sms_load,base_line,probe_line,&ts,&th); run_case("control",control_load,base_line,probe_line,&cs,&ch); printf("RESULT base=%d second_trigger=%d probe=%d delta_from_second=%d trained_avg=%llu control_avg=%llu trained_hit_rate=%.4f control_hit_rate=%.4f\n",base_line,base_line+15,probe_line,probe_line-(base_line+15),(unsigned long long)(ts/ROUNDS),(unsigned long long)(cs/ROUNDS),(double)th/(double)ROUNDS,(double)ch/(double)ROUNDS); cleanup_pool(); return 0; }
