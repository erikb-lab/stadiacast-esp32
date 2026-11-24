/* Extra USB HID Controller Modes:
 *   Mode 2: Sony DualShock 4 v2 (054c:09cc) + Bidirectional 48 kHz UAC1 Audio
 *   Mode 3: Nintendo Switch Pro Controller (057e:2009), HID-only
 *   Mode 4: Sony PlayStation 3 / DualShock 3 (054c:0268), HID-only
 */
#include "usb_modes_extra.h"
#include "usb_audio.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TAG "USB-EXTRA"
#define TRIGGER_THRESHOLD 30

/* ============================================================================
 * Shared Helpers
 * ============================================================================ */
static uint8_t         s_mac[6] = { 0x02, 0x53, 0x54, 0x41, 0x44, 0x01 };
static usb_rumble_cb_t s_rumble_cb = NULL;

static inline uint8_t axis_to_u8_0_255(int32_t v) {
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    return (uint8_t)(((v + 32768) * 255 + 32767) / 65535);
}

static inline uint8_t dpad_to_hat(uint32_t b) {
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

/* ============================================================================
 * Mode 2: Sony DualShock 4 v2 (054c:09cc, CUH-ZCT2x) + UAC1 Audio
 * ============================================================================ */
#define DS4_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + UAC1_AUDIO_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t s_ds4_report_desc[507] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25,
    0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00, 0x25, 0x7F, 0x81, 0x02, 0x05,
    0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x36, 0x81, 0x02, 0x85, 0x05, 0x09, 0x22, 0x95, 0x1F, 0x91,
    0x02, 0x85, 0x04, 0x09, 0x23, 0x95, 0x24, 0xB1, 0x02, 0x85, 0x02, 0x09, 0x24, 0x95, 0x24, 0xB1,
    0x02, 0x85, 0x08, 0x09, 0x25, 0x95, 0x03, 0xB1, 0x02, 0x85, 0x10, 0x09, 0x26, 0x95, 0x04, 0xB1,
    0x02, 0x85, 0x11, 0x09, 0x27, 0x95, 0x02, 0xB1, 0x02, 0x85, 0x12, 0x06, 0x02, 0xFF, 0x09, 0x21,
    0x95, 0x0F, 0xB1, 0x02, 0x85, 0x13, 0x09, 0x22, 0x95, 0x16, 0xB1, 0x02, 0x85, 0x14, 0x06, 0x05,
    0xFF, 0x09, 0x20, 0x95, 0x10, 0xB1, 0x02, 0x85, 0x15, 0x09, 0x21, 0x95, 0x2C, 0xB1, 0x02, 0x06,
    0x80, 0xFF, 0x85, 0x80, 0x09, 0x20, 0x95, 0x06, 0xB1, 0x02, 0x85, 0x81, 0x09, 0x21, 0x95, 0x06,
    0xB1, 0x02, 0x85, 0x82, 0x09, 0x22, 0x95, 0x05, 0xB1, 0x02, 0x85, 0x83, 0x09, 0x23, 0x95, 0x01,
    0xB1, 0x02, 0x85, 0x84, 0x09, 0x24, 0x95, 0x04, 0xB1, 0x02, 0x85, 0x85, 0x09, 0x25, 0x95, 0x06,
    0xB1, 0x02, 0x85, 0x86, 0x09, 0x26, 0x95, 0x06, 0xB1, 0x02, 0x85, 0x87, 0x09, 0x27, 0x95, 0x23,
    0xB1, 0x02, 0x85, 0x88, 0x09, 0x28, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0x89, 0x09, 0x29, 0x95, 0x02,
    0xB1, 0x02, 0x85, 0x90, 0x09, 0x30, 0x95, 0x05, 0xB1, 0x02, 0x85, 0x91, 0x09, 0x31, 0x95, 0x03,
    0xB1, 0x02, 0x85, 0x92, 0x09, 0x32, 0x95, 0x03, 0xB1, 0x02, 0x85, 0x93, 0x09, 0x33, 0x95, 0x0C,
    0xB1, 0x02, 0x85, 0x94, 0x09, 0x34, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xA0, 0x09, 0x40, 0x95, 0x06,
    0xB1, 0x02, 0x85, 0xA1, 0x09, 0x41, 0x95, 0x01, 0xB1, 0x02, 0x85, 0xA2, 0x09, 0x42, 0x95, 0x01,
    0xB1, 0x02, 0x85, 0xA3, 0x09, 0x43, 0x95, 0x30, 0xB1, 0x02, 0x85, 0xA4, 0x09, 0x44, 0x95, 0x0D,
    0xB1, 0x02, 0x85, 0xF0, 0x09, 0x47, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF1, 0x09, 0x48, 0x95, 0x3F,
    0xB1, 0x02, 0x85, 0xF2, 0x09, 0x49, 0x95, 0x0F, 0xB1, 0x02, 0x85, 0xA7, 0x09, 0x4A, 0x95, 0x01,
    0xB1, 0x02, 0x85, 0xA8, 0x09, 0x4B, 0x95, 0x01, 0xB1, 0x02, 0x85, 0xA9, 0x09, 0x4C, 0x95, 0x08,
    0xB1, 0x02, 0x85, 0xAA, 0x09, 0x4E, 0x95, 0x01, 0xB1, 0x02, 0x85, 0xAB, 0x09, 0x4F, 0x95, 0x39,
    0xB1, 0x02, 0x85, 0xAC, 0x09, 0x50, 0x95, 0x39, 0xB1, 0x02, 0x85, 0xAD, 0x09, 0x51, 0x95, 0x0B,
    0xB1, 0x02, 0x85, 0xAE, 0x09, 0x52, 0x95, 0x01, 0xB1, 0x02, 0x85, 0xAF, 0x09, 0x53, 0x95, 0x02,
    0xB1, 0x02, 0x85, 0xB0, 0x09, 0x54, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xE0, 0x09, 0x57, 0x95, 0x02,
    0xB1, 0x02, 0x85, 0xB3, 0x09, 0x55, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xB4, 0x09, 0x55, 0x95, 0x3F,
    0xB1, 0x02, 0x85, 0xB5, 0x09, 0x56, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xD0, 0x09, 0x58, 0x95, 0x3F,
    0xB1, 0x02, 0x85, 0xD4, 0x09, 0x59, 0x95, 0x3F, 0xB1, 0x02, 0xC0,
};

