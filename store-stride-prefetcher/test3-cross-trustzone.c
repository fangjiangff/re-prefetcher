#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/tee.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "until.h"

#define STR_HELPER(x) #x
#define XSTR(x) STR_HELPER(x)

/*
 * Normal world / Secure world stride prefetcher state test skeleton.
 *
 * This file needs a TEE device such as /dev/tee0 and a Trusted Application
 * that implements CMD_TRIGGER_STORE and CMD_TRIGGER_LOAD:
 *
 *   param0: MEMREF_INOUT, the shared 4KB page
 *   param1: VALUE_INPUT, a = first_trigger_offset,
 *                        b = second_trigger_offset
 *
 * The TA should perform exactly one byte access to each of:
 *
 *   memref_base + first_trigger_offset
 *   memref_base + second_trigger_offset
 *
 * Store mode:
 *   Normal world stores line 0, 5, 10, 15
 *
 *   Depending on the selected experiment, normal world or secure world stores
 *   lines 20 and 25 as trigger accesses.
 *
 * Load mode:
 *   Normal world loads line 0, 5, 10, 15, switches to secure world for a
 *   no-op TA command, then optionally loads lines 20 and 25 as triggers. The
 *   load train and trigger accesses are from the same noinline normal-world PC.
 *
 * Normal world:
 *   probe one line per round. The next stride after the trigger line is the
 *   expected prediction if the stride state is shared across worlds.
 *
 * Important: this uses TEE shared memory. It is not a secure-only physical
 * page, because normal world cannot directly access secure-only memory.
 * Instead, the test checks whether predictor state crosses Normal/Secure
 * execution while both worlds touch the same shared physical buffer.
 */

#define PAGE_LINES (PAGE_SIZE / LINE_SIZE)
#define DUMMY_BUFFER_SIZE (PAGE_SIZE * 10)

#ifndef STRIDE_LINES
#define STRIDE_LINES 5
#endif

#ifndef TRAIN_ACCESSES
#define TRAIN_ACCESSES 5
#endif

#ifndef ROUNDS
#define ROUNDS 4000
#endif

#ifndef PROBE_POSITIONS
#define PROBE_POSITIONS PAGE_LINES
#endif

#ifndef CPU_ID
#define CPU_ID -1
#endif

#ifndef USE_NOINLINE_STORE
#define USE_NOINLINE_STORE 1
#endif

#ifndef TRAIN_ACCESS_LOAD
#define TRAIN_ACCESS_LOAD 0
#endif

#ifndef TRIGGER_ACCESSES
#define TRIGGER_ACCESSES 2
#endif

#define TRAIN_ONLY_ACCESSES TRAIN_ACCESSES
#define FIRST_TRIGGER_LINE_INDEX (TRAIN_ACCESSES * STRIDE_LINES)
#define LAST_TRIGGER_LINE_INDEX ((TRAIN_ACCESSES + TRIGGER_ACCESSES - 1) * STRIDE_LINES)
#define PREDICTED_LINE_INDEX ((TRAIN_ACCESSES + TRIGGER_ACCESSES) * STRIDE_LINES)

#ifndef CMD_TRIGGER_STORE
#define CMD_TRIGGER_STORE 0
#endif

#ifndef CMD_TRIGGER_LOAD
#define CMD_TRIGGER_LOAD 1
#endif

#ifndef CMD_TRAIN_LOAD
#define CMD_TRAIN_LOAD 2
#endif

#ifndef CMD_NOP
#define CMD_NOP 3
#endif

#ifndef NO_TRIGGER
#define NO_TRIGGER 0
#endif

#ifndef SKIP_SECURE_SWITCH
#define SKIP_SECURE_SWITCH 0
#endif

#ifndef NS_TRIGGER_AFTER_SECURE_NOOP
#define NS_TRIGGER_AFTER_SECURE_NOOP 0
#endif

#ifndef DEFAULT_TA_UUID
#define DEFAULT_TA_UUID "b6a189a0-7697-4aa8-9d62-80f64ec4e74d"
#endif

static uint8_t *shared_page;
static uint8_t *dummy_buffer;
static uint8_t array1[100 * LINE_SIZE] = {0};

