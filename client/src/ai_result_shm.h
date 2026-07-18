#ifndef AI_RESULT_SHM_H
#define AI_RESULT_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define AI_RESULT_SHM_NAME    "/motor_ai_result"
#define AI_RESULT_SHM_MAGIC   0x41495200u   /* "AIR\0" */
#define AI_RESULT_VERSION     1u
#define AI_RESULT_STR_LEN     256u

#ifndef AI_CACHELINE
#define AI_CACHELINE 64u
#endif

typedef struct {
    _Alignas(AI_CACHELINE) _Atomic uint32_t seqlock;
    uint64_t     timestamp;
    uint32_t     producer_seq;
    uint16_t     flags;
    uint16_t     _pad;
    char         anomaly_result[AI_RESULT_STR_LEN];
    char         fault_class_result[AI_RESULT_STR_LEN];
    char         pred_maint_result[AI_RESULT_STR_LEN];
} ai_result_snapshot_t;

typedef struct {
    uint32_t  magic;
    uint16_t  version;
    uint16_t  _pad0;
    _Alignas(AI_CACHELINE) ai_result_snapshot_t snapshot;
} ai_result_region_t;

#endif
