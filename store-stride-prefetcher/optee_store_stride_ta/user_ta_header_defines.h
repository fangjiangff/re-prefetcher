#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include <stdint.h>
#include <store_stride_ta.h>

#define TA_UUID STORE_STRIDE_TA_UUID

#define TA_FLAGS TA_FLAG_EXEC_DDR
#define TA_STACK_SIZE (2 * 1024)
#define TA_DATA_SIZE (32 * 1024)

#define TA_CURRENT_TA_EXT_PROPERTIES \
    { "gp.ta.description", USER_TA_PROP_TYPE_STRING, \
      "Store-stride TrustZone trigger TA" }, \
    { "gp.ta.version", USER_TA_PROP_TYPE_U32, \
      &(const uint32_t){ 0x0001 } }

#endif
