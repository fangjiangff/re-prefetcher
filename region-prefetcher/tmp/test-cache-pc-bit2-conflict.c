
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
#define CODE_SIZE 65536
#endif
#ifndef CACHE_HIT_THRESHOLD_TICKS
#define CACHE_HIT_THRESHOLD_TICKS 100
#endif
#define LINES_PER_REGION (PAGE_SIZE / LINE_SIZE)
#define REGION_POOL_SIZE ((size_t)REGION_POOL_PAGES * PAGE_SIZE)
static const int footprint_offsets[]={0,15,2,23,9,-3,11};
static uint8_t *region_pool,*code_pool; typedef void(*fn_t)(void*);
static void flush_page(uint8_t*p){for(size_t l=0;l<LINES_PER_REGION;l++)flush(p+l*LINE_SIZE);mfence();}
static size_t page_index(uint64_t r,size_t p){return(size_t)((r*151u+p*73u)%REGION_POOL_PAGES);} 
static void wait_for_prefetch(void){for(int i=0;i<1000;i++)nop(); struct timespec ts={.tv_sec=0,.tv_nsec=10000}; nanosleep(&ts,NULL);} 
static int init_region(void){region_pool=mmap(NULL,REGION_POOL_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_POPULATE,-1,0); if(region_pool==MAP_FAILED){perror("mmap data");return-1;} memset(region_pool,0xff,REGION_POOL_SIZE); for(size_t o=0;o<REGION_POOL_SIZE;o+=LINE_SIZE)mLoad_inline(region_pool+o); mfence(); return 0;}
static int init_code(void){code_pool=mmap(NULL,CODE_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); if(code_pool==MAP_FAILED){perror("mmap code");return-1;} uint32_t*p=(uint32_t*)code_pool; for(size_t i=0;i<CODE_SIZE/4;i++)p[i]=0xd503201f; uint32_t*a=(uint32_t*)code_pool; a[0]=0x39400001; a[1]=0xd65f03c0; for(size_t off=64; off+12<=CODE_SIZE; off+=64){uint32_t*q=(uint32_t*)(code_pool+off); q[0]=0xd503201f; q[1]=0x39400001; q[2]=0xd65f03c0;} __builtin___clear_cache((char*)code_pool,(char*)code_pool+CODE_SIZE); if(mprotect(code_pool,CODE_SIZE,PROT_READ|PROT_EXEC)){perror("mprotect");return-1;} return 0;}
static void cleanup(void){if(region_pool&&region_pool!=MAP_FAILED)munmap(region_pool,REGION_POOL_SIZE); if(code_pool&&code_pool!=MAP_FAILED)munmap(code_pool,CODE_SIZE);} 
static fn_t fn_at(size_t off){return(fn_t)(void*)(code_pool+off);} 
static void train(uint64_t r,size_t start,int base,fn_t f){for(size_t reg=0;reg<TRAIN_REGIONS;reg++){uint8_t*page=region_pool+page_index(r,start+reg)*PAGE_SIZE; flush_page(page); for(size_t i=0;i<sizeof(footprint_offsets)/sizeof(footprint_offsets[0]);i++){int line=base+footprint_offsets[i]; f(page+(size_t)line*LINE_SIZE); nops();} mfence();}}
static void run(int do_group,int count,uint64_t*sum,uint64_t*hits){*sum=0;*hits=0; const int base=20, probe=37; fn_t a=fn_at(0); for(uint64_t r=0;r<ROUNDS;r++){cpp_rctx(); train(r,0,base,a); if(do_group){for(int k=1;k<=count;k++)train(r,(size_t)k*(TRAIN_REGIONS+1),base,fn_at((size_t)k*64));} uint8_t*t=region_pool+page_index(r,TRAIN_REGIONS)*PAGE_SIZE; flush_page(t); a(t+base*LINE_SIZE); nops(); a(t+(base+15)*LINE_SIZE); nops(); wait_for_prefetch(); uint8_t*pa=t+probe*LINE_SIZE; uint64_t st=timestamp(); mLoad_inline(pa); uint64_t el=timestamp()-st; *sum+=el; *hits+=(el<=CACHE_HIT_THRESHOLD_TICKS); flush(pa);}}
int main(int argc,char**argv){int count=argc>1?atoi(argv[1]):12; if(init_region()||init_code())return 1; uint64_t bs,bh,as,ah; run(0,count,&bs,&bh); run(1,count,&as,&ah); printf("RESULT bit=2 count=%d a_load_pc=0x%llx first_b_entry=0x%llx first_b_load_pc=0x%llx baseline_avg=%llu after_avg=%llu baseline_hit_rate=%.4f after_hit_rate=%.4f\n",count,(unsigned long long)(uintptr_t)(code_pool),(unsigned long long)(uintptr_t)(code_pool+64),(unsigned long long)(uintptr_t)(code_pool+68),(unsigned long long)(bs/ROUNDS),(unsigned long long)(as/ROUNDS),(double)bh/ROUNDS,(double)ah/ROUNDS); cleanup(); return 0;}
