/* Core 1 full-duplex Opus encoder (48kHz stereo headset) & decoder (48kHz mono mic). */
#include "stadia_opus.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "opus.h"

#define TAG "STADIA-OPUS"

#define PKT_RING_SLOTS  6
#define PCM_RING_FRAMES 4

typedef struct {
    uint16_t len;
    uint8_t  data[STADIA_MAX_OPUS_PKT_BYTES];
} opus_slot_t;

/* Lock-free SPSC packet rings */
static opus_slot_t   s_rx_ring[PKT_RING_SLOTS];
static atomic_uint   s_rx_head;
static atomic_uint   s_rx_tail;

static opus_slot_t   s_tx_ring[PKT_RING_SLOTS];
static atomic_uint   s_tx_head;
static atomic_uint   s_tx_tail;

/* Lock-free SPSC PCM sample rings */
#define SPK_RING_CAP (PCM_RING_FRAMES * STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS)
#define MIC_RING_CAP (PCM_RING_FRAMES * STADIA_FRAME_SAMPLES * STADIA_MIC_CHANNELS)

static int16_t       s_spk_pcm[SPK_RING_CAP];
static atomic_uint   s_spk_head;
static atomic_uint   s_spk_tail;

static int16_t       s_mic_pcm[MIC_RING_CAP];
static atomic_uint   s_mic_head;
static atomic_uint   s_mic_tail;

static atomic_bool   s_loopback = true;

/* Scratch frame buffers for Core 1 worker */
static int16_t s_dec_mono[STADIA_FRAME_SAMPLES];
static int16_t s_enc_stereo[STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS];
static uint8_t s_enc_pkt[STADIA_MAX_OPUS_PKT_BYTES];

void stadia_opus_set_loopback(bool enable) {
    atomic_store(&s_loopback, enable);
}

bool stadia_opus_is_loopback(void) {
    return atomic_load(&s_loopback);
}

bool stadia_opus_push_rx_packet(const uint8_t *opus_data, size_t len) {
    if (!opus_data || len == 0 || len > STADIA_MAX_OPUS_PKT_BYTES) return false;
    unsigned h = atomic_load_explicit(&s_rx_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_rx_tail, memory_order_acquire);
    if ((h - t) >= PKT_RING_SLOTS) return false;
    opus_slot_t *slot = &s_rx_ring[h % PKT_RING_SLOTS];
    slot->len = (uint16_t)len;
    memcpy(slot->data, opus_data, len);
    atomic_store_explicit(&s_rx_head, h + 1u, memory_order_release);
    return true;
}

static bool pop_rx_packet(opus_slot_t *out) {
    unsigned t = atomic_load_explicit(&s_rx_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_rx_head, memory_order_acquire);
    if (h == t) return false;
    *out = s_rx_ring[t % PKT_RING_SLOTS];
    atomic_store_explicit(&s_rx_tail, t + 1u, memory_order_release);
    return true;
}

static bool push_tx_packet(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > STADIA_MAX_OPUS_PKT_BYTES) return false;
    unsigned h = atomic_load_explicit(&s_tx_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_tx_tail, memory_order_acquire);
    if ((h - t) >= PKT_RING_SLOTS) return false;
    opus_slot_t *slot = &s_tx_ring[h % PKT_RING_SLOTS];
    slot->len = (uint16_t)len;
    memcpy(slot->data, data, len);
    atomic_store_explicit(&s_tx_head, h + 1u, memory_order_release);
    return true;
}

bool stadia_opus_pop_tx_packet(uint8_t *opus_out, size_t max_len, size_t *out_len) {
    unsigned t = atomic_load_explicit(&s_tx_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_tx_head, memory_order_acquire);
    if (h == t) return false;
    opus_slot_t *slot = &s_tx_ring[t % PKT_RING_SLOTS];
    if (slot->len > max_len) return false;
    memcpy(opus_out, slot->data, slot->len);
    if (out_len) *out_len = slot->len;
    atomic_store_explicit(&s_tx_tail, t + 1u, memory_order_release);
    return true;
}

size_t stadia_opus_write_speaker_pcm(const int16_t *stereo_samples, size_t frame_count) {
    size_t n = frame_count * STADIA_SPK_CHANNELS;
    unsigned h = atomic_load_explicit(&s_spk_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_spk_tail, memory_order_acquire);
    size_t avail = SPK_RING_CAP - (h - t);
    if (n > avail) n = avail - (avail % STADIA_SPK_CHANNELS);
    for (size_t i = 0; i < n; i++) {
        s_spk_pcm[(h + i) % SPK_RING_CAP] = stereo_samples[i];
    }
    atomic_store_explicit(&s_spk_head, h + (unsigned)n, memory_order_release);
    return n / STADIA_SPK_CHANNELS;
}

