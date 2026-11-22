/* Phase 2: Discovery state, GMsg queue, auto-linking (1..4 controllers). */
#include "stadia_signaling.h"
#include "stadia_transport.h"
#include "usb_gamepad.h"

#include <string.h>
#include <stdio.h>

#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "pb.h"
#include "pb_encode.h"
#include "discovery_api.pb.h"

#define TAG "SIG"

#if CONFIG_SPIRAM
#define STADIA_SIG_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define STADIA_SIG_EXT_BSS
#endif

#define GMSG_COMMAND_TYPE    42
#define SERVER_NAME          "stadia-esp32"
#define DISPLAY_ID_PREFIX    SERVER_NAME
#define DISPLAY_TOKEN_PREFIX SERVER_NAME "-display"

STADIA_SIG_EXT_BSS stadia_session_t stadia_sessions[STADIA_MAX_PLAYERS];

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
    SemaphoreHandle_t old_lock = sess->lock;
    SemaphoreHandle_t old_sem  = sess->gmsg_sem;
    int old_slot = sess->slot;
    memset(sess, 0, sizeof(*sess));
    sess->slot = old_slot;
    sess->next_msg_id = 1;
    sess->lock = old_lock ? old_lock : xSemaphoreCreateMutex();
    sess->gmsg_sem = old_sem ? old_sem : xSemaphoreCreateCounting(GMMSG_QUEUE_DEPTH + 1, 0);
    stadia_signaling_generate_ids(sess);
}

void stadia_signaling_init_all(void) {
    for (int i = 0; i < STADIA_MAX_PLAYERS; i++) {
        stadia_sessions[i].slot = i;
        stadia_signaling_init(&stadia_sessions[i]);
    }
}

int stadia_signaling_resolve_slot(const char *dev_id, const char *reg_or_sub, uint32_t peer_ip) {
    int max_p = (int)usb_gamepad_get_max_players();
    if (max_p <= 1) {
        if (peer_ip != 0) stadia_sessions[0].peer_ip = peer_ip;
        stadia_sessions[0].last_seen_us = esp_timer_get_time();
        return 0;
    }
    if (max_p > STADIA_MAX_PLAYERS) max_p = STADIA_MAX_PLAYERS;
    int64_t now = esp_timer_get_time();

    /* 1. Exact match by device_id */
    if (dev_id && dev_id[0] != '\0') {
        for (int i = 0; i < max_p; i++) {
            if (stadia_sessions[i].device_id[0] != '\0' &&
                strcmp(stadia_sessions[i].device_id, dev_id) == 0) {
                if (peer_ip != 0) stadia_sessions[i].peer_ip = peer_ip;
                stadia_sessions[i].last_seen_us = now;
                return i;
            }
        }
    }

    /* 2. Match by registration_id or subscription containing registration_id */
    if (reg_or_sub && reg_or_sub[0] != '\0') {
        for (int i = 0; i < max_p; i++) {
            if (stadia_sessions[i].registration_id[0] != '\0' &&
                strstr(reg_or_sub, stadia_sessions[i].registration_id) != NULL) {
                if (peer_ip != 0) stadia_sessions[i].peer_ip = peer_ip;
                stadia_sessions[i].last_seen_us = now;
                return i;
            }
        }
    }

    /* 3. Match by SoftAP client IPv4 address */
    if (peer_ip != 0) {
        for (int i = 0; i < max_p; i++) {
            if (stadia_sessions[i].peer_ip == peer_ip) {
                stadia_sessions[i].last_seen_us = now;
                return i;
            }
        }
    }

    /* 4. Claim first unused slot */
    for (int i = 0; i < max_p; i++) {
        if (!stadia_sessions[i].registered && stadia_sessions[i].peer_ip == 0) {
            stadia_sessions[i].peer_ip = peer_ip;
            stadia_sessions[i].last_seen_us = now;
            if (dev_id && dev_id[0]) {
                snprintf(stadia_sessions[i].device_id, sizeof(stadia_sessions[i].device_id), "%s", dev_id);
            }
            ESP_LOGI(TAG, "Assigned controller (dev=%s ip=0x%08lx) -> Player %d",
                     dev_id ? dev_id : "?", (unsigned long)peer_ip, i + 1);
            return i;
        }
    }

    /* 5. Reclaim disconnected or oldest slot */
    int victim = 0;
    int64_t oldest = stadia_sessions[0].last_seen_us;
    for (int i = 0; i < max_p; i++) {
        if (!stadia_transport_is_slot_ready(i) &&
            (now - stadia_sessions[i].last_seen_us) > 10000000LL) {
            victim = i;
            break;
        }
        if (stadia_sessions[i].last_seen_us < oldest) {
            oldest = stadia_sessions[i].last_seen_us;
            victim = i;
        }
    }

    stadia_signaling_init(&stadia_sessions[victim]);
    stadia_sessions[victim].peer_ip = peer_ip;
    stadia_sessions[victim].last_seen_us = now;
    if (dev_id && dev_id[0]) {
        snprintf(stadia_sessions[victim].device_id, sizeof(stadia_sessions[victim].device_id), "%s", dev_id);
    }
    ESP_LOGI(TAG, "Reassigned slot -> Player %d (dev=%s ip=0x%08lx)",
             victim + 1, dev_id ? dev_id : "?", (unsigned long)peer_ip);
    return victim;
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
        ESP_LOGI(TAG, "[P%d] Auto-linked display_id=%s token=%s",
                 sess->slot + 1, sess->display_id, sess->display_token);
    }
}
