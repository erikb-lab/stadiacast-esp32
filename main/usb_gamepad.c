/* Phase 4/5: Wired Google Stadia Controller (18d1:9400) USB HID + Rumble (Report 5). */
#include "usb_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "class/hid/hid_device.h"
#include "device/usbd_pvt.h"
#include "usb_audio.h"

#define TAG "USB-STADIA"

#define INPUT_REPORT_ID     3
#define RUMBLE_REPORT_ID    5
#define TRIGGER_THRESHOLD   30
#define UAC1_IAD_DESC_LEN 8
#define UAC1_AUDIO_DESC_LEN ( \
    UAC1_IAD_DESC_LEN + \
    TUD_AUDIO10_DESC_STD_AC_LEN + \
    TUD_AUDIO10_DESC_CS_AC_LEN(2) + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN + \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN + \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN )
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + UAC1_AUDIO_DESC_LEN)

typedef struct __attribute__((packed)) {
    uint8_t hat;
    uint8_t btn2;
    uint8_t btn3;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t lt;
    uint8_t rt;
} stadia_hid_report_t;

static const uint8_t s_hid_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x81, 0x03, 0x05, 0x09, 0x09, 0x12, 0x09, 0x11, 0x09, 0x14, 0x09, 0x13, 0x09, 0x0D, 0x09, 0x0C, 0x09, 0x0B, 0x09, 0x0F, 0x09, 0x0E, 0x09, 0x08, 0x09, 0x07, 0x09, 0x05, 0x09, 0x04, 0x09, 0x02, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x01, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x05, 0x02, 0x09, 0xC5, 0x09, 0xC4, 0x15, 0x00, 0x95, 0x02, 0x81, 0x02, 0x85, 0x05, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x02, 0x91, 0x02, 0xC0
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x18D1,
    .idProduct          = 0x9400,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

