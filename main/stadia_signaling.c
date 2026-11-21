/* Phase 2: Discovery state, GMsg queue, auto-linking (ported from Stadiacast). */
#include "stadia_signaling.h"

#include <string.h>
#include <stdio.h>

#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"

#include "pb.h"
#include "pb_encode.h"
#include "discovery_api.pb.h"

#define TAG "SIG"

#define GMSG_COMMAND_TYPE    42
#define SERVER_NAME          "stadia-esp32"
#define DISPLAY_ID_PREFIX    SERVER_NAME
#define DISPLAY_TOKEN_PREFIX SERVER_NAME "-display"

void stadia_signaling_generate_ids(stadia_session_t *sess) {
    snprintf(sess->display_id, sizeof(sess->display_id), "%s", DISPLAY_ID_PREFIX);
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    snprintf(sess->display_id + strlen(sess->display_id), sizeof(sess->display_id) - strlen(sess->display_id),
             "-%02x%02x%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    snprintf(sess->display_token, sizeof(sess->display_token), "%s-%02x%02x%02x%02x%02x%02x",
             DISPLAY_TOKEN_PREFIX, r[0], r[1], r[2], r[3], r[4], r[5]);
    sess->display_token[sizeof(sess->display_token) - 1] = 0;
}

void stadia_signaling_init(stadia_session_t *sess) {
    memset(sess, 0, sizeof(*sess));
    sess->next_msg_id = 1;
    sess->lock = xSemaphoreCreateMutex();
    sess->gmsg_sem = xSemaphoreCreateCounting(GMMSG_QUEUE_DEPTH + 1, 0);
    stadia_signaling_generate_ids(sess);
}

/* ---- GMsg ---- */
bool stadia_signaling_queue_gmsg(stadia_session_t *sess, const uint8_t *payload, size_t len) {
    xSemaphoreTake(sess->lock, portMAX_DELAY);
    if (sess->gmsg_count >= GMMSG_QUEUE_DEPTH) {
        sess->gmsg_head = (sess->gmsg_head + 1) % GMMSG_QUEUE_DEPTH;
        sess->gmsg_count--;
    }
    uint8_t *dst = sess->gmsg_queue[sess->gmsg_tail];
    size_t capped = len > GMMSG_MAX ? GMMSG_MAX : len;
    memcpy(dst, payload, capped);
    sess->gmsg_queue_lens[sess->gmsg_tail] = capped;
    sess->gmsg_ids[sess->gmsg_tail] = sess->next_msg_id++;
    sess->gmsg_tail = (sess->gmsg_tail + 1) % GMMSG_QUEUE_DEPTH;
    sess->gmsg_count++;
    xSemaphoreGive(sess->lock);

    xSemaphoreGive(sess->gmsg_sem);
    return true;
}

bool stadia_signaling_gmsg_wait(stadia_session_t *sess, uint32_t timeout_ms,
                                uint8_t *out, size_t out_len, size_t *out_len_ret,
                                uint32_t *out_id) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        xSemaphoreTake(sess->lock, portMAX_DELAY);
        if (sess->gmsg_count > 0) {
            const uint8_t *src = sess->gmsg_queue[sess->gmsg_head];
            size_t len = sess->gmsg_queue_lens[sess->gmsg_head];
            uint32_t id = sess->gmsg_ids[sess->gmsg_head];
            size_t n = len < out_len ? len : out_len;
            memcpy(out, src, n);
            *out_len_ret = n;
            if (out_id) *out_id = id;
            sess->gmsg_head = (sess->gmsg_head + 1) % GMMSG_QUEUE_DEPTH;
            sess->gmsg_count--;
            xSemaphoreGive(sess->lock);
            return true;
        }
        xSemaphoreGive(sess->lock);

        TickType_t now = xTaskGetTickCount();
        if (timeout_ms == 0 || (int32_t)(deadline - now) <= 0) {
            return false;
        }
        if (xSemaphoreTake(sess->gmsg_sem, deadline - now) != pdTRUE) {
            return false;
        }
    }
}

void stadia_signaling_mark_acked(stadia_session_t *sess, uint32_t id) {
    (void)sess; (void)id;
}

void stadia_signaling_auto_link(stadia_session_t *sess) {
    if (!sess->has_display) {
        sess->has_display = true;
        ESP_LOGI(TAG, "Auto-linked display_id=%s token=%s", sess->display_id, sess->display_token);
    }
}