static const tusb_desc_device_t s_ds4_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x054C,
    .idProduct          = 0x09CC,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01,
};

static const char *s_ds4_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Sony Interactive Entertainment",
    "Wireless Controller",
};

static const uint8_t s_ds4_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, DS4_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* Matches CUH-ZCT2x interface order: ITF 0..2 Audio, ITF 3 HID (EP 0x03 OUT, EP 0x84 IN) */
    UAC1_AUDIO_DESC_BODY_AT(0, 1, 2),
    TUD_HID_INOUT_DESCRIPTOR(3, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_ds4_report_desc), 0x03, 0x84, 64, 1),
};

_Static_assert(sizeof(s_ds4_config_desc) == DS4_DESC_TOTAL_LEN, "s_ds4_config_desc size mismatch");

static uint8_t  s_ds4_counter = 0;
static uint16_t s_ds4_sensor_ts = 0;

/* Builds the 63-byte payload following Report ID 0x01 (total 64 bytes on wire). */
static void ds4_build_input(uint8_t payload[63], const usb_canonical_input_t *in) {
    memset(payload, 0, 63);
    int32_t inv_ly = (in->ly == -32768) ? 32767 : -in->ly;
    int32_t inv_ry = (in->ry == -32768) ? 32767 : -in->ry;

    payload[0] = axis_to_u8_0_255(in->lx);
    payload[1] = axis_to_u8_0_255(inv_ly);
    payload[2] = axis_to_u8_0_255(in->rx);
    payload[3] = axis_to_u8_0_255(inv_ry);

    uint32_t b = in->buttons;
    uint8_t b0 = dpad_to_hat(b);
    if (b & (1u << 14)) b0 |= 0x10; /* Square  <- X */
    if (b & (1u << 12)) b0 |= 0x20; /* Cross   <- A */
    if (b & (1u << 13)) b0 |= 0x40; /* Circle  <- B */
    if (b & (1u << 15)) b0 |= 0x80; /* Triangle<- Y */
    payload[4] = b0;

    uint8_t b1 = 0;
    if (b & (1u <<  8)) b1 |= 0x01; /* L1 */
    if (b & (1u <<  9)) b1 |= 0x02; /* R1 */
    if (in->lt > TRIGGER_THRESHOLD) b1 |= 0x04; /* L2 digital */
    if (in->rt > TRIGGER_THRESHOLD) b1 |= 0x08; /* R2 digital */
    if (b & (1u <<  5)) b1 |= 0x10; /* Share   <- Options (...) */
    if (b & (1u <<  4)) b1 |= 0x20; /* Options <- Menu */
    if (b & (1u <<  6)) b1 |= 0x40; /* L3 */
    if (b & (1u <<  7)) b1 |= 0x80; /* R3 */
    payload[5] = b1;

    uint8_t b2 = (uint8_t)((s_ds4_counter++ & 0x3Fu) << 2);
    if (b & (1u << 16)) b2 |= 0x01; /* PS             <- Stadia */
    if (b & (1u << 11)) b2 |= 0x02; /* Touchpad Click <- Capture */
    payload[6] = b2;

    payload[7] = in->lt;
    payload[8] = in->rt;

    s_ds4_sensor_ts += 188u;
    payload[9]  = (uint8_t)(s_ds4_sensor_ts & 0xFFu);
    payload[10] = (uint8_t)(s_ds4_sensor_ts >> 8);
    payload[11] = 0x19; /* ~25C */

    /* Stationary IMU: gyro = {0,0,0}, accel = {0, +8192 (+1g), 0} matching flat CUH-ZCT2x */
    payload[20] = 0x00;
    payload[21] = 0x20; /* +8192 LE on Y */

    /* Wire byte 30 (payload[29]): battery nibble (0..10, discharging) + headset/mic detect */
    uint8_t batt_nib = (in->battery_pct >= 100u) ? 10u : (uint8_t)(in->battery_pct / 10u);
    uint8_t status0 = batt_nib & 0x0Fu;
    if (in->headset_plugged) status0 |= 0x60u; /* Headphones (0x20) + Mic (0x40) */
    payload[29] = status0;

    /* Touchpad: 1 report, all touch points marked inactive (BIT(7) = 0x80) */
    payload[32] = 0x01;
    payload[34] = 0x80;
    payload[38] = 0x80;
    payload[43] = 0x80;
    payload[47] = 0x80;
    payload[52] = 0x80;
    payload[56] = 0x80;
}

