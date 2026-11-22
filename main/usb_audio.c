#include "usb_audio.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "class/audio/audio_device.h"

#define TAG "USB_AUDIO"
#define SPK_SPSC_CAP 8192u

typedef struct {
    uint8_t *buf;
    size_t capacity;
    atomic_size_t head;
    atomic_size_t tail;
    atomic_uint_fast32_t drops;
} spsc_byte_buf_t;

static uint8_t s_spk_mem[SPK_SPSC_CAP] __attribute__((aligned(4)));
static spsc_byte_buf_t s_spk_buf = {
    .buf = s_spk_mem,
    .capacity = SPK_SPSC_CAP,
    .head = ATOMIC_VAR_INIT(0),
    .tail = ATOMIC_VAR_INIT(0),
    .drops = ATOMIC_VAR_INIT(0),
};

static atomic_uint_fast8_t  s_spk_alt = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_rx_cb_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_rx_bytes_total = ATOMIC_VAR_INIT(0);

static inline size_t spsc_push(spsc_byte_buf_t *rb, const uint8_t *src, size_t len) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t mask = rb->capacity - 1u;
    size_t used = (head - tail) & mask;
    size_t avail = (rb->capacity - 1u) - used;
    size_t to_write = (len < avail) ? len : avail;

    for (size_t i = 0; i < to_write; i++) {
        rb->buf[(head + i) & mask] = src[i];
    }
    atomic_store_explicit(&rb->head, (head + to_write) & mask, memory_order_release);

    if (to_write < len) {
        atomic_fetch_add_explicit(&rb->drops, (uint_fast32_t)(len - to_write), memory_order_relaxed);
    }
    return to_write;
}

static inline size_t spsc_pop(spsc_byte_buf_t *rb, uint8_t *dst, size_t len) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t mask = rb->capacity - 1u;
    size_t used = (head - tail) & mask;
    size_t to_read = (len < used) ? len : used;

    for (size_t i = 0; i < to_read; i++) {
        dst[i] = rb->buf[(tail + i) & mask];
    }
    atomic_store_explicit(&rb->tail, (tail + to_read) & mask, memory_order_release);
    return to_read;
}

static inline void spsc_reset(spsc_byte_buf_t *rb) {
    atomic_store_explicit(&rb->tail, atomic_load_explicit(&rb->head, memory_order_acquire), memory_order_release);
}

void usb_audio_init(void) {
    atomic_store(&s_spk_alt, 0);
    spsc_reset(&s_spk_buf);
    ESP_LOGI(TAG, "UAC1 speaker-only ready (EP 0x%02X, %uB, 48kHz 16-bit stereo)",
             USB_UAC1_SPK_EP_OUT, (unsigned)USB_UAC1_SPK_MPS);
}

void usb_audio_get_stats(usb_audio_stats_t *out_stats) {
    if (!out_stats) return;
    out_stats->rx_cb_count = (uint32_t)atomic_load(&s_rx_cb_count);
    out_stats->rx_bytes_total = (uint32_t)atomic_load(&s_rx_bytes_total);
    out_stats->rx_drop_bytes = (uint32_t)atomic_load(&s_spk_buf.drops);
    out_stats->spk_alt = (uint8_t)atomic_load(&s_spk_alt);
}

bool usb_audio_spk_active(void) {
    return atomic_load(&s_spk_alt) != 0;
}

size_t usb_audio_read_spk_pcm(int16_t *stereo_out, size_t max_frames) {
    if (!stereo_out || max_frames == 0) return 0;
    size_t frame_bytes = 2u * sizeof(int16_t);
    size_t got = spsc_pop(&s_spk_buf, (uint8_t *)stereo_out, max_frames * frame_bytes);
    return got / frame_bytes;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (!p_request) return false;
    uint8_t itf = tu_u16_low(p_request->wIndex);
    uint8_t alt = tu_u16_low(p_request->wValue);
    if (itf == USB_ITF_AUDIO_STREAMING_SPK) {
        atomic_store(&s_spk_alt, alt);
        if (alt == 0) spsc_reset(&s_spk_buf);
        return true;
    }
    return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (!p_request) return false;
    if (tu_u16_low(p_request->wIndex) == USB_ITF_AUDIO_STREAMING_SPK) {
        atomic_store(&s_spk_alt, 0);
        spsc_reset(&s_spk_buf);
    }
    return true;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff) {
    (void)rhport; (void)p_request; (void)pBuff;
    return true;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    if (!p_request) return false;
    if (tu_u16_high(p_request->wValue) == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        static uint8_t freq_48k[3] = { 0x80u, 0xBBu, 0x00u };
        return tud_control_xfer(rhport, p_request, freq_48k, sizeof(freq_48k));
    }
    return false;
}

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    uint8_t tmp[USB_UAC1_SPK_MPS];
    if (n_bytes_received > 0) {
        uint16_t to_read = (n_bytes_received > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : n_bytes_received;
        uint16_t r = tud_audio_read(tmp, to_read);
        if (r > 0) {
            spsc_push(&s_spk_buf, tmp, r);
            atomic_fetch_add_explicit(&s_rx_cb_count, 1u, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_rx_bytes_total, r, memory_order_relaxed);
        }
    }
    return true;
}
