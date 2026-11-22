#include "usb_audio.h"
#include "stadia_opus.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "class/audio/audio_device.h"

#define TAG "USB_AUDIO"

/* Power-of-2 lock-free SPSC byte buffer for USB Speaker OUT (48kHz stereo s16le = 192B/ms) */
#define SPK_SPSC_CAP          8192u /* ~42 ms */
#define SPK_OPUS_FRAME_BYTES  (STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS * sizeof(int16_t)) /* 3840 B */

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
static atomic_uint_fast8_t  s_mic_alt = ATOMIC_VAR_INIT(0);
static atomic_bool          s_spk_flush = ATOMIC_VAR_INIT(false);
static atomic_bool          s_mic_primed = ATOMIC_VAR_INIT(false);
static atomic_bool          s_mic_muted = ATOMIC_VAR_INIT(false);

static atomic_uint_fast32_t s_rx_cb_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_rx_bytes_total = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_tx_cb_count = ATOMIC_VAR_INIT(0);
static atomic_uint_fast32_t s_tx_underruns = ATOMIC_VAR_INIT(0);
static uint16_t             s_spk_rx_since_notify = 0;

static inline size_t spsc_push(spsc_byte_buf_t *rb, const uint8_t *src, size_t len) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t mask = rb->capacity - 1u;
    size_t used = (head - tail) & mask;
    size_t avail = (rb->capacity - 1u) - used;
    size_t to_write = (len < avail) ? len : avail;

    /* Always keep writes aligned to 4-byte stereo s16le frames */
    to_write &= ~((size_t)3u);

    if (to_write > 0) {
        size_t first = rb->capacity - head;
        if (first > to_write) first = to_write;
        memcpy(rb->buf + head, src, first);
        if (to_write > first) {
            memcpy(rb->buf, src + first, to_write - first);
        }
        atomic_store_explicit(&rb->head, (head + to_write) & mask, memory_order_release);
    }

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

    to_read &= ~((size_t)3u);

    if (to_read > 0) {
        size_t first = rb->capacity - tail;
        if (first > to_read) first = to_read;
        memcpy(dst, rb->buf + tail, first);
        if (to_read > first) {
            memcpy(dst + first, rb->buf, to_read - first);
        }
        atomic_store_explicit(&rb->tail, (tail + to_read) & mask, memory_order_release);
    }
    return to_read;
}

/* Called only from the consumer thread (opus_worker_task) */
static inline void spsc_consumer_flush(spsc_byte_buf_t *rb) {
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    atomic_store_explicit(&rb->tail, head, memory_order_release);
}

/* Push exactly 1 ms (48 mono s16le samples = 96 bytes) into TinyUSB EP IN FIFO.
 * ISR-safe: no float, no logging, no blocking, lock-free 2-chunk memcpy SPSC read only. */
static void push_mic_tx_1ms(void) {
    enum { SAMPLES_1MS = 48 };
    int16_t pcm[SAMPLES_1MS];

    size_t avail = stadia_opus_mic_avail();
    bool primed = atomic_load_explicit(&s_mic_primed, memory_order_relaxed);

    if (!primed) {
        /* 10 ms (480 samples) cushion: half-frame low latency with zero startup stutter */
        if (avail >= (STADIA_FRAME_SAMPLES / 2u)) {
            primed = true;
            atomic_store_explicit(&s_mic_primed, true, memory_order_relaxed);
        }
    }

    if (primed && avail >= SAMPLES_1MS) {
        /* Gentle clock-drift slip: if >40 ms queued, drop 1 extra sample */
        if (avail > (STADIA_FRAME_SAMPLES * 2u)) {
            int16_t discard;
            (void)stadia_opus_read_mic_pcm(&discard, 1u);
        }
        size_t got = stadia_opus_read_mic_pcm(pcm, SAMPLES_1MS);
        if (atomic_load_explicit(&s_mic_muted, memory_order_relaxed)) {
            memset(pcm, 0, sizeof(pcm));
        } else if (got < SAMPLES_1MS) {
            memset(pcm + got, 0, (SAMPLES_1MS - got) * sizeof(int16_t));
        }
    } else {
        if (primed) {
            atomic_store_explicit(&s_mic_primed, false, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_tx_underruns, 1u, memory_order_relaxed);
        }
        memset(pcm, 0, sizeof(pcm));
    }

    (void)tud_audio_write(pcm, (uint16_t)sizeof(pcm));
}

