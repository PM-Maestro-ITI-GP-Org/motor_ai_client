#ifndef AI_RESULT_WRITER_H
#define AI_RESULT_WRITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ai_result_writer ai_result_writer_t;

ai_result_writer_t *ai_result_writer_open(void);
void ai_result_writer_close(ai_result_writer_t *w);

void ai_result_writer_publish(ai_result_writer_t *w,
                               uint64_t timestamp,
                               uint32_t producer_seq,
                               uint16_t flags,
                               const char *anomaly_result,
                               const char *fault_class_result,
                               const char *pred_maint_result);

#ifdef __cplusplus
}
#endif

#endif
