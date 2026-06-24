#include <stdbool.h>
#include <stdint.h>
#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>
#include <store_stride_ta.h>

TEE_Result TA_CreateEntryPoint(void) {
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void) {
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types,
                                    TEE_Param params[4],
                                    void **session) {
    (void)param_types;
    (void)params;
    (void)session;
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *session) {
    (void)session;
}

static void __attribute__((noinline)) secure_trigger_store(void *addr) {
    volatile uint8_t *target = (volatile uint8_t *)addr;

    *target = (uint8_t)(uintptr_t)addr;
}

static uint8_t __attribute__((noinline)) secure_trigger_load(void *addr) {
    volatile uint8_t *target = (volatile uint8_t *)addr;

    return *target;
}

static TEE_Result trigger_store(uint32_t param_types, TEE_Param params[4]) {
    uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INOUT,
                                       TEE_PARAM_TYPE_VALUE_INPUT,
                                       TEE_PARAM_TYPE_NONE,
                                       TEE_PARAM_TYPE_NONE);
    uint8_t *base;
    size_t size;
    size_t offset;
    size_t second_offset;

    if (param_types != expected) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    base = params[0].memref.buffer;
    size = params[0].memref.size;
    offset = (size_t)params[1].value.a;
    second_offset = (size_t)params[1].value.b;

    if (!base || offset >= size ||
        (second_offset != 0 && second_offset >= size)) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    secure_trigger_store(base + offset);
    if (second_offset != 0) {
        secure_trigger_store(base + second_offset);
    }
    return TEE_SUCCESS;
}

static TEE_Result load_sequence(uint32_t param_types, TEE_Param params[4],
                                bool include_trigger) {
    uint32_t expected = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INOUT,
                                       TEE_PARAM_TYPE_VALUE_INPUT,
                                       TEE_PARAM_TYPE_NONE,
                                       TEE_PARAM_TYPE_NONE);
    uint8_t *base;
    size_t size;
    size_t stride_bytes;
    uint32_t train_accesses;
    uint32_t access_count;
    volatile uint8_t sink = 0;

    if (param_types != expected) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    base = params[0].memref.buffer;
    size = params[0].memref.size;
    stride_bytes = (size_t)params[1].value.a;
    train_accesses = params[1].value.b;
    access_count = train_accesses + (include_trigger ? 1U : 0U);

    if (!base || stride_bytes == 0 || access_count == 0 ||
        (size_t)(access_count - 1U) * stride_bytes >= size) {
        return TEE_ERROR_BAD_PARAMETERS;
    }

    for (uint32_t step = 0; step < access_count; step++) {
        sink ^= secure_trigger_load(base + ((size_t)step * stride_bytes));
    }

    (void)sink;
    return TEE_SUCCESS;
}

TEE_Result TA_InvokeCommandEntryPoint(void *session,
                                      uint32_t command,
                                      uint32_t param_types,
                                      TEE_Param params[4]) {
    (void)session;

    switch (command) {
    case STORE_STRIDE_TA_CMD_TRIGGER_STORE:
        return trigger_store(param_types, params);
    case STORE_STRIDE_TA_CMD_TRIGGER_LOAD:
        return load_sequence(param_types, params, true);
    case STORE_STRIDE_TA_CMD_TRAIN_LOAD:
        return load_sequence(param_types, params, false);
    case STORE_STRIDE_TA_CMD_NOP:
        return TEE_SUCCESS;
    default:
        return TEE_ERROR_NOT_SUPPORTED;
    }
}