void usb_audio_init(void) {
    atomic_store(&s_spk_alt, 0);
    atomic_store(&s_mic_alt, 0);
    atomic_store(&s_spk_flush, false);
    atomic_store(&s_mic_primed, false);
    s_spk_rx_since_notify = 0;
    spsc_consumer_flush(&s_spk_buf);
    ESP_LOGI(TAG, "UAC1 full-duplex ready: SPK EP 0x%02X (%uB 48kHz stereo) + MIC EP 0x%02X (%uB 48kHz mono)",
             USB_UAC1_SPK_EP_OUT, (unsigned)USB_UAC1_SPK_MPS,
             USB_UAC1_MIC_EP_IN, (unsigned)USB_UAC1_MIC_MPS);
}

void usb_audio_get_stats(usb_audio_stats_t *out_stats) {
    if (!out_stats) return;
    out_stats->rx_cb_count = (uint32_t)atomic_load(&s_rx_cb_count);
    out_stats->rx_bytes_total = (uint32_t)atomic_load(&s_rx_bytes_total);
    out_stats->rx_drop_bytes = (uint32_t)atomic_load(&s_spk_buf.drops);
    out_stats->tx_cb_count = (uint32_t)atomic_load(&s_tx_cb_count);
    out_stats->tx_underruns = (uint32_t)atomic_load(&s_tx_underruns);
    out_stats->spk_alt = (uint8_t)atomic_load(&s_spk_alt);
    out_stats->mic_alt = (uint8_t)atomic_load(&s_mic_alt);
}

bool usb_audio_spk_active(void) {
    return atomic_load_explicit(&s_spk_alt, memory_order_acquire) != 0;
}

bool usb_audio_mic_active(void) {
    return atomic_load_explicit(&s_mic_alt, memory_order_acquire) != 0;
}

void usb_audio_set_mic_muted(bool muted) {
    atomic_store_explicit(&s_mic_muted, muted, memory_order_release);
}

bool usb_audio_is_mic_muted(void) {
    return atomic_load_explicit(&s_mic_muted, memory_order_acquire);
}

size_t usb_audio_read_spk_pcm(int16_t *stereo_out, size_t max_frames) {
    if (!stereo_out || max_frames == 0) return 0;
    if (atomic_exchange_explicit(&s_spk_flush, false, memory_order_acq_rel)) {
        spsc_consumer_flush(&s_spk_buf);
        return 0;
    }
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
        s_spk_rx_since_notify = 0;
        atomic_store_explicit(&s_spk_alt, alt, memory_order_release);
        if (alt == 0) {
            atomic_store_explicit(&s_spk_flush, true, memory_order_release);
            stadia_opus_notify();
        }
        return true;
    }

    if (itf == USB_ITF_AUDIO_STREAMING_MIC) {
        atomic_store_explicit(&s_mic_primed, false, memory_order_release);
        stadia_opus_mic_reset();
        atomic_store_explicit(&s_mic_alt, alt, memory_order_release);
        if (alt != 0) {
            push_mic_tx_1ms();
        }
        return true;
    }

    return false;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    if (!p_request) return false;
    uint8_t itf = tu_u16_low(p_request->wIndex);
    if (itf == USB_ITF_AUDIO_STREAMING_SPK) {
        s_spk_rx_since_notify = 0;
        atomic_store_explicit(&s_spk_alt, 0, memory_order_release);
        atomic_store_explicit(&s_spk_flush, true, memory_order_release);
        stadia_opus_notify();
    } else if (itf == USB_ITF_AUDIO_STREAMING_MIC) {
        atomic_store_explicit(&s_mic_alt, 0, memory_order_release);
        atomic_store_explicit(&s_mic_primed, false, memory_order_release);
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

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting) {
    (void)rhport; (void)func_id; (void)ep_out;
    if (cur_alt_setting == 0 || n_bytes_received == 0) return true;

    uint8_t tmp[USB_UAC1_SPK_MPS];
    uint16_t to_read = (n_bytes_received > sizeof(tmp)) ? (uint16_t)sizeof(tmp) : n_bytes_received;
    uint16_t r = tud_audio_read(tmp, to_read);
    if (r > 0) {
        size_t pushed = spsc_push(&s_spk_buf, tmp, r);
        atomic_fetch_add_explicit(&s_rx_cb_count, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_rx_bytes_total, r, memory_order_relaxed);

        /* Wake Core 1 Opus encoder exactly once per 20 ms frame (3840 bytes) */
        s_spk_rx_since_notify += (uint16_t)pushed;
        if (s_spk_rx_since_notify >= SPK_OPUS_FRAME_BYTES) {
            s_spk_rx_since_notify -= SPK_OPUS_FRAME_BYTES;
            stadia_opus_notify_from_isr();
        }
    }
    return true;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id,
                           uint8_t ep_in, uint8_t cur_alt_setting) {
    (void)rhport; (void)n_bytes_sent; (void)func_id; (void)ep_in;
    if (cur_alt_setting == 0) return true;

    atomic_fetch_add_explicit(&s_tx_cb_count, 1u, memory_order_relaxed);
    push_mic_tx_1ms();
    return true;
}
