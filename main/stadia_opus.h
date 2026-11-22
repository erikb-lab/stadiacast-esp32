/* Core 1 full-duplex Opus encoder (48kHz stereo headset) & decoder (48kHz mono mic). */
#ifndef STADIA_OPUS_H
#define STADIA_OPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STADIA_AUDIO_RATE          48000
#define STADIA_FRAME_SAMPLES       960   /* 20 ms @ 48 kHz */
#define STADIA_SPK_CHANNELS        2     /* Stereo headset output */
#define STADIA_MIC_CHANNELS        1     /* Mono controller mic input */
#define STADIA_MAX_OPUS_PKT_BYTES  320

void stadia_opus_init(void);
void stadia_opus_set_loopback(bool enable);
bool stadia_opus_is_loopback(void);

/* Wake Core 1 Opus worker immediately (task or ISR context) */
void stadia_opus_notify(void);
void stadia_opus_notify_from_isr(void);

/* Core 0 (SRTP RX) -> Core 1 (Opus decoder) */
bool stadia_opus_push_rx_packet(const uint8_t *opus_data, size_t len);

/* Core 1 (Opus encoder) -> Core 0 (SRTP TX) */
bool stadia_opus_pop_tx_packet(uint8_t *opus_out, size_t max_len, size_t *out_len);

/* USB UAC1 Speaker (48kHz stereo s16le) -> Core 1 (Opus encoder) */
size_t stadia_opus_write_speaker_pcm(const int16_t *stereo_samples, size_t frame_count);

/* Core 1 (Opus decoder) -> USB UAC1 Mic ISR (48kHz mono s16le, ISR-safe lock-free SPSC) */
size_t stadia_opus_mic_avail(void);
void   stadia_opus_mic_reset(void);
size_t stadia_opus_read_mic_pcm(int16_t *mono_out, size_t max_samples);

#endif /* STADIA_OPUS_H */