static uint16_t ds4_get_report(uint8_t report_id, uint8_t *buf, uint16_t reqlen,
                               const usb_canonical_input_t *in) {
    if (report_id == 0x01 && reqlen >= 63) {
        ds4_build_input(buf, in);
        return 63;
    }
    if (report_id == 0x02 && reqlen >= 36) {
        /* Unity IMU calibration: zero bias, valid non-zero +-ranges so hid-playstation never div/0 */
        static const uint8_t k_imu_cal[36] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* gyro pitch/yaw/roll bias = 0 */
            0x28, 0x23, 0xD8, 0xDC,             /* pitch +9000 / -9000 */
            0x28, 0x23, 0xD8, 0xDC,             /* yaw   +9000 / -9000 */
            0x28, 0x23, 0xD8, 0xDC,             /* roll  +9000 / -9000 */
            0x1C, 0x02, 0x1C, 0x02,             /* gyro speed_plus=540, speed_minus=540 */
            0x00, 0x20, 0x00, 0xE0,             /* acc X +8192 / -8192 */
            0x00, 0x20, 0x00, 0xE0,             /* acc Y +8192 / -8192 */
            0x00, 0x20, 0x00, 0xE0,             /* acc Z +8192 / -8192 */
            0x01, 0x00,
        };
        memcpy(buf, k_imu_cal, sizeof(k_imu_cal));
        return sizeof(k_imu_cal);
    }
    if (report_id == 0x12 && reqlen >= 15) {
        /* Controller MAC (6B reversed) + 08 25 00 + Host MAC (6B zero) */
        memset(buf, 0, 15);
        for (int i = 0; i < 6; i++) buf[i] = s_mac[5 - i];
        buf[6] = 0x08;
        buf[7] = 0x25;
        buf[8] = 0x00;
        return 15;
    }
    if (report_id == 0x81 && reqlen >= 6) {
        for (int i = 0; i < 6; i++) buf[i] = s_mac[5 - i];
        return 6;
    }
    if (report_id == 0xA3 && reqlen >= 48) {
        /* CUH-ZCT2x firmware/hardware version block */
        memset(buf, 0, 48);
        memcpy(buf + 0,  "Sep 21 2018", 11);
        memcpy(buf + 16, "04:50:51", 8);
        static const uint8_t k_ver_tail[16] = {
            0x00, 0x01, 0x00, 0xB4, 0x01, 0x00, 0x00, 0x00,
            0x0A, 0xA0, 0x10, 0x20, 0x00, 0xA0, 0x02, 0x00,
        };
        memcpy(buf + 32, k_ver_tail, sizeof(k_ver_tail));
        return 48;
    }
    if (report_id == 0xF2 && reqlen >= 15) {
        memset(buf, 0, 15);
        buf[1] = 0x01; /* idle/not-ready status */
        return 15;
    }
    return 0;
}

static void ds4_set_report(uint8_t report_id, const uint8_t *buf, uint16_t len) {
    if (!s_rumble_cb || !buf) return;
    const uint8_t *p = NULL;
    uint16_t plen = 0;
    if (report_id == 0x05) {
        p = buf;
        plen = len;
    } else if (report_id == 0 && len >= 1 && buf[0] == 0x05) {
        p = buf + 1;
        plen = (uint16_t)(len - 1);
    }
    /* p[0]=valid_flag0, p[3]=motor_right (weak), p[4]=motor_left (strong) */
    if (p && plen >= 5 && (p[0] & 0x01u)) {
        uint16_t weak   = (uint16_t)p[3] * 257u;
        uint16_t strong = (uint16_t)p[4] * 257u;
        s_rumble_cb(strong, weak);
    }
}

/* ============================================================================
 * Mode 3: Nintendo Switch Pro Controller (057e:2009), HID-only
 * ============================================================================ */
#define SW_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t s_switch_report_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x05,        /* Usage (Game Pad) */
    0xA1, 0x01,        /* Collection (Application) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,  /*   Logical Maximum (255) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x3F,        /*   Report Count (63) -> 64B total with Report ID */
    0x06, 0x00, 0xFF,  /*   Usage Page (Vendor Defined 0xFF00) */
    0x85, 0x30, 0x09, 0x01, 0x81, 0x02, /* Input  0x30 (Standard Full) */
    0x85, 0x21, 0x09, 0x01, 0x81, 0x02, /* Input  0x21 (Subcommand Reply) */
    0x85, 0x81, 0x09, 0x01, 0x81, 0x02, /* Input  0x81 (USB Handshake Reply) */
    0x85, 0x31, 0x09, 0x01, 0x81, 0x02, /* Input  0x31 (NFC/IR) */
    0x85, 0x01, 0x09, 0x01, 0x91, 0x02, /* Output 0x01 (Rumble + Subcommand) */
    0x85, 0x10, 0x09, 0x01, 0x91, 0x02, /* Output 0x10 (Rumble Only) */
    0x85, 0x80, 0x09, 0x01, 0x91, 0x02, /* Output 0x80 (USB Handshake) */
    0xC0,              /* End Collection */
};

static const tusb_desc_device_t s_switch_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x057E,
    .idProduct          = 0x2009,
    .bcdDevice          = 0x0210,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_switch_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Nintendo Co., Ltd.",
    "Pro Controller",
    "000000000001",
};

static const uint8_t s_switch_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, SW_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* HID-only (no UAC1) so a real Nintendo Switch recognizes 057e:2009 cleanly */
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_switch_report_desc), 0x01, 0x81, 64, 8),
};

_Static_assert(sizeof(s_switch_config_desc) == SW_DESC_TOTAL_LEN, "s_switch_config_desc size mismatch");

#define SW_REPLY_Q_CAP 8u
static uint8_t s_sw_q[SW_REPLY_Q_CAP][64];
static uint8_t s_sw_q_head = 0;
static uint8_t s_sw_q_tail = 0;
static bool    s_sw_streaming = false;
static uint8_t s_sw_timer = 0;

static void sw_enqueue_reply(const uint8_t pkt[64]) {
    uint8_t next = (uint8_t)((s_sw_q_head + 1u) % SW_REPLY_Q_CAP);
    if (next == s_sw_q_tail) {
        s_sw_q_tail = (uint8_t)((s_sw_q_tail + 1u) % SW_REPLY_Q_CAP);
    }
    memcpy(s_sw_q[s_sw_q_head], pkt, 64);
    s_sw_q_head = next;
}

