/* Core 1 event-driven full-duplex Opus encoder (48kHz stereo headset) & decoder (48kHz mono mic). */
#include "stadia_opus.h"
#include "usb_audio.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "nvs.h"
#include "opus.h"

#define TAG "STADIA-OPUS"
#define NVS_NS_AUDIO       "stadiacast"
#define NVS_KEY_MIC_GAIN   "mic_gain"

extern void stadia_transport_wake(void);

#define PKT_RING_SLOTS  8u
#define PKT_RING_MASK   (PKT_RING_SLOTS - 1u)

/* Power-of-2 PCM sample rings (2-chunk memcpy, wrap-safe forever, ISR-safe for mic consumer) */
#define SPK_RING_CAP    8192u  /* 4096 stereo frames (~85 ms @ 48kHz) */
#define SPK_RING_MASK   (SPK_RING_CAP - 1u)
#define MIC_RING_CAP    4096u  /* 4096 mono samples  (~85 ms @ 48kHz) */
#define MIC_RING_MASK   (MIC_RING_CAP - 1u)

typedef struct {
    uint16_t len;
    uint8_t  data[STADIA_MAX_OPUS_PKT_BYTES];
} opus_slot_t;

/* Lock-free SPSC packet rings (Core 0 <-> Core 1) in OPI PSRAM to save internal DRAM */
static EXT_RAM_BSS_ATTR opus_slot_t s_rx_ring[PKT_RING_SLOTS];
static atomic_uint   s_rx_head = ATOMIC_VAR_INIT(0);
static atomic_uint   s_rx_tail = ATOMIC_VAR_INIT(0);

static EXT_RAM_BSS_ATTR opus_slot_t s_tx_ring[PKT_RING_SLOTS];
static atomic_uint   s_tx_head = ATOMIC_VAR_INIT(0);
static atomic_uint   s_tx_tail = ATOMIC_VAR_INIT(0);

/* Lock-free SPSC PCM sample rings */
static EXT_RAM_BSS_ATTR int16_t s_spk_pcm[SPK_RING_CAP];
static atomic_uint   s_spk_head = ATOMIC_VAR_INIT(0);
static atomic_uint   s_spk_tail = ATOMIC_VAR_INIT(0);

static int16_t       s_mic_pcm[MIC_RING_CAP];
static atomic_uint   s_mic_head = ATOMIC_VAR_INIT(0);
static atomic_uint   s_mic_tail = ATOMIC_VAR_INIT(0);

static atomic_bool          s_loopback = ATOMIC_VAR_INIT(false);
static atomic_uint_fast16_t s_mic_gain_q8 = ATOMIC_VAR_INIT(256u); /* 256 = 1.0x */
static TaskHandle_t         s_opus_task = NULL;