static const char *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Google Inc.",
    "Stadia Controller rev. A",
    "STADIA-WIFI-S3",
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* ITF 0: Stadia HID Gamepad + Rumble (EP 0x01 OUT, EP 0x81 IN, 64B, 1 ms) */
    TUD_HID_INOUT_DESCRIPTOR(USB_ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), 0x01, 0x81, 64, 1),
    /* ITF 1..3: IAD + UAC1 Full-Duplex Audio (SPK EP 0x02 OUT 192B ADAPTIVE, MIC EP 0x83 IN 96B ASYNC) */
    0x08, TUSB_DESC_INTERFACE_ASSOCIATION, USB_ITF_AUDIO_CONTROL, 0x03, TUSB_CLASS_AUDIO, 0x00, 0x00, 0x00,
    TUD_AUDIO10_DESC_STD_AC(USB_ITF_AUDIO_CONTROL, 0x00, 0x00),
    TUD_AUDIO10_DESC_CS_AC(
        0x0100,
        (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN),
        USB_ITF_AUDIO_STREAMING_SPK, USB_ITF_AUDIO_STREAMING_MIC),
    TUD_AUDIO10_DESC_INPUT_TERM(USB_UAC1_SPK_IT_ID, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x02, 0x0003, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(USB_UAC1_SPK_OT_ID, AUDIO_TERM_TYPE_OUT_HEADPHONES, 0x00, USB_UAC1_SPK_IT_ID, 0x00),
    TUD_AUDIO10_DESC_INPUT_TERM(USB_UAC1_MIC_IT_ID, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00, 0x01, 0x0001, 0x00, 0x00),
    TUD_AUDIO10_DESC_OUTPUT_TERM(USB_UAC1_MIC_OT_ID, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, USB_UAC1_MIC_IT_ID, 0x00),
    /* ITF 2: Speaker OUT (48kHz, 16-bit stereo, Adaptive - required so TinyUSB 0.21 classifies as data EP) */
    TUD_AUDIO10_DESC_STD_AS_INT(USB_ITF_AUDIO_STREAMING_SPK, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(USB_ITF_AUDIO_STREAMING_SPK, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(USB_UAC1_SPK_IT_ID, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(2, 2, 16, 48000),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(USB_UAC1_SPK_EP_OUT, (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE), USB_UAC1_SPK_MPS, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(0x00, AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
    /* ITF 3: Mic IN (48kHz, 16-bit mono, Asynchronous - required so TinyUSB 0.21 classifies as data EP) */
    TUD_AUDIO10_DESC_STD_AS_INT(USB_ITF_AUDIO_STREAMING_MIC, 0x00, 0x00, 0x00),
    TUD_AUDIO10_DESC_STD_AS_INT(USB_ITF_AUDIO_STREAMING_MIC, 0x01, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_INT(USB_UAC1_MIC_OT_ID, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(1, 2, 16, 48000),
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(USB_UAC1_MIC_EP_IN, (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS), USB_UAC1_MIC_MPS, 0x01, 0x00),
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(0x00, AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),
};

_Static_assert(sizeof(s_config_desc) == TUSB_DESC_TOTAL_LEN, "s_config_desc size mismatch");

static portMUX_TYPE s_hid_mux = portMUX_INITIALIZER_UNLOCKED;
static stadia_hid_report_t s_latest = { .hat = 8, .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
static bool s_dirty = false;
static bool s_kick_pending = false;
static int64_t s_last_tx_us = 0;
static usb_rumble_cb_t s_rumble_cb = NULL;
static bool s_usb_installed = false;

static void hid_flush_on_tusb_task(void *arg) {
    (void)arg;
    stadia_hid_report_t rep;
    bool should_send = false;
    portENTER_CRITICAL(&s_hid_mux);
    s_kick_pending = false;
    if (s_dirty) {
        rep = s_latest;
        s_dirty = false;
        should_send = true;
    }
    portEXIT_CRITICAL(&s_hid_mux);

    if (should_send && tud_mounted() && tud_hid_ready()) {
        s_last_tx_us = esp_timer_get_time();
        tud_hid_report(INPUT_REPORT_ID, &rep, sizeof(rep));
    } else if (should_send) {
        portENTER_CRITICAL(&s_hid_mux);
        s_dirty = true;
        portEXIT_CRITICAL(&s_hid_mux);
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_type;
    if (report_id == INPUT_REPORT_ID && reqlen >= sizeof(s_latest)) {
        portENTER_CRITICAL(&s_hid_mux);
        memcpy(buffer, &s_latest, sizeof(s_latest));
        portEXIT_CRITICAL(&s_hid_mux);
        return sizeof(s_latest);
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_type;
    if (!s_rumble_cb || !buffer) return;
    uint16_t left = 0, right = 0;
    if (report_id == RUMBLE_REPORT_ID && bufsize >= 4) {
        left  = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        right = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
        s_rumble_cb(left, right);
    } else if (report_id == 0 && bufsize >= 5 && buffer[0] == RUMBLE_REPORT_ID) {
        left  = (uint16_t)buffer[1] | ((uint16_t)buffer[2] << 8);
        right = (uint16_t)buffer[3] | ((uint16_t)buffer[4] << 8);
        s_rumble_cb(left, right);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance; (void)report; (void)len;
    hid_flush_on_tusb_task(NULL);
}

static uint8_t axis_to_u8(int32_t v) {
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    int32_t s = ((v + 32768) * 254 + 32767) / 65535 + 1;
    if (s < 1) s = 1;
    if (s > 255) s = 255;
    return (uint8_t)s;
}

static uint8_t dpad_to_hat(uint32_t b) {
    bool u = (b & (1u << 0)) != 0;
    bool d = (b & (1u << 1)) != 0;
    bool l = (b & (1u << 2)) != 0;
    bool r = (b & (1u << 3)) != 0;
    if (u && r) return 1;
    if (d && r) return 3;
    if (d && l) return 5;
    if (u && l) return 7;
    if (u)      return 0;
    if (r)      return 2;
    if (d)      return 4;
    if (l)      return 6;
    return 8;
}

static void push_report(const stadia_hid_report_t *rep) {
    if (!s_usb_installed) return;
    bool kick = false;
    portENTER_CRITICAL(&s_hid_mux);
    s_latest = *rep;
    s_dirty = true;
    if (!s_kick_pending) {
        s_kick_pending = true;
        kick = true;
    }
    portEXIT_CRITICAL(&s_hid_mux);
    if (kick) {
        usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
    }
}

void usb_gamepad_send(uint32_t b,
                      int32_t lx, int32_t ly,
                      int32_t rx, int32_t ry,
                      uint32_t lt, uint32_t rt) {
    uint8_t btn2 = 0, btn3 = 0;
    if (b & (1u << 11)) btn2 |= 0x01; /* Capture */
    if (b & (1u << 10)) btn2 |= 0x02; /* Assistant */
    if (lt > TRIGGER_THRESHOLD) btn2 |= 0x04; /* L2 digital */
    if (rt > TRIGGER_THRESHOLD) btn2 |= 0x08; /* R2 digital */
    if (b & (1u << 16)) btn2 |= 0x10; /* Stadia */
    if (b & (1u <<  4)) btn2 |= 0x20; /* Menu */
    if (b & (1u <<  5)) btn2 |= 0x40; /* Options */
    if (b & (1u <<  7)) btn2 |= 0x80; /* R3 */

    if (b & (1u <<  6)) btn3 |= 0x01; /* L3 */
    if (b & (1u <<  9)) btn3 |= 0x02; /* R1 */
    if (b & (1u <<  8)) btn3 |= 0x04; /* L1 */
    if (b & (1u << 15)) btn3 |= 0x08; /* Y */
    if (b & (1u << 14)) btn3 |= 0x10; /* X */
    if (b & (1u << 13)) btn3 |= 0x20; /* B */
    if (b & (1u << 12)) btn3 |= 0x40; /* A */

    int32_t inv_ly = (ly == -32768) ? 32767 : -ly;
    int32_t inv_ry = (ry == -32768) ? 32767 : -ry;

    stadia_hid_report_t rep = {
        .hat  = dpad_to_hat(b),
        .btn2 = btn2,
        .btn3 = btn3,
        .lx   = axis_to_u8(lx),
        .ly   = axis_to_u8(inv_ly),
        .rx   = axis_to_u8(rx),
        .ry   = axis_to_u8(inv_ry),
        .lt   = (uint8_t)(lt > 255 ? 255 : lt),
        .rt   = (uint8_t)(rt > 255 ? 255 : rt),
    };
    push_report(&rep);
}

void usb_gamepad_reset(void) {
    stadia_hid_report_t neutral = { .hat = 8, .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
    push_report(&neutral);
}

/* Re-send current state every 100 ms safely via Core 1 TinyUSB task */
void usb_gamepad_keepalive_tick(void) {
    if (!s_usb_installed) return;
    int64_t now = esp_timer_get_time();
    if ((now - s_last_tx_us) >= 100000) {
        bool kick = false;
        portENTER_CRITICAL(&s_hid_mux);
        s_dirty = true;
        if (!s_kick_pending) {
            s_kick_pending = true;
            kick = true;
        }
        portEXIT_CRITICAL(&s_hid_mux);
        if (kick) {
            usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
        }
    }
}

void usb_gamepad_init(usb_rumble_cb_t rumble_cb) {
    s_rumble_cb = rumble_cb;
    usb_audio_init();
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = &s_device_desc,
        .string_descriptor        = s_string_desc,
        .string_descriptor_count  = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
        .external_phy             = false,
        .configuration_descriptor = s_config_desc,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %d", (int)err);
        return;
    }
    s_usb_installed = true;
    ESP_LOGI(TAG, "USB HID Stadia Controller (18d1:9400) ready at 1000 Hz");
}