static long long latency_sum[PROBE_POSITIONS] = {0};
static int probe_count[PROBE_POSITIONS] = {0};

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void set_cpu_if_requested(void) {
#if CPU_ID >= 0
    cpu_set_t mask;

    CPU_ZERO(&mask);
    CPU_SET(CPU_ID, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        die("sched_setaffinity");
    }
#endif
}

static int hex_value(char c) {
    if ('0' <= c && c <= '9') {
        return c - '0';
    }
    if ('a' <= c && c <= 'f') {
        return c - 'a' + 10;
    }
    if ('A' <= c && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_uuid(const char *text, uint8_t uuid[TEE_IOCTL_UUID_LEN]) {
    char compact[TEE_IOCTL_UUID_LEN * 2 + 1];
    size_t out = 0;

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '-') {
            continue;
        }
        if (hex_value(text[i]) < 0 || out >= TEE_IOCTL_UUID_LEN * 2) {
            return -1;
        }
        compact[out++] = text[i];
    }
    if (out != TEE_IOCTL_UUID_LEN * 2) {
        return -1;
    }
    compact[out] = '\0';

    for (size_t i = 0; i < TEE_IOCTL_UUID_LEN; i++) {
        int hi = hex_value(compact[i * 2]);
        int lo = hex_value(compact[i * 2 + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        uuid[i] = (uint8_t)((hi << 4) | lo);
    }

    return 0;
}

static void flush_shared_page(void) {
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        flush(shared_page + offset);
    }
    // mfence();
}

static void dummyAccesses(void) {
// #if TRAIN_ACCESS_LOAD
//     for (size_t i = 0; i < DUMMY_BUFFER_SIZE; i += LINE_SIZE) {
//         mStore_inline(dummy_buffer + i);
//     }
//     mfence();
// #else
    dummyAccess(dummy_buffer, DUMMY_BUFFER_SIZE);
// #endif
}

static inline __attribute__((always_inline)) void access_for_train(void *addr) {
#if TRAIN_ACCESS_LOAD
    mLoad_noinline(addr);
#else
#if USE_NOINLINE_STORE
    mStore_noinline(addr);
#else
    mStore_inline(addr);
#endif
#endif
}

static void train_in_normal_world(int stride_bytes) {
    for (int step = 0; step < TRAIN_ONLY_ACCESSES; step++) {
        access_for_train(shared_page + ((size_t)step * (size_t)stride_bytes));
    }
}

static size_t trigger_offset_for_index(int trigger_index, int stride_bytes) {
    return (size_t)(TRAIN_ONLY_ACCESSES + trigger_index) * (size_t)stride_bytes;
}

#if !NO_TRIGGER && (TRAIN_ACCESS_LOAD || SKIP_SECURE_SWITCH || NS_TRIGGER_AFTER_SECURE_NOOP)
static void trigger_one_in_normal_world(size_t trigger_offset) {
#if TRAIN_ACCESS_LOAD
    mLoad_noinline(shared_page + trigger_offset);
#else
#if USE_NOINLINE_STORE
    mStore_noinline(shared_page + trigger_offset);
#else
    mStore_inline(shared_page + trigger_offset);
#endif
#endif
}

static void trigger_in_normal_world(int stride_bytes) {
    for (int trigger_index = 0; trigger_index < TRIGGER_ACCESSES; trigger_index++) {
        trigger_one_in_normal_world(trigger_offset_for_index(trigger_index,
                                                            stride_bytes));
    }
}
#endif

static void delay_after_trigger(void) {
    uint64_t dummy = 0;

    for (int k = 0; k < 100; k++) {
        dummy += array1[k * LINE_SIZE];
    }
    for (int i = 0; i < 1000; i++) {
        nop();
    }

    (void)dummy;
}

static int open_tee_device(const char *path) {
    struct tee_ioctl_version_data version;
    int fd = open(path, O_RDWR);

    if (fd < 0) {
        die("open TEE device");
    }
    memset(&version, 0, sizeof(version));
    if (ioctl(fd, TEE_IOC_VERSION, &version) != 0) {
        die("TEE_IOC_VERSION");
    }

    printf("# tee_impl_id=%u tee_impl_caps=0x%x tee_gen_caps=0x%x\n",
           version.impl_id, version.impl_caps, version.gen_caps);

    return fd;
}

static int alloc_tee_shared_page(int tee_fd, int *shm_id) {
    struct tee_ioctl_shm_alloc_data data;
    int shm_fd;

    memset(&data, 0, sizeof(data));
    data.size = PAGE_SIZE;

    shm_fd = ioctl(tee_fd, TEE_IOC_SHM_ALLOC, &data);
    if (shm_fd < 0) {
        die("TEE_IOC_SHM_ALLOC");
    }
    *shm_id = data.id;

    shared_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, shm_fd, 0);
    if (shared_page == MAP_FAILED) {
        die("mmap TEE shared memory");
    }

    return shm_fd;
}

