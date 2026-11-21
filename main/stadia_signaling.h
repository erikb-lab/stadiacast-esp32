/* Phase 2: Discovery state, GMsg queue, auto-linking (ported from Stadiacast). */
#ifndef STADIA_SIGNALING_H
#define STADIA_SIGNALING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Max sizes (validated against Stadiacast: real bodies are 470–600+ bytes) */
#define DISPLAY_ID_MAX    63
#define DISPLAY_TOKEN_MAX 63
#define GMMSG_MAX         1024
#define GMMSG_QUEUE_DEPTH 4
#define REGISTER_ID_MAX   64

/* Per-controller session (simplified: one controller supported for v1) */
typedef struct {
    bool registered;
    char registration_id[REGISTER_ID_MAX];
    int64_t poll_interval_ms;
    char registration_token[REGISTER_ID_MAX];
    bool has_display;                         /* display oneof set (autolink) */
    char display_id[DISPLAY_ID_MAX];
    char display_token[DISPLAY_TOKEN_MAX];
    /* GMMsg queue */
    uint8_t  gmsg_queue[GMMSG_QUEUE_DEPTH][GMMSG_MAX];
    size_t   gmsg_queue_lens[GMMSG_QUEUE_DEPTH];
    uint32_t gmsg_ids[GMMSG_QUEUE_DEPTH];
    int      gmsg_head, gmsg_tail, gmsg_count;
    uint32_t next_msg_id;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t gmsg_sem;               /* Wakes a parked /gmsg poll immediately */
} stadia_session_t;

void stadia_signaling_init(stadia_session_t *sess);
void stadia_signaling_generate_ids(stadia_session_t *sess);           /* display_id/token */
bool stadia_signaling_queue_gmsg(stadia_session_t *sess, const uint8_t *payload, size_t len);
/* Pop the oldest pending GMMsg envelope payload (waits up to timeout_ms). */
bool stadia_signaling_gmsg_wait(stadia_session_t *sess, uint32_t timeout_ms,
                                uint8_t *out, size_t out_len, size_t *out_len_ret,
                                uint32_t *out_id);
void stadia_signaling_mark_acked(stadia_session_t *sess, uint32_t id);
void stadia_signaling_auto_link(stadia_session_t *sess);              /* Generate display_id/token */

#endif /* STADIA_SIGNALING_H */
