#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
// #include "victim.h" 

#ifndef __aarch64__
#error "store_stride_poc uses AArch64 DC CIVAC, DSB/ISB, and STRB/LDRB."
#endif

#ifndef CPU_ID
#define CPU_ID 0
#endif
#define LINE_SIZE 64
#define PAGE_SIZE 4096
// #define Items 256
#ifndef Items
#define Items 10240
#endif
static inline void nop(void) {
    asm volatile("nop");
}
//mix index (),to ensure not to trigger a simple stride prefetcher in the CPU.
// int array_index[16] = {37, 192, 84, 231, 15, 126, 203, 58, 174, 9, 247, 101, 66, 219, 142, 30};
int array_index[256] = {1384, 217, 1746, 905, 63, 1297, 1881, 452,
1016, 733, 1549, 320, 1974, 118, 1672, 861,
1435, 29, 1264, 1991, 582, 1048, 301, 1793,
936, 1461, 705, 1918, 155, 1127, 487, 1329,
768, 1856, 244, 977, 1690, 604, 1518, 396,
1943, 81, 1215, 1739, 534, 1086, 199, 1607,
894, 136, 1822, 675, 1151, 348, 1426, 1899,
59, 1003, 1577, 821, 226, 1755, 646, 1238,
1870, 514, 961, 1473, 287, 1666, 1104, 35,
1984, 739, 1301, 470, 1837, 912, 257, 1542,
1196, 704, 1711, 103, 1935, 568, 1347, 808,
1499, 376, 1808, 621, 1073, 192, 1644, 984,
443, 1259, 187, 1776, 693, 1185, 21, 1526,
872, 1408, 331, 1962, 529, 1027, 1588, 753,
241, 1698, 1117, 410, 1906, 647, 1221, 84,
1459, 1817, 999, 271, 1631, 721, 1374, 497,
185, 1569, 928, 1987, 115, 1318, 566, 1741,
805, 151, 1888, 1009, 366, 142, 1658, 603,
1247, 913, 46, 1951, 783, 1095, 1703, 319,
1396, 682, 1865, 537, 234, 1591, 1199, 907,
75, 1335, 1768, 451, 101, 1947, 844, 1268,
579, 1486, 1929, 306, 1139, 618, 1719, 488,
863, 26, 1831, 1002, 154, 1412, 746, 1971,
391, 1283, 572, 1649, 959, 208, 1536, 897,
121, 1801, 436, 1079, 1892, 653, 1362, 14,
1995, 834, 1176, 263, 1451, 702, 1728, 951,
328, 1601, 1229, 64, 1879, 505, 1113, 779,
1490, 219, 1958, 642, 1309, 888, 33, 1679,
1018, 554, 1826, 293, 1189, 738, 1583, 970,
405, 1901, 626, 1354, 168, 1779, 856, 1122,
477, 1443, 1968, 711, 126, 1617, 998, 352,
1844, 589, 1206, 817, 1531, 40, 1733, 1081};

