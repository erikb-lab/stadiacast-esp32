#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tusb.h"

enum {
    USB_ITF_HID = 0,
    USB_ITF_AUDIO_CONTROL = 1,
    USB_ITF_AUDIO_STREAMING_SPK = 2,
    USB_ITF_TOTAL = 3,
};

#define USB_UAC1_SPK_IT_ID  0x01
#define USB_UAC1_SPK_OT_ID  0x03

#define USB_UAC1_SPK_EP_OUT 0x02u
#define USB_UAC1_SPK_MPS    192u

typedef struct {
    uint32_t rx_cb_count;
    uint32_t rx_bytes_total;
    uint32_t rx_drop_bytes;
    uint8_t  spk_alt;
} usb_audio_stats_t;

void usb_audio_init(void);
void usb_audio_get_stats(usb_audio_stats_t *out_stats);
bool usb_audio_spk_active(void);
size_t usb_audio_read_spk_pcm(int16_t *stereo_out, size_t max_frames);