static uint32_t open_ta_session(int tee_fd, const uint8_t uuid[TEE_IOCTL_UUID_LEN]) {
    size_t buf_size = sizeof(struct tee_ioctl_open_session_arg);
    struct tee_ioctl_open_session_arg *arg = calloc(1, buf_size);
    struct tee_ioctl_buf_data buf;
    uint32_t session;

    if (!arg) {
        die("calloc open session");
    }

    memcpy(arg->uuid, uuid, TEE_IOCTL_UUID_LEN);
    arg->clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
    arg->num_params = 0;

    memset(&buf, 0, sizeof(buf));
    buf.buf_ptr = (uintptr_t)arg;
    buf.buf_len = buf_size;

    if (ioctl(tee_fd, TEE_IOC_OPEN_SESSION, &buf) != 0) {
        die("TEE_IOC_OPEN_SESSION");
    }
    if (arg->ret != 0) {
        fprintf(stderr, "TEE open session failed: ret=0x%x origin=0x%x\n",
                arg->ret, arg->ret_origin);
        exit(1);
    }

    session = arg->session;
    free(arg);
    return session;
}

static void close_ta_session(int tee_fd, uint32_t session) {
    struct tee_ioctl_close_session_arg arg;

    memset(&arg, 0, sizeof(arg));
    arg.session = session;
    if (ioctl(tee_fd, TEE_IOC_CLOSE_SESSION, &arg) != 0) {
        die("TEE_IOC_CLOSE_SESSION");
    }
}

#if (!NO_TRIGGER && !TRAIN_ACCESS_LOAD) || (TRAIN_ACCESS_LOAD && !SKIP_SECURE_SWITCH)
static void invoke_secure_world(int tee_fd, uint32_t session, int shm_id,
                                size_t arg_a, size_t arg_b) {
    enum { NUM_PARAMS = 2 };
    size_t buf_size = sizeof(struct tee_ioctl_invoke_arg) +
                      NUM_PARAMS * sizeof(struct tee_ioctl_param);
    struct tee_ioctl_invoke_arg *arg = calloc(1, buf_size);
    struct tee_ioctl_buf_data buf;

    if (!arg) {
        die("calloc invoke");
    }

#if TRAIN_ACCESS_LOAD
    arg->func = CMD_NOP;
#elif NS_TRIGGER_AFTER_SECURE_NOOP
    arg->func = CMD_NOP;
#else
    arg->func = CMD_TRIGGER_STORE;
#endif
    arg->session = session;
    arg->num_params = NUM_PARAMS;

    arg->params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
    arg->params[0].a = 0;
    arg->params[0].b = PAGE_SIZE;
    arg->params[0].c = (uint64_t)shm_id;

    arg->params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
    arg->params[1].a = arg_a;
    arg->params[1].b = arg_b;

    memset(&buf, 0, sizeof(buf));
    buf.buf_ptr = (uintptr_t)arg;
    buf.buf_len = buf_size;

    if (ioctl(tee_fd, TEE_IOC_INVOKE, &buf) != 0) {
        die("TEE_IOC_INVOKE");
    }
    if (arg->ret != 0) {
        fprintf(stderr, "TEE invoke failed: ret=0x%x origin=0x%x\n",
                arg->ret, arg->ret_origin);
        exit(1);
    }

    free(arg);
}
#endif