/* Scratch frame buffers for Core 1 worker */
static EXT_RAM_BSS_ATTR int16_t s_dec_mono[STADIA_FRAME_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t s_enc_stereo[STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS];
static EXT_RAM_BSS_ATTR uint8_t s_enc_pkt[STADIA_MAX_OPUS_PKT_BYTES];

void stadia_opus_set_loopback(bool enable) {
    atomic_store(&s_loopback, enable);
}

bool stadia_opus_is_loopback(void) {
    return atomic_load(&s_loopback);
}

void stadia_opus_set_mic_gain_q8(uint16_t gain_q8) {
    if (gain_q8 < 128u) gain_q8 = 128u;
    if (gain_q8 > 768u) gain_q8 = 768u;
    atomic_store_explicit(&s_mic_gain_q8, gain_q8, memory_order_release);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AUDIO, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u16(h, NVS_KEY_MIC_GAIN, gain_q8) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

uint16_t stadia_opus_get_mic_gain_q8(void) {
    return (uint16_t)atomic_load_explicit(&s_mic_gain_q8, memory_order_acquire);
}

void stadia_opus_notify(void) {
    TaskHandle_t t = s_opus_task;
    if (t) xTaskNotifyGive(t);
}

void stadia_opus_notify_from_isr(void) {
    TaskHandle_t t = s_opus_task;
    if (t) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(t, &hpw);
        if (hpw) portYIELD_FROM_ISR();
    }
}

bool stadia_opus_push_rx_packet(const uint8_t *opus_data, size_t len) {
    if (!opus_data || len == 0 || len > STADIA_MAX_OPUS_PKT_BYTES) return false;
    unsigned h = atomic_load_explicit(&s_rx_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_rx_tail, memory_order_acquire);
    if ((h - t) >= PKT_RING_SLOTS) return false;
    opus_slot_t *slot = &s_rx_ring[h & PKT_RING_MASK];
    slot->len = (uint16_t)len;
    memcpy(slot->data, opus_data, len);
    atomic_store_explicit(&s_rx_head, h + 1u, memory_order_release);
    stadia_opus_notify();
    return true;
}

static bool pop_rx_packet(opus_slot_t *out) {
    unsigned t = atomic_load_explicit(&s_rx_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_rx_head, memory_order_acquire);
    if (h == t) return false;
    *out = s_rx_ring[t & PKT_RING_MASK];
    atomic_store_explicit(&s_rx_tail, t + 1u, memory_order_release);
    return true;
}

static bool push_tx_packet(const uint8_t *data, size_t len) {
    if (!data || len == 0 || len > STADIA_MAX_OPUS_PKT_BYTES) return false;
    unsigned h = atomic_load_explicit(&s_tx_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_tx_tail, memory_order_acquire);
    if ((h - t) >= PKT_RING_SLOTS) return false;
    opus_slot_t *slot = &s_tx_ring[h & PKT_RING_MASK];
    slot->len = (uint16_t)len;
    memcpy(slot->data, data, len);
    atomic_store_explicit(&s_tx_head, h + 1u, memory_order_release);
    stadia_transport_wake();
    return true;
}

bool stadia_opus_pop_tx_packet(uint8_t *opus_out, size_t max_len, size_t *out_len) {
    unsigned t = atomic_load_explicit(&s_tx_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_tx_head, memory_order_acquire);
    if (h == t) return false;
    opus_slot_t *slot = &s_tx_ring[t & PKT_RING_MASK];
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
    if (n > avail) n = avail & ~((size_t)1u);
    if (n == 0) return 0;

    size_t pos = h & SPK_RING_MASK;
    size_t first = SPK_RING_CAP - pos;
    if (first > n) first = n;
    memcpy(&s_spk_pcm[pos], stereo_samples, first * sizeof(int16_t));
    if (n > first) {
        memcpy(&s_spk_pcm[0], stereo_samples + first, (n - first) * sizeof(int16_t));
    }
    atomic_store_explicit(&s_spk_head, h + (unsigned)n, memory_order_release);
    return n / STADIA_SPK_CHANNELS;
}

static bool read_speaker_frame(int16_t *dst_stereo) {
    const size_t need = STADIA_FRAME_SAMPLES * STADIA_SPK_CHANNELS;
    unsigned t = atomic_load_explicit(&s_spk_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_spk_head, memory_order_acquire);
    unsigned used = h - t;
    if (used < need) return false;
    /* Cap speaker latency to <= 40 ms (2 frames) */
    while (used > (need * 2u)) {
        t += (unsigned)need;
        used -= (unsigned)need;
    }
    size_t pos = t & SPK_RING_MASK;
    size_t first = SPK_RING_CAP - pos;
    if (first > need) first = need;
    memcpy(dst_stereo, &s_spk_pcm[pos], first * sizeof(int16_t));
    if (need > first) {
        memcpy(dst_stereo + first, &s_spk_pcm[0], (need - first) * sizeof(int16_t));
    }
    atomic_store_explicit(&s_spk_tail, t + (unsigned)need, memory_order_release);
    return true;
}

static void write_mic_frame(const int16_t *src_mono, size_t samples) {
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_mic_tail, memory_order_acquire);
    size_t avail = MIC_RING_CAP - (h - t);
    if (samples > avail) samples = avail;
    if (samples == 0) return;

    size_t pos = h & MIC_RING_MASK;
    size_t first = MIC_RING_CAP - pos;
    if (first > samples) first = samples;
    memcpy(&s_mic_pcm[pos], src_mono, first * sizeof(int16_t));
    if (samples > first) {
        memcpy(&s_mic_pcm[0], src_mono + first, (samples - first) * sizeof(int16_t));
    }
    atomic_store_explicit(&s_mic_head, h + (unsigned)samples, memory_order_release);
}

size_t stadia_opus_mic_avail(void) {
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&s_mic_tail, memory_order_relaxed);
    return (size_t)(h - t);
}

void stadia_opus_mic_reset(void) {
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_acquire);
    atomic_store_explicit(&s_mic_tail, h, memory_order_release);
}

size_t stadia_opus_read_mic_pcm(int16_t *mono_out, size_t max_samples) {
    unsigned t = atomic_load_explicit(&s_mic_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&s_mic_head, memory_order_acquire);
    size_t avail = (size_t)(h - t);
    size_t n = (avail < max_samples) ? avail : max_samples;
    if (n == 0) return 0;

    size_t pos = t & MIC_RING_MASK;
    size_t first = MIC_RING_CAP - pos;
    if (first > n) first = n;
    memcpy(mono_out, &s_mic_pcm[pos], first * sizeof(int16_t));
    if (n > first) {
        memcpy(mono_out + first, &s_mic_pcm[0], (n - first) * sizeof(int16_t));
    }
    atomic_store_explicit(&s_mic_tail, t + (unsigned)n, memory_order_release);
    return n;
}