// int array_index[256] ={1279, 1188, 1654, 1704, 257, 198, 913, 1153,
//             500, 1722, 1803, 347, 1565, 780, 146, 197,
//             624, 1359, 1664, 1467, 1145, 699, 1992, 1949,
//             763, 1028, 590, 419, 1758, 895, 1747, 964,
//             1616, 1734, 767, 652, 928, 1978, 1255, 1055,
//             1835, 684, 1357, 736, 1231, 527, 1856, 1956,
//             1621, 361, 1681, 270, 346, 1932, 1209, 27,
//             334, 1955, 293, 812, 854, 312, 461, 451,
//             910, 1987, 1356, 811, 1793, 922, 1610, 1012,
//             1218, 290, 1210, 402, 730, 1265, 1636, 1260,
//             1198, 153, 307, 1938, 1545, 486, 585, 246,
//             1388, 1907, 465, 942, 1168, 487, 1597, 1641,
//             1219, 98, 266, 1625, 474, 200, 11, 804,
//             1014, 1589, 1620, 42, 677, 1420, 820, 1596,
//             1278, 1967, 1591, 1678, 1604, 1884, 635, 1020,
//             857, 447, 340, 1432, 351, 508, 955, 714,
//             655, 1562, 1163, 630, 1284, 1141, 559, 1002,
//             234, 1090, 181, 1849, 707, 544, 1891, 329,
//             970, 250, 106, 1323, 1628, 968, 1316, 29,
//             1147, 1773, 1502, 1135, 297, 1646, 881, 1220,
//             1796, 301, 1442, 961, 717, 548, 1629, 891,
//             774, 1894, 164, 934, 1203, 1537, 1933, 938,
//             385, 189, 797, 1766, 58, 1841, 731, 1075,
//             1718, 683, 946, 239, 1506, 1427, 196, 1631,
//             1558, 1251, 1661, 1019, 1536, 1831, 227, 511,
//             350, 1934, 1814, 1076, 697, 1122, 138, 673,
//             869, 967, 933, 1345, 90, 1184, 1285, 1822,
//             4, 1102, 564, 916, 669, 1355, 489, 859,
//             1571, 92, 905, 1500, 151, 890, 130, 353,
//             1586, 1474, 1462, 1051, 1064, 1557, 269, 679,
//             553, 1948, 1954, 1561, 1081, 1797, 9, 1695,
//             1880, 264, 380, 54, 637, 1034, 252, 1957};


#ifndef STRIDE_BYTES
#define STRIDE_BYTES 64
#endif

#ifndef TRAIN_STEP
#define TRAIN_STEP 10
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS 256
#endif

#ifndef DEFAULT_PROBE_INDEX
#define DEFAULT_PROBE_INDEX 0
#endif

#ifndef TRIGGER_START
#define TRIGGER_START 170
#endif

#ifndef TRIGGER_END
#define TRIGGER_END 195
#endif

#ifndef RANDOM_BUFFER_PAGES
#define RANDOM_BUFFER_PAGES 1024
#endif

#ifndef RANDOM_ACCESS_PCS
#define RANDOM_ACCESS_PCS 8
#endif

#ifndef ENABLE_CONTEXT_SWITCH_FLUSH
#define ENABLE_CONTEXT_SWITCH_FLUSH 0
#endif

#define TEST_MODE_WINDOW 0
#define TEST_MODE_NODE 1
#define TEST_MODE_DEPTH 2

#ifndef TEST_MODE
#define TEST_MODE TEST_MODE_WINDOW
#endif

#if TEST_MODE < TEST_MODE_WINDOW || TEST_MODE > TEST_MODE_DEPTH
#error "TEST_MODE must be TEST_MODE_WINDOW, TEST_MODE_NODE, or TEST_MODE_DEPTH"
#endif

#if RANDOM_ACCESS_PCS < 1 || RANDOM_ACCESS_PCS > 8
#error "RANDOM_ACCESS_PCS must be in the range 1..8"
#endif

#define RANDOM_BUFFER_SIZE (PAGE_SIZE * RANDOM_BUFFER_PAGES)
#define RANDOM_BUFFER_LINES (RANDOM_BUFFER_SIZE / LINE_SIZE)

static inline void mfence(void) {
    asm volatile("DSB SY\nISB" ::: "memory");
}

static inline void flush(void *addr) {
    asm volatile("DC CIVAC, %0" :: "r" (addr) : "memory");
}


#define _mStore(addr, val)                 \
asm volatile(                                      \
    "strb %w[value], [%[address]]\n\t"          \
    :                                              \
    : [address] "r" (addr), [value] "r" ((uint32_t)(val)) \
    : "memory")

#define _mLoad(addr) \
    asm volatile( \
        "ldrb w0, [%0]\n\t" \
        :: "r" (addr) \
        : "memory", "w0")

void mLoad(void* p){
    _mLoad(p);
}

void mStore(void* p, uint8_t val){
    _mStore(p, val);
}


static inline __attribute__((always_inline))
uint64_t timestamp(void)
{
    struct timespec t1;
    asm volatile("DSB SY" ::: "memory");
    asm volatile("ISB" ::: "memory");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    asm volatile("ISB" ::: "memory");
    asm volatile("DSB SY" ::: "memory");
    return t1.tv_sec * 1000ULL * 1000ULL * 1000ULL + t1.tv_nsec;
}



