#ifndef SHM_READER_H
#define SHM_READER_H

#include <stdint.h>
#include <stddef.h>

#define SHM_READER_MAX_ROWS_PER_BLOCK 200u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t current[8];
    int16_t  vib_x;
    int16_t  vib_y;
    int16_t  vib_z;
    uint16_t rpm;
} motor_row_copy_t;

typedef struct {
    uint32_t producer_seq;
    uint64_t timestamp;
    uint16_t n_rows;
    uint16_t flags;
    motor_row_copy_t rows[SHM_READER_MAX_ROWS_PER_BLOCK];
} motor_block_copy_t;

typedef struct shm_reader shm_reader_t;

shm_reader_t *shm_reader_open(void);
void shm_reader_close(shm_reader_t *r);

size_t shm_reader_poll_blocks(shm_reader_t *r,
                               motor_block_copy_t *out_blocks,
                               size_t max_blocks,
                               uint64_t *out_dropped_blocks);

#ifdef __cplusplus
}
#endif

#endif