static uint16_t sw_axis_12bit(int32_t v) {
    /* Map -32768..32767 -> 0x100..0xF00 (center 0x800, +-0x700 matching SPI 0x603D calib) */
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    int32_t scaled = 0x800 + (v * 0x700) / 32768;
    if (scaled < 0) scaled = 0;
    if (scaled > 4095) scaled = 4095;
    return (uint16_t)scaled;
}

static void sw_fill_input_header(uint8_t pkt[64], const usb_canonical_input_t *in) {
    /* Genuine Switch Pro timer ticks at 200 Hz (5 ms/tick); fallback to +3 if called faster */
    uint8_t hw_tick = (uint8_t)((esp_timer_get_time() / 5000LL) & 0xFF);
    if (hw_tick == s_sw_timer) hw_tick = (uint8_t)(s_sw_timer + 1u);
    s_sw_timer = hw_tick;
    pkt[1] = s_sw_timer;

    /* High nibble: battery level (8=full, 6=med, 4=low, 2=critical), low nibble: 0x1 (USB/Pro) */
    uint8_t batt_n = (in->battery_pct >= 75u) ? 0x80u :
                     (in->battery_pct >= 45u) ? 0x60u :
                     (in->battery_pct >= 20u) ? 0x40u : 0x20u;
    pkt[2] = (uint8_t)(batt_n | 0x01u);

    uint32_t b = in->buttons;
    /* Nintendo physical-position mapping: A(bottom)->B, B(right)->A, X(left)->Y, Y(top)->X */
    uint8_t right_b = 0;
    if (b & (1u << 14)) right_b |= 0x01; /* Y <- Stadia X */
    if (b & (1u << 15)) right_b |= 0x02; /* X <- Stadia Y */
    if (b & (1u << 12)) right_b |= 0x04; /* B <- Stadia A */
    if (b & (1u << 13)) right_b |= 0x08; /* A <- Stadia B */
    if (b & (1u <<  9)) right_b |= 0x40; /* R <- R1 */
    if (in->rt > TRIGGER_THRESHOLD) right_b |= 0x80; /* ZR <- R2 */
    pkt[3] = right_b;

    uint8_t shared_b = 0;
    if (b & (1u <<  5)) shared_b |= 0x01; /* Minus   <- Options (...) */
    if (b & (1u <<  4)) shared_b |= 0x02; /* Plus    <- Menu */
    if (b & (1u <<  7)) shared_b |= 0x04; /* R3 */
    if (b & (1u <<  6)) shared_b |= 0x08; /* L3 */
    if (b & (1u << 16)) shared_b |= 0x10; /* Home    <- Stadia */
    if (b & (1u << 11)) shared_b |= 0x20; /* Capture <- Capture */
    pkt[4] = shared_b;

    uint8_t left_b = 0;
    if (b & (1u <<  1)) left_b |= 0x01; /* Down */
    if (b & (1u <<  0)) left_b |= 0x02; /* Up */
    if (b & (1u <<  3)) left_b |= 0x04; /* Right */
    if (b & (1u <<  2)) left_b |= 0x08; /* Left */
    if (b & (1u <<  8)) left_b |= 0x40; /* L <- L1 */
    if (in->lt > TRIGGER_THRESHOLD) left_b |= 0x80; /* ZL <- L2 */
    pkt[5] = left_b;

    /* Switch Pro sticks: Up = +0xFFF, Right = +0xFFF (same polarity as Stadia canonical lx/ly) */
    uint16_t lx = sw_axis_12bit(in->lx);
    uint16_t ly = sw_axis_12bit(in->ly);
    uint16_t rx = sw_axis_12bit(in->rx);
    uint16_t ry = sw_axis_12bit(in->ry);

    pkt[6]  = (uint8_t)(lx & 0xFFu);
    pkt[7]  = (uint8_t)(((lx >> 8) & 0x0Fu) | ((ly & 0x0Fu) << 4));
    pkt[8]  = (uint8_t)((ly >> 4) & 0xFFu);
    pkt[9]  = (uint8_t)(rx & 0xFFu);
    pkt[10] = (uint8_t)(((rx >> 8) & 0x0Fu) | ((ry & 0x0Fu) << 4));
    pkt[11] = (uint8_t)((ry >> 4) & 0xFFu);
    pkt[12] = 0x80; /* Vibrator input report ready */
}

static void sw_build_input_30(uint8_t pkt[64], const usb_canonical_input_t *in) {
    memset(pkt, 0, 64);
    pkt[0] = 0x30;
    sw_fill_input_header(pkt, in);
    /* 3 stationary IMU samples (accel Z = +4096 = +1g, gyro = 0) at bytes 13..48 */
    for (int s = 0; s < 3; s++) {
        int off = 13 + s * 12;
        pkt[off + 4] = 0x00;
        pkt[off + 5] = 0x10; /* +4096 LE */
    }
}

static uint16_t sw_decode_hd_rumble_side(const uint8_t b[4]) {
    /* Idle neutral packet is {0x00, 0x01, 0x40, 0x40} */
    uint32_t hf_raw = (uint32_t)(b[1] & 0xFEu);
    uint32_t hf_255 = (hf_raw * 255u) / 0xC8u;
    if (hf_255 > 255u) hf_255 = 255u;

    int lf_delta = (int)b[3] - 0x40;
    if (lf_delta < 0) lf_delta = 0;
    uint32_t lf_255 = ((uint32_t)lf_delta * 255u) / 0x32u;
    if (lf_255 > 255u) lf_255 = 255u;

    uint32_t amp = (hf_255 > lf_255) ? hf_255 : lf_255;
    return (uint16_t)(amp * 257u);
}