static void opus_worker_task(void *arg) {
    (void)arg;
    int err = 0;

    int dec_sz = opus_decoder_get_size(STADIA_MIC_CHANNELS);
    OpusDecoder *dec = (OpusDecoder *)heap_caps_calloc(1, (size_t)dec_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec) dec = (OpusDecoder *)heap_caps_calloc(1, (size_t)dec_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    err = dec ? opus_decoder_init(dec, STADIA_AUDIO_RATE, STADIA_MIC_CHANNELS) : OPUS_ALLOC_FAIL;
    if (!dec || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_init failed: %d", err);
        if (dec) free(dec);
        vTaskDelete(NULL);
        return;
    }

    int enc_sz = opus_encoder_get_size(STADIA_SPK_CHANNELS);
    OpusEncoder *enc = (OpusEncoder *)heap_caps_calloc(1, (size_t)enc_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc) enc = (OpusEncoder *)heap_caps_calloc(1, (size_t)enc_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    err = enc ? opus_encoder_init(enc, STADIA_AUDIO_RATE, STADIA_SPK_CHANNELS,
                                  OPUS_APPLICATION_RESTRICTED_LOWDELAY) : OPUS_ALLOC_FAIL;
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_init failed: %d", err);
        free(dec);
        if (enc) free(enc);
        vTaskDelete(NULL);
        return;
    }

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    /* Complexity 0 in RESTRICTED_LOWDELAY (CELT-only): ~35% less CPU/heat at same 48kHz stereo quality */
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    ESP_LOGI(TAG, "Core 1 event-driven Opus worker ready (48kHz mono dec + stereo CELT enc, c=0)");

    opus_slot_t rx_pkt;
    bool prev_spk_active = false;

    while (1) {
        bool did_work = false;
        bool loopback = atomic_load_explicit(&s_loopback, memory_order_relaxed);

        /* 1. Drain all queued incoming controller mic Opus packets -> 48kHz mono PCM */
        while (pop_rx_packet(&rx_pkt)) {
            if (loopback || usb_audio_mic_active()) {
                int ns = opus_decode(dec, rx_pkt.data, rx_pkt.len,
                                     s_dec_mono, STADIA_FRAME_SAMPLES, 0);
                if (ns > 0) {
                    if (loopback) {
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
                        uint16_t g = (uint16_t)atomic_load_explicit(&s_mic_gain_q8, memory_order_relaxed);
                        if (g != 256u) {
                            for (int i = 0; i < ns; i++) {
                                int32_t v = ((int32_t)s_dec_mono[i] * (int32_t)g) >> 8;
                                if (v > 32767) v = 32767;
                                else if (v < -32768) v = -32768;
                                s_dec_mono[i] = (int16_t)v;
                            }
                        }
                        write_mic_frame(s_dec_mono, (size_t)ns);
                    }
                }
            }
            did_work = true;
        }

        /* 2. Drain USB UAC1 speaker PCM and encode 20 ms stereo frames -> Opus */
        if (!loopback) {
            bool spk_active = usb_audio_spk_active();
            if (!spk_active && prev_spk_active) {
                unsigned h = atomic_load_explicit(&s_spk_head, memory_order_relaxed);
                atomic_store_explicit(&s_spk_tail, h, memory_order_release);
            }
            prev_spk_active = spk_active;

            int16_t usb_tmp[256 * STADIA_SPK_CHANNELS];
            size_t got;
            while ((got = usb_audio_read_spk_pcm(usb_tmp, 256)) > 0) {
                if (spk_active) {
                    stadia_opus_write_speaker_pcm(usb_tmp, got);
                }
            }

            while (read_speaker_frame(s_enc_stereo)) {
                int elen = opus_encode(enc, s_enc_stereo, STADIA_FRAME_SAMPLES,
                                       s_enc_pkt, sizeof(s_enc_pkt));
                if (elen > 0) {
                    push_tx_packet(s_enc_pkt, (size_t)elen);
                }
                did_work = true;
            }
        }

        if (!did_work) {
            /* Sleep at 0% CPU until woken immediately by mic RX or 20ms speaker frame ready */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        }
    }
}

void stadia_opus_init(void) {
    nvs_handle_t h;
    uint16_t saved_gain = 256u;
    if (nvs_open(NVS_NS_AUDIO, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u16(h, NVS_KEY_MIC_GAIN, &saved_gain);
        nvs_close(h);
    }
    if (saved_gain >= 128u && saved_gain <= 768u) {
        atomic_store_explicit(&s_mic_gain_q8, saved_gain, memory_order_release);
    }
    xTaskCreatePinnedToCore(opus_worker_task, "opus_audio", 16384, NULL, 3, &s_opus_task, 1);
}