static bool read_speaker_frame(int16_t *dst_stereo) {
    const size_t need = STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS;
    unsigned t = atomic_load_explicit(&s_spk_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_spk_head, memory_order_acquire);
    if ((h - t) < need) return false;
    for (size_t i = 0; i < need; i++) {
        dst_stereo[i] = s_spk_pcm[(t + i) % SPK_RING_CAP];
    }
    atomic_store_explicit(&s_spk_tail, t + (unsigned)need, memory_order_release);
    return true;
}

static void write_mic_frame(const int16_t *src_mono, size_t samples) {
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_mic_tail, memory_order_acquire);
    size_t avail = MIC_RING_CAP - (h - t);
    if (samples > avail) samples = avail;
    for (size_t i = 0; i < samples; i++) {
        s_mic_pcm[(h + i) % MIC_RING_CAP] = src_mono[i];
    }
    atomic_store_explicit(&s_mic_head, h + (unsigned)samples, memory_order_release);
}

size_t stadia_opus_read_mic_pcm(int16_t *mono_out, size_t max_samples) {
    unsigned t = atomic_load_explicit(&s_mic_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_acquire);
    size_t avail = h - t;
    size_t n = avail < max_samples ? avail : max_samples;
    for (size_t i = 0; i < n; i++) {
        mono_out[i] = s_mic_pcm[(t + i) % MIC_RING_CAP];
    }
    atomic_store_explicit(&s_mic_tail, t + (unsigned)n, memory_order_release);
    return n;
}

static void opus_worker_task(void *arg) {
    (void)arg;
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(STADIA_AUDIO_RATE, STADIA_MIC_CHANNELS, &err);
    if (!dec || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    OpusEncoder *enc = opus_encoder_create(STADIA_AUDIO_RATE, STADIA_SPK_CHANNELS,
                                           OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %d", err);
        opus_decoder_destroy(dec);
        vTaskDelete(NULL);
        return;
    }

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(1));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    ESP_LOGI(TAG, "Core 1 Opus full-duplex worker ready (48kHz mono dec + stereo enc)");

    opus_slot_t rx_pkt;
    uint32_t frames_done = 0;
    int64_t total_us = 0;

    while (1) {
        bool did_work = false;

        /* 1. Decode incoming controller mic Opus packet -> 48kHz mono PCM */
        if (pop_rx_packet(&rx_pkt)) {
            int64_t t0 = esp_timer_get_time();
            int ns = opus_decode(dec, rx_pkt.data, rx_pkt.len,
                                 s_dec_mono, STADIA_FRAME_SAMPLES, 0);
            if (ns > 0) {
                if (atomic_load(&s_loopback)) {
                    /* Phase 6B-1 loopback: mono PCM -> stereo PCM -> re-encode Opus */
                    for (int i = 0; i < ns; i++) {
                        s_enc_stereo[2 * i]     = s_dec_mono[i];
                        s_enc_stereo[2 * i + 1] = s_dec_mono[i];
                    }
                    int elen = opus_encode(enc, s_enc_stereo, STADIA_FRAME_SAMPLES,
                                           s_enc_pkt, sizeof(s_enc_pkt));
                    if (elen > 0) {
                        push_tx_packet(s_enc_pkt, (size_t)elen);
                    }
                } else {
                    write_mic_frame(s_dec_mono, (size_t)ns);
                }
                total_us += (esp_timer_get_time() - t0);
                frames_done++;
                if ((frames_done % 250u) == 0u) {
                    ESP_LOGI(TAG, "Core 1 Opus avg frame time: %lld us / 20000 us",
                             (long long)(total_us / 250));
                    total_us = 0;
                }
            }
            did_work = true;
        }

        /* 2. In USB Audio mode, encode queued 20ms stereo speaker PCM -> Opus */
        if (!atomic_load(&s_loopback) && read_speaker_frame(s_enc_stereo)) {
            int elen = opus_encode(enc, s_enc_stereo, STADIA_FRAME_SAMPLES,
                                   s_enc_pkt, sizeof(s_enc_pkt));
            if (elen > 0) {
                push_tx_packet(s_enc_pkt, (size_t)elen);
            }
            did_work = true;
        }

        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

void stadia_opus_init(void) {
    xTaskCreatePinnedToCore(opus_worker_task, "opus_audio", 16384, NULL, 5, NULL, 1);
}