static void sw_read_spi_flash(uint32_t addr, uint8_t len, uint8_t *dst) {
    /* Factory SPI image covering 0x6000..0x60AF; all other addresses (incl 0x8010 user cal) = 0xFF */
    static uint8_t s_spi_6000[0xB0];
    static bool    s_spi_inited = false;
    if (!s_spi_inited) {
        memset(s_spi_6000, 0xFF, sizeof(s_spi_6000));
        /* 0x6020..0x6037: IMU factory cal (accel origin=0, 1g coeff=0x4000; gyro origin=0, coeff=13371) */
        static const uint8_t k_imu[24] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,
        };
        memcpy(s_spi_6000 + 0x20, k_imu, sizeof(k_imu));
        /* 0x603D..0x604E: Stick factory cal (center=0x800, +-0x700)
         * Left  (0x603D): max_above, center, min_below
         * Right (0x6046): center, min_below, max_above */
        static const uint8_t k_sticks[18] = {
            0x00, 0x07, 0x70, 0x00, 0x08, 0x80, 0x00, 0x07, 0x70,
            0x00, 0x08, 0x80, 0x00, 0x07, 0x70, 0x00, 0x07, 0x70,
        };
        memcpy(s_spi_6000 + 0x3D, k_sticks, sizeof(k_sticks));
        /* 0x6050..0x605B: Pro Controller colors (body, buttons, L-grip, R-grip) */
        static const uint8_t k_colors[12] = {
            0x2D, 0x2D, 0x2D, 0xE6, 0xE6, 0xE6, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D, 0x2D,
        };
        memcpy(s_spi_6000 + 0x50, k_colors, sizeof(k_colors));
        /* 0x6080..0x6085: IMU horizontal offsets (values read from a real Pro Controller over USB) */
        static const uint8_t k_imu_hoff[6] = { 0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F };
        memcpy(s_spi_6000 + 0x80, k_imu_hoff, sizeof(k_imu_hoff));
        /* 0x6086..0x60A9: Factory L & R stick parameters (values read from a real Pro Controller over USB) */
        static const uint8_t k_sparams[18] = {
            0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3, 0xD4, 0x14, 0x54,
            0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63,
        };
        memcpy(s_spi_6000 + 0x86, k_sparams, 18);
        memcpy(s_spi_6000 + 0x98, k_sparams, 18);
        s_spi_inited = true;
    }

    for (uint8_t i = 0; i < len; i++) {
        uint32_t a = addr + i;
        if (a >= 0x6000u && a < 0x6000u + sizeof(s_spi_6000)) {
            dst[i] = s_spi_6000[a - 0x6000u];
        } else {
            dst[i] = 0xFF;
        }
    }
}

/* CRC-8 (poly 0x07, init 0) over bytes 15..47 of an NFC/IR MCU reply, stored at byte 48.
 * Matches the 0x21 reply of a real Pro Controller over USB (CRC 0x5C). */
static uint8_t sw_mcu_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void sw_set_report(uint8_t report_id, const uint8_t *buf, uint16_t len,
                          const usb_canonical_input_t *in, bool *out_kick_tx) {
    if (!buf || len == 0) return;
    uint8_t rid = report_id;
    const uint8_t *p = buf;
    uint16_t plen = len;
    if (rid == 0) {
        rid = buf[0];
        p = buf + 1;
        plen = (uint16_t)(len - 1);
    }

    if (rid == 0x80 && plen >= 1) {
        uint8_t sub = p[0];
        uint8_t rep[64];
        memset(rep, 0, sizeof(rep));
        if (sub == 0x01) {
            rep[0] = 0x81; rep[1] = 0x01; rep[2] = 0x00; rep[3] = 0x03; /* Pro Controller */
            for (int i = 0; i < 6; i++) rep[4 + i] = s_mac[5 - i];
            sw_enqueue_reply(rep);
            *out_kick_tx = true;
        } else if (sub == 0x02) {
            rep[0] = 0x81; rep[1] = 0x02;
            sw_enqueue_reply(rep);
            *out_kick_tx = true;
        } else if (sub == 0x03) {
            rep[0] = 0x81; rep[1] = 0x03;
            sw_enqueue_reply(rep);
            *out_kick_tx = true;
        } else if (sub == 0x04) {
            s_sw_streaming = true;
            *out_kick_tx = true;
        } else if (sub == 0x05) {
            s_sw_streaming = false;
        }
        return;
    }

    if ((rid == 0x10 || rid == 0x01) && plen >= 9 && s_rumble_cb) {
        /* p[0]=counter, p[1..4]=left HD rumble, p[5..8]=right HD rumble */
        uint16_t rl = sw_decode_hd_rumble_side(p + 1);
        uint16_t rr = sw_decode_hd_rumble_side(p + 5);
        s_rumble_cb(rl, rr);
    }

    if (rid == 0x01 && plen >= 10) {
        uint8_t subcmd = p[9];
        const uint8_t *args = (plen > 10) ? (p + 10) : NULL;
        uint16_t arglen = (plen > 10) ? (uint16_t)(plen - 10) : 0;

        uint8_t rep[64];
        memset(rep, 0, sizeof(rep));
        rep[0] = 0x21;
        sw_fill_input_header(rep, in);
        rep[14] = subcmd;

        switch (subcmd) {
        case 0x01: /* Bluetooth manual pairing. Over USB the Switch sends step 0x04 (host address + name)
                    * and a real Pro Controller answers ACK 0x81 with data 0x03. A plain 0x80 ACK
                    * here made the Switch's hid sysmodule abort (fatal 2162-0002) on FW 20.5.0. */
            rep[13] = 0x81;
            rep[15] = 0x03;
            break;
        case 0x02: /* Request Device Info */
            rep[13] = 0x82;
            rep[15] = 0x03; rep[16] = 0x8B; /* FW 3.139 */
            rep[17] = 0x03;                 /* Pro Controller */
            rep[18] = 0x02;
            memcpy(rep + 19, s_mac, 6);
            rep[25] = 0x03;                 /* Same trailing bytes as a real Pro Controller over USB */
            rep[26] = 0x01;                 /* Use SPI colors */
            break;
        case 0x03: /* Set Input Report Mode */
            rep[13] = 0x80;
            if (args && arglen >= 1 && args[0] == 0x30) {
                s_sw_streaming = true;
            }
            break;
        case 0x04: /* Trigger Buttons Elapsed Time */
            rep[13] = 0x83;
            break;
        case 0x10: /* SPI Flash Read */
            rep[13] = 0x90;
            if (args && arglen >= 5) {
                uint32_t addr = (uint32_t)args[0] | ((uint32_t)args[1] << 8) |
                                ((uint32_t)args[2] << 16) | ((uint32_t)args[3] << 24);
                uint8_t rlen = args[4];
                if (rlen > 29u) rlen = 29u;
                memcpy(rep + 15, args, 4);
                rep[19] = rlen;
                sw_read_spi_flash(addr, rlen, rep + 20);
            }
            break;
        case 0x21: { /* Set NFC/IR MCU configuration: MCU state report, as sent by a real Pro Controller */
            static const uint8_t k_mcu_state[8] = { 0x01, 0x00, 0xFF, 0x00, 0x03, 0x00, 0x05, 0x01 };
            rep[13] = 0xA0;
            memcpy(rep + 15, k_mcu_state, sizeof(k_mcu_state));
            rep[48] = sw_mcu_crc8(rep + 15, 33);
            break;
        }
        default:
            /* 0x08, 0x22, 0x30 (Player LEDs), 0x38 (Home LED), 0x40 (IMU), 0x48 (Vib) */
            rep[13] = 0x80;
            break;
        }
        sw_enqueue_reply(rep);
        *out_kick_tx = true;
    }
}