uint8_t array2[Items * LINE_SIZE] __attribute__((aligned(4096)));;

long long int latency_sum[Items] = {0};
int probe_count[Items] = {0};

uint8_t array1[100*LINE_SIZE]={0};
uint8_t array0[100*LINE_SIZE]={0};

#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

static uint8_t* dummy_buffer;
static uint8_t* random_buffer;
static volatile uint64_t random_sink;
static int cs_parent_to_child[2] = {-1, -1};
static int cs_child_to_parent[2] = {-1, -1};
static pid_t cs_child_pid = -1;

static void die_perror(const char *msg) {
    perror(msg);
    exit(1);
}

static void fullWrite(int fd, const void *buf, size_t size) {
    const uint8_t *p = (const uint8_t *)buf;

    while(size > 0) {
        ssize_t n = write(fd, p, size);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            die_perror("write");
        }
        p += n;
        size -= (size_t)n;
    }
}

static void fullRead(int fd, void *buf, size_t size) {
    uint8_t *p = (uint8_t *)buf;

    while(size > 0) {
        ssize_t n = read(fd, p, size);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            die_perror("read");
        }
        if(n == 0) {
            fprintf(stderr, "unexpected EOF during context-switch ping-pong\n");
            exit(1);
        }
        p += n;
        size -= (size_t)n;
    }
}

static void initContextSwitchHelper(void) {
#if ENABLE_CONTEXT_SWITCH_FLUSH
    if(pipe(cs_parent_to_child) != 0 || pipe(cs_child_to_parent) != 0) {
        die_perror("pipe");
    }

    cs_child_pid = fork();
    if(cs_child_pid < 0) {
        die_perror("fork");
    }

    if(cs_child_pid == 0) {
        uint8_t token;

        close(cs_parent_to_child[1]);
        close(cs_child_to_parent[0]);

        for(;;) {
            ssize_t n = read(cs_parent_to_child[0], &token, sizeof(token));
            if(n == 0) {
                _exit(0);
            }
            if(n < 0) {
                if(errno == EINTR) {
                    continue;
                }
                _exit(1);
            }
            fullWrite(cs_child_to_parent[1], &token, sizeof(token));
        }
    }

    close(cs_parent_to_child[0]);
    close(cs_child_to_parent[1]);
#endif
}

static void cleanupContextSwitchHelper(void) {
#if ENABLE_CONTEXT_SWITCH_FLUSH
    int status;

    if(cs_child_pid > 0) {
        close(cs_parent_to_child[1]);
        close(cs_child_to_parent[0]);
        waitpid(cs_child_pid, &status, 0);
        cs_child_pid = -1;
    }
#endif
}

static inline void forceContextSwitch(void) {
#if ENABLE_CONTEXT_SWITCH_FLUSH
    uint8_t token = 0xa7;
    fullWrite(cs_parent_to_child[1], &token, sizeof(token));
    fullRead(cs_child_to_parent[0], &token, sizeof(token));
    mfence();
#else
#endif
}

void dummyAccesses(void){
    for(uint64_t i = 0; i < DUMMY_BUFFER_SIZE; i += LINE_SIZE){
        // mStore(dummy_buffer + i, random() & 0xFF);
        asm volatile("LDR w0, [%0]\n\t" :: "r"(&dummy_buffer[i]) : "memory", "w0");
    }
    mfence();
}