static void print_header(const char *tee_path, int stride_bytes,
                         int first_trigger_line, int last_trigger_line,
                         int predicted_line) {
    printf("# arm64 Normal/Secure TrustZone stride retention test\n");
    printf("# training_mode=%s accesses=%d train_only_accesses=%d trigger_accesses=%d access=%s\n",
#if TRAIN_ACCESS_LOAD
#if SKIP_SECURE_SWITCH
           "normal_world_train_normal_world_trigger_no_secure_switch",
#else
           "normal_world_train_secure_world_noop_normal_world_trigger",
#endif
#elif SKIP_SECURE_SWITCH
           "normal_world_train_normal_world_trigger_no_secure_switch",
#elif NS_TRIGGER_AFTER_SECURE_NOOP
           "normal_world_train_secure_world_noop_normal_world_trigger",
#else
           "normal_world_train_secure_world_trigger",
#endif
           TRAIN_ONLY_ACCESSES + TRIGGER_ACCESSES,
           TRAIN_ONLY_ACCESSES, TRIGGER_ACCESSES,
#if TRAIN_ACCESS_LOAD
           "load"
#else
           "store"
#endif
    );
    printf("# tee_device=%s shared_page=0x%016lx\n",
           tee_path, (unsigned long)(uintptr_t)shared_page);
    printf("# stride_lines=%d stride_bytes=%d rounds=%d probe_positions=%d\n",
           STRIDE_LINES, stride_bytes, ROUNDS, PROBE_POSITIONS);
    printf("# trigger_lines=%d..%d predicted_line=%d train_pc=%s\n",
           first_trigger_line, last_trigger_line, predicted_line,
#if TRAIN_ACCESS_LOAD
           "normal_world_load_noinline_same_pc"
#elif SKIP_SECURE_SWITCH || NS_TRIGGER_AFTER_SECURE_NOOP
#if USE_NOINLINE_STORE
           "normal_world_store_noinline_same_pc"
#else
           "normal_world_store_inline_call_site_pc"
#endif
#else
#if USE_NOINLINE_STORE
           "normal_world_train_noinline_secure_trigger"
#else
           "normal_world_train_inline_secure_trigger"
#endif
#endif
    );
    printf("# trigger=%s\n",
#if NO_TRIGGER
#if TRAIN_ACCESS_LOAD && SKIP_SECURE_SWITCH
           "disabled_without_secure_switch"
#else
           "disabled_after_secure_noop"
#endif
#elif TRAIN_ACCESS_LOAD
#if SKIP_SECURE_SWITCH
           "normal_world_load_without_secure_switch"
#else
           "normal_world_load_after_secure_noop"
#endif
#elif SKIP_SECURE_SWITCH
           "normal_world_store_without_secure_switch"
#elif NS_TRIGGER_AFTER_SECURE_NOOP
           "normal_world_store_after_secure_noop"
#else
           "secure_world_ta_cmd_" XSTR(CMD_TRIGGER_STORE)
#endif
    );
    printf("# no_trigger_command=%s\n",
#if !NO_TRIGGER
           "not_applicable"
#elif SKIP_SECURE_SWITCH
           "none"
#elif TRAIN_ACCESS_LOAD || NS_TRIGGER_AFTER_SECURE_NOOP
           "secure_world_ta_cmd_" XSTR(CMD_NOP)
#else
           "normal_train_only"
#endif
    );
    printf("# position\toffset_bytes\tavg_ns\tprobes\n");
}