/* ============================================================================
 * Mode 4: Sony PlayStation 3 / DualShock 3 (054c:0268), HID-only
 * ============================================================================ */
#define PS3_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* Authentic 148-byte Sony DualShock 3 / SIXAXIS HID Report Descriptor */
static const uint8_t s_ps3_report_desc[148] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02,
    0x85, 0x01, 0x75, 0x08, 0x95, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x81, 0x03,
    0x75, 0x01, 0x95, 0x13, 0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x13, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x0D, 0x06, 0x00, 0xFF, 0x81, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x75, 0x08, 0x95, 0x04, 0x35, 0x00, 0x46, 0xFF, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02, 0xC0,
    0x05, 0x01, 0x75, 0x08, 0x95, 0x27, 0x09, 0x01, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x30, 0x09, 0x01, 0x91, 0x02,
    0x75, 0x08, 0x95, 0x30, 0x09, 0x01, 0xB1, 0x02, 0xC0,
    0xA1, 0x02, 0x85, 0x02, 0x75, 0x08, 0x95, 0x30, 0x09, 0x01, 0xB1, 0x02, 0xC0,
    0xA1, 0x02, 0x85, 0xEE, 0x75, 0x08, 0x95, 0x30, 0x09, 0x01, 0xB1, 0x02, 0xC0,
    0xA1, 0x02, 0x85, 0xEF, 0x75, 0x08, 0x95, 0x30, 0x09, 0x01, 0xB1, 0x02, 0xC0,
    0xC0,
};

static const tusb_desc_device_t s_ps3_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x054C,
    .idProduct          = 0x0268,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,
    .bNumConfigurations = 0x01,
};

static const char *s_ps3_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Sony",
    "PLAYSTATION(R)3 Controller",
};

static const uint8_t s_ps3_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, PS3_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_ps3_report_desc), 0x01, 0x81, 64, 1),
};

_Static_assert(sizeof(s_ps3_config_desc) == PS3_DESC_TOTAL_LEN, "s_ps3_config_desc size mismatch");