__attribute__((noinline)) static uint64_t randomLoadPc0(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc1(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc2(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc3(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc4(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc5(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc6(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

__attribute__((noinline)) static uint64_t randomLoadPc7(uint8_t *addr) {
    uint64_t v;
    asm volatile("ldrb %w0, [%1]\n\t" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static uint64_t randomLoadByPc(int pc, uint8_t *addr) {
    switch(pc) {
        case 0: return randomLoadPc0(addr);
        case 1: return randomLoadPc1(addr);
        case 2: return randomLoadPc2(addr);
        case 3: return randomLoadPc3(addr);
        case 4: return randomLoadPc4(addr);
        case 5: return randomLoadPc5(addr);
        case 6: return randomLoadPc6(addr);
        default: return randomLoadPc7(addr);
    }
}

void randomAccesses(void) {
    uint64_t acc = random_sink;
    uint64_t state = 0x9e3779b97f4a7c15ULL ^ acc;

    for(uint64_t i = 0; i < RANDOM_BUFFER_LINES; i++) {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z = z ^ (z >> 31);

        uint64_t line = z % RANDOM_BUFFER_LINES;
        uint8_t *addr = random_buffer + line * LINE_SIZE;
        acc += randomLoadByPc((int)(i % RANDOM_ACCESS_PCS), addr);
    }

    random_sink = acc ^ state;
    mfence();
}

static void print_test_header(uint64_t rounds) {
    printf("# arm64 CMC irregular-load prefetch latency map\n");
    printf("# access mode: load (ldrb)\n");
#if TEST_MODE == TEST_MODE_NODE
    printf("# test_mode=node\n");
#elif TEST_MODE == TEST_MODE_DEPTH
    printf("# test_mode=depth\n");
#else
    printf("# test_mode=window\n");
#endif
    printf("# sequence_length=%zu line_size=%d rounds=%llu probe_positions=%d\n",
           sizeof(array_index) / sizeof(array_index[0]),
           LINE_SIZE,
           (unsigned long long)rounds,
           PROBE_POSITIONS);
    printf("# trigger_index=%d trigger_position=%d trigger_offset_bytes=%d\n",
           DEFAULT_PROBE_INDEX,
           array_index[DEFAULT_PROBE_INDEX],
           array_index[DEFAULT_PROBE_INDEX] * LINE_SIZE);
#if TEST_MODE == TEST_MODE_WINDOW
    printf("# trigger_range=[%d,%d)\n", TRIGGER_START, TRIGGER_END);
#elif TEST_MODE == TEST_MODE_NODE
    printf("# node mode: access array_index[n], then probe array_index[n+1]\n");
#else
    printf("# depth mode: access array_index[0..d-1], then probe array_index[d]\n");
#endif
    printf("# random_access_pages=%d random_access_lines=%d random_access_pcs=%d\n",
           RANDOM_BUFFER_PAGES,
           RANDOM_BUFFER_LINES,
           RANDOM_ACCESS_PCS);
    printf("# context_switch_flush=%s each round\n",
           ENABLE_CONTEXT_SWITCH_FLUSH ? "pipe_fork_pingpong" : "disabled");
    printf("# timer: clock_gettime(CLOCK_MONOTONIC) ns\n");
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}


void flushArray2() {
    for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
        flush(&array2[offset]);
    }
}

static inline void delayBeforeTrigger(void) {
    volatile uint64_t dummy = 0;
    for(int k = 0; k < 100; k++) {
        dummy += array0[k * LINE_SIZE];
    }
    for(int j = 0; j < 100; j++) {
        nop();
    }
    (void)dummy;
}

static inline void delayBeforeProbe(void) {
    volatile uint64_t dummy = 0;
    for(int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for(int i = 0; i < 1000; i++) {
        nop();
    }
    (void)dummy;
}

static inline void trainCmcSequence(void) {
    for(int repeat = 0; repeat < 5; repeat++) {
        flushArray2();
        for(int t = 0; t < 256; t++) {
            mLoad(array2 + (array_index[t] * LINE_SIZE));
        }
    }
}

static inline __attribute__((always_inline))
uint64_t reloadTimeNs(volatile uint8_t *probe_addr, unsigned int *junk) {
    uint64_t time1 = timestamp();
    unsigned int value = *probe_addr;
    uint64_t elapsed = timestamp() - time1;
    *junk = value;
    return elapsed;
}

static void recordProbePosition(int probe_pos, uint64_t latency) {
    if(probe_pos >= 0 && probe_pos < Items) {
        latency_sum[probe_pos] += (long long int)latency;
        probe_count[probe_pos]++;
    }
}

static void printArrayIndexLatencies(void) {
    for(int t = 0; t < 256; t++) {
        int probe_pos = array_index[t];
        printf("array_index=%3d, offset_bytes=%4d * LINE_SIZE, avg_ns=%10lld, probes=%5d\n",
                t,
                probe_pos,
                probe_count[probe_pos] > 0 ? latency_sum[probe_pos] / probe_count[probe_pos] : -1,
                probe_count[probe_pos]);
    }
}

int main(){
  volatile uint8_t * probe_addr;
  unsigned int junk = 0;


  memset(array2,-1,Items*LINE_SIZE*sizeof(uint8_t));


  dummy_buffer = (uint8_t*)mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(dummy_buffer == MAP_FAILED) {
      printf("failed to map memory to access!\n");
      exit(1);
  }
  random_buffer = (uint8_t*)mmap(NULL, RANDOM_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
  if(random_buffer == MAP_FAILED) {
      printf("failed to map memory for random accesses!\n");
      exit(1);
  }
  memset(random_buffer, 0xa5, RANDOM_BUFFER_SIZE);

  for(int i=0; i< Items; i++){
    mLoad(&array2[i * 64]);
  }


  for (uint64_t offset = 0; offset < Items*LINE_SIZE; offset+=LINE_SIZE){
    flush(&array2[offset]);
  }
  mfence();


  uint64_t rounds = ROUNDS;

    initContextSwitchHelper();
    print_test_header(rounds);
    for(uint64_t atkRound = 0; atkRound < rounds; ++atkRound) {
        forceContextSwitch();
        
        flushArray2();
        randomAccesses();
        // dummyAccesses();//for dummy accesses , reset the prefetcher state

        trainCmcSequence();
        // 访问其它pc的load，让上面的训练结束，写到L2 Cache中
        // randomAccesses();
        flushArray2();

        delayBeforeTrigger();

#if TEST_MODE == TEST_MODE_NODE
        int node_idx = (int)(atkRound % 255);
        int probe_slot = node_idx + 1;

        mLoad(array2 + (array_index[node_idx] * LINE_SIZE));
        delayBeforeProbe();

        probe_addr = array2 + (array_index[probe_slot] * LINE_SIZE);
        uint64_t time2 = reloadTimeNs(probe_addr, &junk);
        recordProbePosition(array_index[probe_slot], time2);
#elif TEST_MODE == TEST_MODE_DEPTH
        int depth = 1 + (int)(atkRound % 255);
        int probe_slot = depth;

        for(int t = 0; t < depth; t++) {
            mLoad(array2 + (array_index[t] * LINE_SIZE));
        }
        delayBeforeProbe();

        probe_addr = array2 + (array_index[probe_slot] * LINE_SIZE);
        uint64_t time2 = reloadTimeNs(probe_addr, &junk);
        recordProbePosition(array_index[probe_slot], time2);
#else
        for(int t =TRIGGER_START;t<TRIGGER_END;t++){
            mLoad(array2 + (array_index[t] * LINE_SIZE));
            // mfence();
        }
        delayBeforeProbe();

        int probe_pos = atkRound % PROBE_POSITIONS;//test one position each round
        
        probe_addr = array2 + (probe_pos * LINE_SIZE);


        uint64_t time2 = reloadTimeNs(probe_addr, &junk);
        recordProbePosition(probe_pos, time2);
#endif
    } 
    printArrayIndexLatencies();
    // for(int probe_pos = 0; probe_pos < PROBE_POSITIONS; probe_pos++) {
    //     long long int avg_ns = 0;
    //     if(probe_count[probe_pos] > 0) {
    //         avg_ns = latency_sum[probe_pos] / probe_count[probe_pos];
    //     }
    //     printf("%3d\t%12d\t%10lld\t%5d\n",
    //             probe_pos,
    //             probe_pos * LINE_SIZE,
    //             avg_ns,
    //             probe_count[probe_pos]);
    // }
      printf("\n");

  (void)junk;
  cleanupContextSwitchHelper();
  return 0;
}