int main(int argc, char **argv) {
    const char *tee_path = "/dev/tee0";
    const char *ta_uuid_text = DEFAULT_TA_UUID;
    uint8_t uuid[TEE_IOCTL_UUID_LEN];
    int tee_fd;
    int shm_fd;
    int shm_id;
    uint32_t session;
    int stride_bytes = STRIDE_LINES * LINE_SIZE;
    int first_trigger_line = FIRST_TRIGGER_LINE_INDEX;
    int last_trigger_line = LAST_TRIGGER_LINE_INDEX;
    int predicted_line = PREDICTED_LINE_INDEX;
    unsigned int junk = 0;

    if (argc > 3) {
        fprintf(stderr, "usage: %s [TA_UUID] [TEE_DEVICE]\n", argv[0]);
        fprintf(stderr, "default TA_UUID: %s\n", DEFAULT_TA_UUID);
        return 2;
    }
    if (argc >= 2) {
        ta_uuid_text = argv[1];
    }
    if (argc == 3) {
        tee_path = argv[2];
    }
    if (parse_uuid(ta_uuid_text, uuid) != 0) {
        fprintf(stderr, "invalid TA UUID: %s\n", ta_uuid_text);
        return 2;
    }

    if (TRIGGER_ACCESSES < 1 || TRIGGER_ACCESSES > 2) {
        fprintf(stderr, "TRIGGER_ACCESSES must be 1 or 2\n");
        return 1;
    }
    if (stride_bytes <= 0 ||
        (size_t)PREDICTED_LINE_INDEX * LINE_SIZE >= PAGE_SIZE) {
        fprintf(stderr, "training/trigger/predicted lines must fit in one page\n");
        return 1;
    }
    if (PROBE_POSITIONS > PAGE_LINES) {
        fprintf(stderr, "PROBE_POSITIONS must be <= %d\n", PAGE_LINES);
        return 1;
    }

    set_cpu_if_requested();

    dummy_buffer = mmap(NULL, DUMMY_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (dummy_buffer == MAP_FAILED) {
        die("mmap dummy_buffer");
    }

    tee_fd = open_tee_device(tee_path);
    shm_fd = alloc_tee_shared_page(tee_fd, &shm_id);
    session = open_ta_session(tee_fd, uuid);

    memset(shared_page, 0xff, PAGE_SIZE);
    for (size_t offset = 0; offset < PAGE_SIZE; offset += LINE_SIZE) {
        mLoad(shared_page + offset);
    }

    print_header(tee_path, stride_bytes, first_trigger_line,
                 last_trigger_line, predicted_line);

    for (uint64_t round = 0; round < ROUNDS; round++) {
        int probe_pos = round % PROBE_POSITIONS;
        volatile uint8_t *probe_addr = shared_page + (probe_pos * LINE_SIZE);
        uint64_t time1;
        uint64_t time2;

        flush_shared_page();
        dummyAccesses();

#if TRAIN_ACCESS_LOAD
        train_in_normal_world(stride_bytes);
#if !SKIP_SECURE_SWITCH
        invoke_secure_world(tee_fd, session, shm_id, 0, 0);
#endif
#if !NO_TRIGGER
        trigger_in_normal_world(stride_bytes);
#endif
#else
        train_in_normal_world(stride_bytes);
#if !NO_TRIGGER
#if SKIP_SECURE_SWITCH
        trigger_in_normal_world(stride_bytes);
#elif NS_TRIGGER_AFTER_SECURE_NOOP
        invoke_secure_world(tee_fd, session, shm_id, 0, 0);
        trigger_in_normal_world(stride_bytes);
#else
        size_t first_trigger_offset = trigger_offset_for_index(0, stride_bytes);
        size_t second_trigger_offset =
            TRIGGER_ACCESSES > 1 ? trigger_offset_for_index(1, stride_bytes) : 0;

        invoke_secure_world(tee_fd, session, shm_id,
                            first_trigger_offset, second_trigger_offset);
#endif
#endif
#endif
        delay_after_trigger();

        time1 = timestamp();
        junk += *probe_addr;
        time2 = timestamp() - time1;

        latency_sum[probe_pos] += (long long)time2;
        probe_count[probe_pos]++;
    }

    for (int pos = 0; pos < PROBE_POSITIONS; pos++) {
        long long avg_ns = 0;

        if (probe_count[pos] > 0) {
            avg_ns = latency_sum[pos] / probe_count[pos];
        }
        printf("%3d\t%12d\t%10lld\t%5d\n",
               pos, pos * LINE_SIZE, avg_ns, probe_count[pos]);
    }

    close_ta_session(tee_fd, session);
    close(shm_fd);
    close(tee_fd);

    (void)junk;
    return 0;
}