/* Builds the 48-byte payload following Report ID 0x01 (total 49 bytes on wire). */
static void ps3_build_input(uint8_t p[48], const usb_canonical_input_t *in) {
    memset(p, 0, 48);
    uint32_t b = in->buttons;

    uint8_t b1 = 0;
    if (b & (1u << 5)) b1 |= 0x01; /* Select <- Options (...) */
    if (b & (1u << 6)) b1 |= 0x02; /* L3 */
    if (b & (1u << 7)) b1 |= 0x04; /* R3 */
    if (b & (1u << 4)) b1 |= 0x08; /* Start  <- Menu */
    if (b & (1u << 0)) b1 |= 0x10; /* Up */
    if (b & (1u << 3)) b1 |= 0x20; /* Right */
    if (b & (1u << 1)) b1 |= 0x40; /* Down */
    if (b & (1u << 2)) b1 |= 0x80; /* Left */
    p[1] = b1; /* wire byte 2 */

    uint8_t b2 = 0;
    if (in->lt > TRIGGER_THRESHOLD) b2 |= 0x01; /* L2 */
    if (in->rt > TRIGGER_THRESHOLD) b2 |= 0x02; /* R2 */
    if (b & (1u <<  8)) b2 |= 0x04; /* L1 */
    if (b & (1u <<  9)) b2 |= 0x08; /* R1 */
    if (b & (1u << 15)) b2 |= 0x10; /* Triangle <- Y */
    if (b & (1u << 13)) b2 |= 0x20; /* Circle   <- B */
    if (b & (1u << 12)) b2 |= 0x40; /* Cross    <- A */
    if (b & (1u << 14)) b2 |= 0x80; /* Square   <- X */
    p[2] = b2; /* wire byte 3 */

    p[3] = (b & (1u << 16)) ? 0x01u : 0x00u; /* wire byte 4: PS/Home <- Stadia */

    int32_t inv_ly = (in->ly == -32768) ? 32767 : -in->ly;
    int32_t inv_ry = (in->ry == -32768) ? 32767 : -in->ry;
    p[5] = axis_to_u8_0_255(in->lx);     /* wire byte 6 */
    p[6] = axis_to_u8_0_255(inv_ly);     /* wire byte 7 */
    p[7] = axis_to_u8_0_255(in->rx);     /* wire byte 8 */
    p[8] = axis_to_u8_0_255(inv_ry);     /* wire byte 9 */

    /* Pressure bytes (wire bytes 14..25 -> p[13..24]) */
    p[13] = (b & (1u << 0)) ? 0xFF : 0x00; /* Up */
    p[14] = (b & (1u << 3)) ? 0xFF : 0x00; /* Right */
    p[15] = (b & (1u << 1)) ? 0xFF : 0x00; /* Down */
    p[16] = (b & (1u << 2)) ? 0xFF : 0x00; /* Left */
    p[17] = in->lt;                         /* L2 analog */
    p[18] = in->rt;                         /* R2 analog */
    p[19] = (b & (1u <<  8)) ? 0xFF : 0x00; /* L1 */
    p[20] = (b & (1u <<  9)) ? 0xFF : 0x00; /* R1 */
    p[21] = (b & (1u << 15)) ? 0xFF : 0x00; /* Triangle */
    p[22] = (b & (1u << 13)) ? 0xFF : 0x00; /* Circle */
    p[23] = (b & (1u << 12)) ? 0xFF : 0x00; /* Cross */
    p[24] = (b & (1u << 14)) ? 0xFF : 0x00; /* Square */

    /* Status & battery (wire bytes 29..31 -> p[28..30]) */
    p[28] = 0x03; /* USB connected, not charging */
    p[29] = (in->battery_pct >= 85u) ? 0x05u :
            (in->battery_pct >= 60u) ? 0x04u :
            (in->battery_pct >= 35u) ? 0x03u :
            (in->battery_pct >= 15u) ? 0x02u : 0x01u;
    p[30] = 0x14; /* Rumble on, USB */

    /* Stationary 10-bit big-endian IMU neutral = 512 (0x02, 0x00) at wire bytes 41..48 (p[40..47]) */
    p[40] = 0x02; p[41] = 0x00; /* Accel X */
    p[42] = 0x02; p[43] = 0x00; /* Accel Y */
    p[44] = 0x02; p[45] = 0x71; /* Accel Z (~+1g neutral) */
    p[46] = 0x02; p[47] = 0x00; /* Gyro Z */
}

static uint16_t ps3_get_report(uint8_t report_id, uint8_t *buf, uint16_t reqlen,
                               const usb_canonical_input_t *in) {
    if (report_id == 0x01 && reqlen >= 48) {
        ps3_build_input(buf, in);
        return 48;
    }
    if (report_id == 0xF2 && reqlen >= 16) {
        /* DS3 Bluetooth MAC & status (17B on wire; 16B after report_id 0xF2) */
        memset(buf, 0, 16);
        buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = 0x00;
        memcpy(buf + 3, s_mac, 6);
        buf[9]  = 0x00; buf[10] = 0x03; buf[11] = 0x50;
        buf[12] = 0x81; buf[13] = 0x01; buf[14] = 0x01; buf[15] = 0x83;
        return 16;
    }
    if (report_id == 0xF5 && reqlen >= 7) {
        memset(buf, 0, 7);
        return 7;
    }
    return 0;
}

static void ps3_set_report(uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buf, uint16_t len) {
    if (report_type == HID_REPORT_TYPE_FEATURE && report_id == 0xF4) {
        return; /* Operational activation ACK */
    }
    if (!s_rumble_cb || !buf) return;
    const uint8_t *p = NULL;
    uint16_t plen = 0;
    if (report_id == 0x01) {
        p = buf;
        plen = len;
    } else if (report_id == 0 && len >= 1 && buf[0] == 0x01) {
        p = buf + 1;
        plen = (uint16_t)(len - 1);
    }
    /* DS3 output report 0x01 payload: p[0]=pad, p[1]=right_dur, p[2]=right_pwr(0/1), p[3]=left_dur, p[4]=left_pwr(0..255) */
    if (p && plen >= 5) {
        uint16_t right = (p[1] > 0 && p[2] > 0) ? 0xFFFFu : 0u;
        uint16_t left  = (p[3] > 0) ? (uint16_t)p[4] * 257u : 0u;
        s_rumble_cb(left, right);
    }
}

/* ============================================================================
 * Public Dispatch API
 * ============================================================================ */
