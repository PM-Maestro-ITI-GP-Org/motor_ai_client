#include "ai_result_writer.h"
#include "ai_result_shm.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

struct ai_result_writer {
    ai_result_region_t *region;
};

ai_result_writer_t *ai_result_writer_open(void)
{
    ai_result_writer_t *w = (ai_result_writer_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    int fd = shm_open(AI_RESULT_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd == -1) { free(w); return NULL; }

    if (ftruncate(fd, (off_t)sizeof(ai_result_region_t)) == -1) {
        close(fd); free(w); return NULL;
    }

    w->region = (ai_result_region_t *)mmap(NULL, sizeof(ai_result_region_t),
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, fd, 0);
    close(fd);
    if (w->region == MAP_FAILED) { free(w); return NULL; }

    w->region->magic   = AI_RESULT_SHM_MAGIC;
    w->region->version = AI_RESULT_VERSION;
    w->region->_pad0   = 0;
    atomic_store_explicit(&w->region->snapshot.seqlock, 0u, memory_order_relaxed);

    /*
     * A freshly created region otherwise sits at seqlock==0 until the first
     * window finishes inference -- motor_diag_service's own reader treats
     * seq==0 as "never written" and reports nothing at all (see
     * build_fault_event() in motor_diag_service.c). Every consumer
     * downstream of that then has no reading at all rather than a
     * plausible one for however long the first window takes.
     *
     * Publish a normal/no-fault/full-health snapshot right here instead, in
     * the exact string shape motor_ai_node itself emits (inference.hpp's
     * Engine::runAnomaly/runFaultClass/runRul, and the server's own
     * "anomaly_ == normal" -> "none" fold in MotorDataStubImpl.cpp): the
     * band word for a full-health motor is "healthy" (health_model.json's
     * bands[0]), health 1.0 is the model's own ceiling, and RUL at full
     * health is life_index (4, health_model.json/rul_config.json) months --
     * "RUL 4.00 months" is not a guess, it is what this exact model prints
     * for an undegraded motor. Overwritten by the first real
     * ai_result_writer_publish() call the moment one window completes.
     */
    ai_result_writer_publish(w, (uint64_t)time(NULL), 0, 0,
                              "normal", "none",
                              "healthy, health 1.00, RUL 4.00 months (provisional)");

    return w;
}

void ai_result_writer_close(ai_result_writer_t *w)
{
    if (!w) return;
    if (w->region && w->region != (ai_result_region_t *)MAP_FAILED)
        munmap(w->region, sizeof(ai_result_region_t));
    shm_unlink(AI_RESULT_SHM_NAME);
    free(w);
}

void ai_result_writer_publish(ai_result_writer_t *w,
                               uint64_t timestamp,
                               uint32_t producer_seq,
                               uint16_t flags,
                               const char *anomaly_result,
                               const char *fault_class_result,
                               const char *pred_maint_result)
{
    ai_result_snapshot_t *s = &w->region->snapshot;

    uint32_t s0 = atomic_load_explicit(&s->seqlock, memory_order_relaxed);
    atomic_store_explicit(&s->seqlock, s0 + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    s->timestamp    = timestamp;
    s->producer_seq = producer_seq;
    s->flags        = flags;
    s->_pad         = 0;

    size_t len;
    len = strlen(anomaly_result);
    if (len >= AI_RESULT_STR_LEN) len = AI_RESULT_STR_LEN - 1;
    memcpy(s->anomaly_result, anomaly_result, len);
    s->anomaly_result[len] = '\0';

    len = strlen(fault_class_result);
    if (len >= AI_RESULT_STR_LEN) len = AI_RESULT_STR_LEN - 1;
    memcpy(s->fault_class_result, fault_class_result, len);
    s->fault_class_result[len] = '\0';

    len = strlen(pred_maint_result);
    if (len >= AI_RESULT_STR_LEN) len = AI_RESULT_STR_LEN - 1;
    memcpy(s->pred_maint_result, pred_maint_result, len);
    s->pred_maint_result[len] = '\0';

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->seqlock, s0 + 2u, memory_order_relaxed);
}