void usb_modes_extra_init(usb_gamepad_mode_t mode, usb_rumble_cb_t rumble_cb) {
    s_rumble_cb = rumble_cb;
    (void)esp_efuse_mac_get_default(s_mac);
    s_ds4_counter = 0;
    s_ds4_sensor_ts = 0;
    s_sw_q_head = 0;
    s_sw_q_tail = 0;
    s_sw_streaming = false;
    s_sw_timer = 0;

    if (mode == USB_GAMEPAD_MODE_DS4) {
        /* Match CUH-ZCT2x: Audio ITF 0..2 (SPK=1, MIC=2), HID ITF 3 */
        usb_audio_set_interfaces(1, 2);
    } else {
        usb_audio_set_interfaces(USB_ITF_AUDIO_STREAMING_SPK, USB_ITF_AUDIO_STREAMING_MIC);
    }
    ESP_LOGI(TAG, "Initialized extra USB mode=%d (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
             (int)mode, s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
}

bool usb_modes_extra_get_tusb_cfg(usb_gamepad_mode_t mode, tinyusb_config_t *out_cfg) {
    if (!out_cfg) return false;
    switch (mode) {
    case USB_GAMEPAD_MODE_DS4:
        *out_cfg = (tinyusb_config_t){
            .device_descriptor        = &s_ds4_device_desc,
            .string_descriptor        = s_ds4_string_desc,
            .string_descriptor_count  = sizeof(s_ds4_string_desc) / sizeof(s_ds4_string_desc[0]),
            .external_phy             = false,
            .configuration_descriptor = s_ds4_config_desc,
        };
        return true;
    case USB_GAMEPAD_MODE_SWITCH_PRO:
        *out_cfg = (tinyusb_config_t){
            .device_descriptor        = &s_switch_device_desc,
            .string_descriptor        = s_switch_string_desc,
            .string_descriptor_count  = sizeof(s_switch_string_desc) / sizeof(s_switch_string_desc[0]),
            .external_phy             = false,
            .configuration_descriptor = s_switch_config_desc,
        };
        return true;
    case USB_GAMEPAD_MODE_PS3:
        *out_cfg = (tinyusb_config_t){
            .device_descriptor        = &s_ps3_device_desc,
            .string_descriptor        = s_ps3_string_desc,
            .string_descriptor_count  = sizeof(s_ps3_string_desc) / sizeof(s_ps3_string_desc[0]),
            .external_phy             = false,
            .configuration_descriptor = s_ps3_config_desc,
        };
        return true;
    default:
        return false;
    }
}

const uint8_t *usb_modes_extra_get_report_desc(usb_gamepad_mode_t mode) {
    switch (mode) {
    case USB_GAMEPAD_MODE_DS4:        return s_ds4_report_desc;
    case USB_GAMEPAD_MODE_SWITCH_PRO: return s_switch_report_desc;
    case USB_GAMEPAD_MODE_PS3:        return s_ps3_report_desc;
    default:                          return NULL;
    }
}

uint16_t usb_modes_extra_get_report(usb_gamepad_mode_t mode,
                                    uint8_t report_id,
                                    hid_report_type_t report_type,
                                    uint8_t *buffer,
                                    uint16_t reqlen,
                                    const usb_canonical_input_t *in) {
    (void)report_type;
    switch (mode) {
    case USB_GAMEPAD_MODE_DS4:
        return ds4_get_report(report_id, buffer, reqlen, in);
    case USB_GAMEPAD_MODE_PS3:
        return ps3_get_report(report_id, buffer, reqlen, in);
    case USB_GAMEPAD_MODE_SWITCH_PRO:
        if (report_id == 0x30 && reqlen >= 63) {
            uint8_t pkt[64];
            sw_build_input_30(pkt, in);
            memcpy(buffer, pkt + 1, 63);
            return 63;
        }
        return 0;
    default:
        return 0;
    }
}

void usb_modes_extra_set_report(usb_gamepad_mode_t mode,
                                uint8_t report_id,
                                hid_report_type_t report_type,
                                const uint8_t *buffer,
                                uint16_t bufsize,
                                const usb_canonical_input_t *in,
                                bool *out_kick_tx) {
    if (out_kick_tx) *out_kick_tx = false;
    switch (mode) {
    case USB_GAMEPAD_MODE_DS4:
        ds4_set_report(report_id, buffer, bufsize);
        break;
    case USB_GAMEPAD_MODE_SWITCH_PRO: {
        bool kick = false;
        sw_set_report(report_id, buffer, bufsize, in, &kick);
        if (out_kick_tx) *out_kick_tx = kick;
        break;
    }
    case USB_GAMEPAD_MODE_PS3:
        ps3_set_report(report_id, report_type, buffer, bufsize);
        break;
    default:
        break;
    }
}

bool usb_modes_extra_has_pending_tx(usb_gamepad_mode_t mode) {
    return (mode == USB_GAMEPAD_MODE_SWITCH_PRO) && (s_sw_q_head != s_sw_q_tail);
}

int64_t usb_modes_extra_keepalive_us(usb_gamepad_mode_t mode) {
    if (mode == USB_GAMEPAD_MODE_SWITCH_PRO) {
        return s_sw_streaming ? 16000 : 100000; /* ~60 Hz steady stream when active */
    }
    return 100000;
}

bool usb_modes_extra_flush_tx(usb_gamepad_mode_t mode,
                              const usb_canonical_input_t *in,
                              bool input_dirty,
                              bool *out_sent_input) {
    if (out_sent_input) *out_sent_input = false;
    if (!tud_mounted() || !tud_hid_ready()) return false;

    if (mode == USB_GAMEPAD_MODE_SWITCH_PRO) {
        if (s_sw_q_head != s_sw_q_tail) {
            uint8_t pkt[64];
            memcpy(pkt, s_sw_q[s_sw_q_tail], 64);
            s_sw_q_tail = (uint8_t)((s_sw_q_tail + 1u) % SW_REPLY_Q_CAP);
            tud_hid_report(pkt[0], pkt + 1, 63);
            return true;
        }
        if (s_sw_streaming && input_dirty) {
            uint8_t pkt[64];
            sw_build_input_30(pkt, in);
            tud_hid_report(0x30, pkt + 1, 63);
            if (out_sent_input) *out_sent_input = true;
            return true;
        }
        return false;
    }

    if (!input_dirty) return false;

    if (mode == USB_GAMEPAD_MODE_DS4) {
        uint8_t payload[63];
        ds4_build_input(payload, in);
        tud_hid_report(0x01, payload, sizeof(payload));
        if (out_sent_input) *out_sent_input = true;
        return true;
    }

    if (mode == USB_GAMEPAD_MODE_PS3) {
        uint8_t payload[48];
        ps3_build_input(payload, in);
        tud_hid_report(0x01, payload, sizeof(payload));
        if (out_sent_input) *out_sent_input = true;
        return true;
    }

    return false;
}
