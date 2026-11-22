/* Composite USB Gamepad (Stadia HID, XInput, DS4, Switch Pro, PS3) + Bidirectional UAC1 Audio
 * + Experimental 1..4 Player Multi-Controller Co-Op mode. */
#include "usb_gamepad.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "class/hid/hid_device.h"
#include "device/usbd_pvt.h"
#include "usb_audio.h"
#include "usb_modes_extra.h"

#define TAG "USB-STADIA"

#define NVS_NS_USB          "stadiacast"
#define NVS_KEY_USB_MODE    "usb_mode"
#define NVS_KEY_MAX_PLAYERS "max_players"
#define NVS_KEY_STICK_DZ    "stick_dz"
#define NVS_KEY_LAST_BATT   "last_batt"

#define INPUT_REPORT_ID     3
#define BATTERY_REPORT_ID   4
#define RUMBLE_REPORT_ID    5
#define TRIGGER_THRESHOLD   30

#define XINPUT_DESC_LEN     (9 + 17 + 7 + 7)
#define HID_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + UAC1_AUDIO_DESC_LEN)
#define XIN_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + XINPUT_DESC_LEN + UAC1_AUDIO_DESC_LEN)

#define UAC1_AUDIO_DESC_BODY UAC1_AUDIO_DESC_BODY_AT(USB_ITF_AUDIO_CONTROL, USB_ITF_AUDIO_STREAMING_SPK, USB_ITF_AUDIO_STREAMING_MIC)

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

typedef struct __attribute__((packed)) {
    uint8_t report_id;    /* 0x00 */
    uint8_t report_size;  /* 0x14 (20) */
    uint8_t buttons_0;    /* Up, Down, Left, Right, Start, Back, L3, R3 */
    uint8_t buttons_1;    /* LB, RB, Guide, 0, A, B, X, Y */
    uint8_t lt;           /* 0..255 */
    uint8_t rt;           /* 0..255 */
    int16_t lx;           /* -32768..32767 */
    int16_t ly;           /* -32768..32767 (Y up = positive) */
    int16_t rx;           /* -32768..32767 */
    int16_t ry;           /* -32768..32767 (Y up = positive) */
    uint8_t reserved[6];
} xinput_report_t;

_Static_assert(sizeof(xinput_report_t) == 20, "xinput_report_t must be 20 bytes");

/* ---------------- Mode 0: Stadia HID (18d1:9400) Descriptors ---------------- */
static const uint8_t s_hid_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x81, 0x03, 0x05, 0x09, 0x09, 0x12, 0x09, 0x11, 0x09, 0x14, 0x09, 0x13, 0x09, 0x0D, 0x09, 0x0C, 0x09, 0x0B, 0x09, 0x0F, 0x09, 0x0E, 0x09, 0x08, 0x09, 0x07, 0x09, 0x05, 0x09, 0x04, 0x09, 0x02, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x01, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x05, 0x02, 0x09, 0xC5, 0x09, 0xC4, 0x15, 0x00, 0x95, 0x02, 0x81, 0x02, 0x85, 0x05, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x02, 0x91, 0x02,
    /* Report ID 4: Battery Strength (Usage Page 0x06 Generic Device Controls, Usage 0x20, 0..100%) */
    0x85, 0x04, 0x05, 0x06, 0x09, 0x20, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0
};

static const tusb_desc_device_t s_hid_device_desc = {
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

static const char *s_hid_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Google Inc.",
    "Stadia Wireless Adapter",
    "STADIA-WIFI-S3",
};

static const uint8_t s_hid_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, HID_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* ITF 0: Stadia HID Gamepad + Rumble (EP 0x01 OUT, EP 0x81 IN, 64B, 1 ms) */
    TUD_HID_INOUT_DESCRIPTOR(USB_ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), 0x01, 0x81, 64, 1),
    UAC1_AUDIO_DESC_BODY,
};

_Static_assert(sizeof(s_hid_config_desc) == HID_DESC_TOTAL_LEN, "s_hid_config_desc size mismatch");

/* ---------------- Mode 1: XInput (1209:9401) + MS OS 2.0 Descriptors ---------------- */
static const tusb_desc_device_t s_xinput_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210, /* USB 2.1 enables MS OS 2.0 BOS descriptor probing on Windows */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x9401,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

static const char *s_xinput_string_desc[] = {
    (const char[]){ 0x09, 0x04 },
    "Stadiacast",
    "Stadia Wireless Adapter XInput",
    "STADIA-XIN-S3",
};

static const uint8_t s_xinput_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_TOTAL, 0, XIN_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    /* ITF 0: XInput Gamepad (Class 0xFF, SubClass 0x5D, Protocol 0x01, EP 0x81 IN 32B 1ms, EP 0x01 OUT 32B 8ms) */
    9, TUSB_DESC_INTERFACE, USB_ITF_HID, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
    /* Standard Xbox 360 17-byte vendor descriptor (type 0x21) */
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 1,
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 8,
    UAC1_AUDIO_DESC_BODY,
};

_Static_assert(sizeof(s_xinput_config_desc) == XIN_DESC_TOTAL_LEN, "s_xinput_config_desc size mismatch");

#define MS_OS_20_VENDOR_CODE    0x20
#define MS_OS_20_DESC_SET_LEN   46
#define BOS_TOTAL_LEN           (5 + 28)

static uint8_t s_multi_config_desc[256];
static uint8_t s_multi_bos_desc[BOS_TOTAL_LEN];
static uint8_t s_multi_ms_os_20_desc_set[10 + 8 + USB_MAX_PLAYERS * 28];
static uint16_t s_multi_ms_os_20_len = MS_OS_20_DESC_SET_LEN;

/* ---------------- Shared State & Core-1 Marshaling ---------------- */
static usb_gamepad_mode_t s_mode = USB_GAMEPAD_MODE_STADIA_HID;
static uint8_t  s_max_players = 1;
static uint16_t s_stick_deadzone = 0; /* 0..8192 (0=raw, 1310=4%, 2621=8%) */
static portMUX_TYPE s_hid_mux = portMUX_INITIALIZER_UNLOCKED;

static stadia_hid_report_t  s_latest_hid[USB_MAX_PLAYERS];
static xinput_report_t      s_latest_xin[USB_MAX_PLAYERS];
static usb_canonical_input_t s_latest_canon[USB_MAX_PLAYERS];
static bool     s_dirty[USB_MAX_PLAYERS];
static bool     s_batt_dirty[USB_MAX_PLAYERS];
static uint8_t  s_battery_pct[USB_MAX_PLAYERS];
static int64_t  s_last_tx_us[USB_MAX_PLAYERS];

static int8_t   s_saved_batt_pct = -1;
static bool     s_headset_plugged = false;
static bool     s_kick_pending = false;
static usb_rumble_cb_t      s_rumble_cb = NULL;
static usb_rumble_slot_cb_t s_rumble_slot_cb = NULL;
static bool     s_usb_installed = false;

/* ---------------- Custom TinyUSB XInput Class Driver (1..4 Players) ---------------- */
static uint8_t s_xin_rhport = 0;
static uint8_t s_xin_ep_in[USB_MAX_PLAYERS];
static uint8_t s_xin_ep_out[USB_MAX_PLAYERS];
static uint8_t s_xin_tx_buf[USB_MAX_PLAYERS][32];
static uint8_t s_xin_rx_buf[USB_MAX_PLAYERS][32];

static void hid_flush_on_tusb_task(void *arg);

static void xinput_init(void) {
    memset(s_xin_ep_in, 0, sizeof(s_xin_ep_in));
    memset(s_xin_ep_out, 0, sizeof(s_xin_ep_out));
}

static bool xinput_deinit(void) {
    memset(s_xin_ep_in, 0, sizeof(s_xin_ep_in));
    memset(s_xin_ep_out, 0, sizeof(s_xin_ep_out));
    return true;
}

static void xinput_reset(uint8_t rhport) {
    (void)rhport;
    memset(s_xin_ep_in, 0, sizeof(s_xin_ep_in));
    memset(s_xin_ep_out, 0, sizeof(s_xin_ep_out));
}

static uint16_t xinput_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    if (itf_desc->bInterfaceClass != 0xFF ||
        itf_desc->bInterfaceSubClass != 0x5D ||
        itf_desc->bInterfaceProtocol != 0x01) {
        return 0;
    }

    uint8_t slot = (itf_desc->bInterfaceNumber < USB_MAX_PLAYERS) ? itf_desc->bInterfaceNumber : 0;
    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p_desc = tu_desc_next(itf_desc);
    uint8_t found = 0;

    while (drv_len < max_len && found < itf_desc->bNumEndpoints) {
        if (p_desc[0] == 0) return 0;
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
            if (!usbd_edpt_open(rhport, ep)) return 0;
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                s_xin_ep_in[slot] = ep->bEndpointAddress;
            } else {
                s_xin_ep_out[slot] = ep->bEndpointAddress;
            }
            found++;
        }
        drv_len += p_desc[0];
        p_desc = tu_desc_next(p_desc);
    }

    s_xin_rhport = rhport;
    if (s_xin_ep_out[slot]) {
        usbd_edpt_xfer(rhport, s_xin_ep_out[slot], s_xin_rx_buf[slot], sizeof(s_xin_rx_buf[slot]), false);
    }
    return drv_len;
}

static bool xinput_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    (void)rhport; (void)stage; (void)request;
    return true;
}

static bool xinput_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    for (int slot = 0; slot < (int)s_max_players; slot++) {
        if (ep_addr == s_xin_ep_out[slot] && s_xin_ep_out[slot] != 0) {
            if (result == XFER_RESULT_SUCCESS && xferred_bytes >= 6 && s_xin_rx_buf[slot][0] == 0x00) {
                uint16_t strong = (uint16_t)s_xin_rx_buf[slot][3] * 257u;
                uint16_t weak   = (uint16_t)s_xin_rx_buf[slot][4] * 257u;
                if (s_rumble_slot_cb) {
                    s_rumble_slot_cb(slot, strong, weak);
                } else if (slot == 0 && s_rumble_cb) {
                    s_rumble_cb(strong, weak);
                }
            }
            usbd_edpt_xfer(rhport, s_xin_ep_out[slot], s_xin_rx_buf[slot], sizeof(s_xin_rx_buf[slot]), false);
            return true;
        } else if (ep_addr == s_xin_ep_in[slot] && s_xin_ep_in[slot] != 0) {
            hid_flush_on_tusb_task(NULL);
            return true;
        }
    }
    return true;
}

static usbd_class_driver_t const s_xinput_driver = {
    .name            = "XINPUT",
    .init            = xinput_init,
    .deinit          = xinput_deinit,
    .reset           = xinput_reset,
    .open            = xinput_open,
    .control_xfer_cb = xinput_control_xfer_cb,
    .xfer_cb         = xinput_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_xinput_driver;
}

/* ---------------- MS OS 2.0 Callbacks (XInput mode only) ---------------- */
static void build_ms_os_20_descriptors(uint8_t players) {
    uint16_t cfg_subset_len = (uint16_t)(8 + (uint16_t)players * 28u);
    uint16_t total_set_len  = (uint16_t)(10 + cfg_subset_len);
    s_multi_ms_os_20_len = total_set_len;

    const uint8_t bos_tmpl[BOS_TOTAL_LEN] = {
        5, TUSB_DESC_BOS, U16_TO_U8S_LE(BOS_TOTAL_LEN), 1,
        28, TUSB_DESC_DEVICE_CAPABILITY, 0x05 /* PLATFORM */, 0x00,
        0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
        0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
        0x00, 0x00, 0x03, 0x06,
        (uint8_t)(total_set_len & 0xFF), (uint8_t)(total_set_len >> 8),
        MS_OS_20_VENDOR_CODE, 0x00,
    };
    memcpy(s_multi_bos_desc, bos_tmpl, sizeof(bos_tmpl));

    uint8_t *p = s_multi_ms_os_20_desc_set;
    /* Set Header (10 bytes) */
    uint8_t hdr[18] = {
        U16_TO_U8S_LE(10), U16_TO_U8S_LE(0x0000), 0x00, 0x00, 0x03, 0x06,
        (uint8_t)(total_set_len & 0xFF), (uint8_t)(total_set_len >> 8),
        /* Configuration Subset Header (8 bytes) */
        U16_TO_U8S_LE(8), U16_TO_U8S_LE(0x0001), 0x00, 0x00,
        (uint8_t)(cfg_subset_len & 0xFF), (uint8_t)(cfg_subset_len >> 8),
    };
    memcpy(p, hdr, sizeof(hdr));
    p += sizeof(hdr);

    for (uint8_t i = 0; i < players; i++) {
        uint8_t fn[28] = {
            /* Function Subset Header (8 bytes): bFirstInterface = i, wSubsetLength = 28 */
            U16_TO_U8S_LE(8), U16_TO_U8S_LE(0x0002), i, 0x00, U16_TO_U8S_LE(28),
            /* Compatible ID Descriptor (20 bytes): "XUSB10\0\0" */
            U16_TO_U8S_LE(20), U16_TO_U8S_LE(0x0003),
            'X', 'U', 'S', 'B', '1', '0', 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        memcpy(p, fn, sizeof(fn));
        p += sizeof(fn);
    }
}

static void build_multi_config_descriptor(usb_gamepad_mode_t mode, uint8_t players) {
    uint8_t *p = s_multi_config_desc;
    if (mode == USB_GAMEPAD_MODE_XINPUT) {
        uint16_t total_len = (uint16_t)(TUD_CONFIG_DESC_LEN + (uint16_t)players * XINPUT_DESC_LEN);
        uint8_t cfg_hdr[TUD_CONFIG_DESC_LEN] = {
            TUD_CONFIG_DESCRIPTOR(1, players, 0, total_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250)
        };
        memcpy(p, cfg_hdr, sizeof(cfg_hdr));
        p += sizeof(cfg_hdr);
        for (uint8_t i = 0; i < players; i++) {
            uint8_t ep_in  = (uint8_t)(0x81u + i);
            uint8_t ep_out = (uint8_t)(0x01u + i);
            uint8_t itf[XINPUT_DESC_LEN] = {
                9, TUSB_DESC_INTERFACE, i, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
                0x11, 0x21, 0x00, 0x01, 0x01, 0x25, ep_in, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, ep_out, 0x08, 0x00, 0x00,
                7, TUSB_DESC_ENDPOINT, ep_in,  TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 1,
                7, TUSB_DESC_ENDPOINT, ep_out, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 8,
            };
            memcpy(p, itf, sizeof(itf));
            p += sizeof(itf);
        }
    } else {
        uint16_t total_len = (uint16_t)(TUD_CONFIG_DESC_LEN + (uint16_t)players * TUD_HID_INOUT_DESC_LEN);
        uint8_t cfg_hdr[TUD_CONFIG_DESC_LEN] = {
            TUD_CONFIG_DESCRIPTOR(1, players, 0, total_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250)
        };
        memcpy(p, cfg_hdr, sizeof(cfg_hdr));
        p += sizeof(cfg_hdr);
        for (uint8_t i = 0; i < players; i++) {
            uint8_t ep_in  = (uint8_t)(0x81u + i);
            uint8_t ep_out = (uint8_t)(0x01u + i);
            uint8_t itf[TUD_HID_INOUT_DESC_LEN] = {
                TUD_HID_INOUT_DESCRIPTOR(i, 0, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), ep_out, ep_in, 64, 1)
            };
            memcpy(p, itf, sizeof(itf));
            p += sizeof(itf);
        }
    }
}

uint8_t const *tud_descriptor_bos_cb(void) {
    return (s_mode == USB_GAMEPAD_MODE_XINPUT) ? s_multi_bos_desc : NULL;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (s_mode == USB_GAMEPAD_MODE_XINPUT &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == MS_OS_20_VENDOR_CODE &&
        request->wIndex == 0x0007) {
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_multi_ms_os_20_desc_set, s_multi_ms_os_20_len);
    }
    return false;
}

/* ---------------- Core-1 TX Marshaling (HID + XInput + Extra Modes) ---------------- */
static void hid_flush_on_tusb_task(void *arg) {
    (void)arg;

    if (s_mode >= USB_GAMEPAD_MODE_DS4) {
        usb_canonical_input_t canon;
        bool should_send = false;
        portENTER_CRITICAL(&s_hid_mux);
        s_kick_pending = false;
        canon = s_latest_canon[0];
        canon.battery_pct = s_battery_pct[0];
        canon.headset_plugged = s_headset_plugged;
        if (s_dirty[0]) {
            s_dirty[0] = false;
            should_send = true;
        }
        portEXIT_CRITICAL(&s_hid_mux);

        bool sent_input = false;
        if (usb_modes_extra_flush_tx(s_mode, &canon, should_send, &sent_input)) {
            if (sent_input) {
                s_last_tx_us[0] = esp_timer_get_time();
            } else if (should_send) {
                portENTER_CRITICAL(&s_hid_mux);
                s_dirty[0] = true;
                portEXIT_CRITICAL(&s_hid_mux);
            }
        } else if (should_send) {
            portENTER_CRITICAL(&s_hid_mux);
            s_dirty[0] = true;
            portEXIT_CRITICAL(&s_hid_mux);
        }
        return;
    }

    portENTER_CRITICAL(&s_hid_mux);
    s_kick_pending = false;
    portEXIT_CRITICAL(&s_hid_mux);

    for (int slot = 0; slot < (int)s_max_players; slot++) {
        stadia_hid_report_t hid_rep;
        xinput_report_t xin_rep;
        bool should_send = false;
        bool send_batt = false;
        uint8_t batt_pct = 100;

        portENTER_CRITICAL(&s_hid_mux);
        if (s_dirty[slot]) {
            hid_rep = s_latest_hid[slot];
            xin_rep = s_latest_xin[slot];
            s_dirty[slot] = false;
            should_send = true;
        } else if (s_batt_dirty[slot] && s_mode == USB_GAMEPAD_MODE_STADIA_HID) {
            batt_pct = s_battery_pct[slot];
            s_batt_dirty[slot] = false;
            send_batt = true;
        }
        portEXIT_CRITICAL(&s_hid_mux);

        if (should_send) {
            if (s_mode == USB_GAMEPAD_MODE_XINPUT) {
                uint8_t ep_in = s_xin_ep_in[slot];
                if (tud_mounted() && ep_in && !usbd_edpt_busy(s_xin_rhport, ep_in) &&
                    usbd_edpt_claim(s_xin_rhport, ep_in)) {
                    memcpy(s_xin_tx_buf[slot], &xin_rep, sizeof(xin_rep));
                    s_last_tx_us[slot] = esp_timer_get_time();
                    usbd_edpt_xfer(s_xin_rhport, ep_in, s_xin_tx_buf[slot], sizeof(xin_rep), false);
                } else {
                    portENTER_CRITICAL(&s_hid_mux);
                    s_dirty[slot] = true;
                    portEXIT_CRITICAL(&s_hid_mux);
                }
            } else {
                if (tud_mounted() && tud_hid_n_ready((uint8_t)slot)) {
                    s_last_tx_us[slot] = esp_timer_get_time();
                    tud_hid_n_report((uint8_t)slot, INPUT_REPORT_ID, &hid_rep, sizeof(hid_rep));
                } else {
                    portENTER_CRITICAL(&s_hid_mux);
                    s_dirty[slot] = true;
                    portEXIT_CRITICAL(&s_hid_mux);
                }
            }
        } else if (send_batt) {
            if (tud_mounted() && tud_hid_n_ready((uint8_t)slot)) {
                uint8_t lvl = batt_pct;
                tud_hid_n_report((uint8_t)slot, BATTERY_REPORT_ID, &lvl, sizeof(lvl));
            } else {
                portENTER_CRITICAL(&s_hid_mux);
                s_batt_dirty[slot] = true;
                portEXIT_CRITICAL(&s_hid_mux);
            }
        }
    }
}

/* ---------------- TinyUSB HID Callbacks (All HID Modes) ---------------- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    if (s_mode >= USB_GAMEPAD_MODE_DS4) {
        const uint8_t *rd = usb_modes_extra_get_report_desc(s_mode);
        if (rd) return rd;
    }
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    int slot = (instance < USB_MAX_PLAYERS) ? (int)instance : 0;
    if (s_mode >= USB_GAMEPAD_MODE_DS4) {
        usb_canonical_input_t canon;
        portENTER_CRITICAL(&s_hid_mux);
        canon = s_latest_canon[0];
        canon.battery_pct = s_battery_pct[0];
        canon.headset_plugged = s_headset_plugged;
        portEXIT_CRITICAL(&s_hid_mux);
        return usb_modes_extra_get_report(s_mode, report_id, report_type, buffer, reqlen, &canon);
    }
    (void)report_type;
    if (report_id == INPUT_REPORT_ID && reqlen >= sizeof(s_latest_hid[0])) {
        portENTER_CRITICAL(&s_hid_mux);
        memcpy(buffer, &s_latest_hid[slot], sizeof(s_latest_hid[0]));
        portEXIT_CRITICAL(&s_hid_mux);
        return sizeof(s_latest_hid[0]);
    }
    if (report_id == BATTERY_REPORT_ID && reqlen >= 1) {
        portENTER_CRITICAL(&s_hid_mux);
        uint8_t pct = s_battery_pct[slot];
        portEXIT_CRITICAL(&s_hid_mux);
        buffer[0] = pct;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    int slot = (instance < USB_MAX_PLAYERS) ? (int)instance : 0;
    if (s_mode >= USB_GAMEPAD_MODE_DS4) {
        usb_canonical_input_t canon;
        portENTER_CRITICAL(&s_hid_mux);
        canon = s_latest_canon[0];
        canon.battery_pct = s_battery_pct[0];
        canon.headset_plugged = s_headset_plugged;
        portEXIT_CRITICAL(&s_hid_mux);
        bool kick = false;
        usb_modes_extra_set_report(s_mode, report_id, report_type, buffer, bufsize, &canon, &kick);
        if (kick) {
            hid_flush_on_tusb_task(NULL);
        }
        return;
    }
    (void)report_type;
    if (!buffer) return;
    uint16_t left = 0, right = 0;
    bool got_rmb = false;
    if (report_id == RUMBLE_REPORT_ID && bufsize >= 4) {
        left  = (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
        right = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
        got_rmb = true;
    } else if (report_id == 0 && bufsize >= 5 && buffer[0] == RUMBLE_REPORT_ID) {
        left  = (uint16_t)buffer[1] | ((uint16_t)buffer[2] << 8);
        right = (uint16_t)buffer[3] | ((uint16_t)buffer[4] << 8);
        got_rmb = true;
    }
    if (got_rmb) {
        if (s_rumble_slot_cb) {
            s_rumble_slot_cb(slot, left, right);
        } else if (slot == 0 && s_rumble_cb) {
            s_rumble_cb(left, right);
        }
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance; (void)report; (void)len;
    hid_flush_on_tusb_task(NULL);
}

/* ---------------- Input Packing ---------------- */
static uint8_t axis_to_u8(int32_t v) {
    if (v < -32768) v = -32768;
    if (v >  32767) v =  32767;
    int32_t s = ((v + 32768) * 254 + 32767) / 65535 + 1;
    if (s < 1) s = 1;
    if (s > 255) s = 255;
    return (uint8_t)s;
}

static int16_t clamp_i16(int32_t v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

/* Smooth rescaled radial deadzone (zero jump past edge; bypass when dz == 0) */
static void apply_radial_deadzone(int32_t *x, int32_t *y, uint16_t dz) {
    if (dz == 0) return;
    float fx = (float)(*x);
    float fy = (float)(*y);
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag <= (float)dz) {
        *x = 0;
        *y = 0;
        return;
    }
    float max_r = 32767.0f;
    float norm = (mag - (float)dz) / (max_r - (float)dz);
    if (norm > 1.0f) norm = 1.0f;
    float scale = (norm * max_r) / mag;
    *x = (int32_t)lroundf(fx * scale);
    *y = (int32_t)lroundf(fy * scale);
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

static void push_reports_slot(int slot,
                              const stadia_hid_report_t *hid,
                              const xinput_report_t *xin,
                              const usb_canonical_input_t *canon) {
    if (slot < 0 || slot >= USB_MAX_PLAYERS) return;
    bool kick = false;
    portENTER_CRITICAL(&s_hid_mux);
    s_latest_hid[slot]   = *hid;
    s_latest_xin[slot]   = *xin;
    s_latest_canon[slot] = *canon;
    s_dirty[slot]        = true;
    if (s_usb_installed && !s_kick_pending) {
        s_kick_pending = true;
        kick = true;
    }
    portEXIT_CRITICAL(&s_hid_mux);
    if (kick) {
        usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
    }
}

void usb_gamepad_send_slot(int slot,
                           uint32_t b,
                           int32_t lx, int32_t ly,
                           int32_t rx, int32_t ry,
                           uint32_t lt, uint32_t rt) {
    if (slot < 0 || slot >= USB_MAX_PLAYERS) return;
    uint16_t dz = s_stick_deadzone;
    if (dz > 0) {
        apply_radial_deadzone(&lx, &ly, dz);
        apply_radial_deadzone(&rx, &ry, dz);
    }

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
    uint8_t lt8 = (uint8_t)(lt > 255 ? 255 : lt);
    uint8_t rt8 = (uint8_t)(rt > 255 ? 255 : rt);

    stadia_hid_report_t hid = {
        .hat  = dpad_to_hat(b),
        .btn2 = btn2,
        .btn3 = btn3,
        .lx   = axis_to_u8(lx),
        .ly   = axis_to_u8(inv_ly),
        .rx   = axis_to_u8(rx),
        .ry   = axis_to_u8(inv_ry),
        .lt   = lt8,
        .rt   = rt8,
    };

    uint8_t xin_b1 = 0;
    if (b & (1u <<  8)) xin_b1 |= 0x01; /* LB */
    if (b & (1u <<  9)) xin_b1 |= 0x02; /* RB */
    if (b & (1u << 16)) xin_b1 |= 0x04; /* Guide / Stadia */
    if (b & (1u << 12)) xin_b1 |= 0x10; /* A */
    if (b & (1u << 13)) xin_b1 |= 0x20; /* B */
    if (b & (1u << 14)) xin_b1 |= 0x40; /* X */
    if (b & (1u << 15)) xin_b1 |= 0x80; /* Y */

    xinput_report_t xin = {
        .report_id   = 0x00,
        .report_size = 0x14,
        .buttons_0   = (uint8_t)(b & 0xFFu),
        .buttons_1   = xin_b1,
        .lt          = lt8,
        .rt          = rt8,
        .lx          = clamp_i16(lx),
        .ly          = clamp_i16(ly),
        .rx          = clamp_i16(rx),
        .ry          = clamp_i16(ry),
    };

    usb_canonical_input_t canon = {
        .buttons = b,
        .lx      = lx,
        .ly      = ly,
        .rx      = rx,
        .ry      = ry,
        .lt      = lt8,
        .rt      = rt8,
    };

    push_reports_slot(slot, &hid, &xin, &canon);
}

void usb_gamepad_send(uint32_t b,
                      int32_t lx, int32_t ly,
                      int32_t rx, int32_t ry,
                      uint32_t lt, uint32_t rt) {
    usb_gamepad_send_slot(0, b, lx, ly, rx, ry, lt, rt);
}

void usb_gamepad_get_latest_input_slot(int slot,
                                       uint32_t *buttons,
                                       int32_t *lx, int32_t *ly,
                                       int32_t *rx, int32_t *ry,
                                       uint8_t *lt, uint8_t *rt) {
    if (slot < 0 || slot >= USB_MAX_PLAYERS) slot = 0;
    usb_canonical_input_t c;
    portENTER_CRITICAL(&s_hid_mux);
    c = s_latest_canon[slot];
    portEXIT_CRITICAL(&s_hid_mux);
    if (buttons) *buttons = c.buttons;
    if (lx)      *lx      = c.lx;
    if (ly)      *ly      = c.ly;
    if (rx)      *rx      = c.rx;
    if (ry)      *ry      = c.ry;
    if (lt)      *lt      = c.lt;
    if (rt)      *rt      = c.rt;
}

void usb_gamepad_get_latest_input(uint32_t *buttons,
                                  int32_t *lx, int32_t *ly,
                                  int32_t *rx, int32_t *ry,
                                  uint8_t *lt, uint8_t *rt) {
    usb_gamepad_get_latest_input_slot(0, buttons, lx, ly, rx, ry, lt, rt);
}

void usb_gamepad_reset_slot(int slot) {
    if (slot < 0 || slot >= USB_MAX_PLAYERS) return;
    stadia_hid_report_t hid_neutral = { .hat = 8, .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
    xinput_report_t xin_neutral = { .report_id = 0x00, .report_size = 0x14 };
    usb_canonical_input_t canon_neutral = { 0 };
    push_reports_slot(slot, &hid_neutral, &xin_neutral, &canon_neutral);
}

void usb_gamepad_reset(void) {
    for (int slot = 0; slot < (int)s_max_players; slot++) {
        usb_gamepad_reset_slot(slot);
    }
}

void usb_gamepad_keepalive_tick(void) {
    if (!s_usb_installed) return;
    int64_t now = esp_timer_get_time();
    int64_t interval_us = (s_mode >= USB_GAMEPAD_MODE_DS4)
                          ? usb_modes_extra_keepalive_us(s_mode)
                          : 100000;
    bool need_kick = false;
    portENTER_CRITICAL(&s_hid_mux);
    for (int slot = 0; slot < (int)s_max_players; slot++) {
        if ((now - s_last_tx_us[slot]) >= interval_us) {
            s_dirty[slot] = true;
            need_kick = true;
        }
    }
    if (need_kick && !s_kick_pending) {
        s_kick_pending = true;
    } else {
        need_kick = false;
    }
    portEXIT_CRITICAL(&s_hid_mux);
    if (need_kick) {
        usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
    }
}

/* ---------------- NVS Mode & Multi-Player Persistence & Init ---------------- */
static usb_gamepad_mode_t load_mode_from_nvs(void) {
    nvs_handle_t h;
    uint8_t val = (uint8_t)USB_GAMEPAD_MODE_STADIA_HID;
    if (nvs_open(NVS_NS_USB, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_USB_MODE, &val);
        nvs_close(h);
    }
    return (val < (uint8_t)USB_GAMEPAD_MODE_COUNT)
           ? (usb_gamepad_mode_t)val
           : USB_GAMEPAD_MODE_STADIA_HID;
}

static uint8_t load_max_players_from_nvs(void) {
    nvs_handle_t h;
    uint8_t val = 1;
    if (nvs_open(NVS_NS_USB, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_MAX_PLAYERS, &val);
        nvs_close(h);
    }
    if (val < 1 || val > USB_MAX_PLAYERS) val = 1;
    return val;
}

uint8_t usb_gamepad_get_max_players(void) {
    return s_max_players;
}

bool usb_gamepad_save_max_players(uint8_t count) {
    if (count < 1 || count > USB_MAX_PLAYERS) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_USB, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, NVS_KEY_MAX_PLAYERS, count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void usb_gamepad_set_rumble_slot_cb(usb_rumble_slot_cb_t cb) {
    s_rumble_slot_cb = cb;
}

usb_gamepad_mode_t usb_gamepad_get_mode(void) {
    return s_mode;
}

const char *usb_gamepad_mode_name(usb_gamepad_mode_t mode) {
    switch (mode) {
    case USB_GAMEPAD_MODE_STADIA_HID: return "Stadia HID";
    case USB_GAMEPAD_MODE_XINPUT:     return "XInput";
    case USB_GAMEPAD_MODE_DS4:        return "DualShock 4";
    case USB_GAMEPAD_MODE_SWITCH_PRO: return "Switch Pro";
    case USB_GAMEPAD_MODE_PS3:        return "PS3 (DS3)";
    default:                          return "Unknown";
    }
}

bool usb_gamepad_mode_has_audio(usb_gamepad_mode_t mode) {
    if (s_max_players > 1) return false;
    return (mode == USB_GAMEPAD_MODE_STADIA_HID ||
            mode == USB_GAMEPAD_MODE_XINPUT ||
            mode == USB_GAMEPAD_MODE_DS4);
}

bool usb_gamepad_save_mode(usb_gamepad_mode_t mode) {
    if ((unsigned)mode >= (unsigned)USB_GAMEPAD_MODE_COUNT) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_USB, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, NVS_KEY_USB_MODE, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void usb_gamepad_set_deadzone(uint16_t dz) {
    if (dz > 8192u) dz = 8192u;
    s_stick_deadzone = dz;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_USB, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u16(h, NVS_KEY_STICK_DZ, dz) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

uint16_t usb_gamepad_get_deadzone(void) {
    return s_stick_deadzone;
}

void usb_gamepad_set_battery_slot(int slot, int pct) {
    if (slot < 0 || slot >= USB_MAX_PLAYERS || pct < 0) return;
    uint8_t val = (uint8_t)(pct > 100 ? 100 : pct);
    bool kick = false;
    portENTER_CRITICAL(&s_hid_mux);
    s_battery_pct[slot] = val;
    if (s_usb_installed) {
        if (s_mode == USB_GAMEPAD_MODE_STADIA_HID) {
            s_batt_dirty[slot] = true;
            if (!s_kick_pending) {
                s_kick_pending = true;
                kick = true;
            }
        } else if (slot == 0 && s_mode >= USB_GAMEPAD_MODE_DS4) {
            s_dirty[0] = true;
            if (!s_kick_pending) {
                s_kick_pending = true;
                kick = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_hid_mux);
    if (kick) {
        usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
    }
    if (slot == 0 && (int8_t)val != s_saved_batt_pct) {
        s_saved_batt_pct = (int8_t)val;
        nvs_handle_t h;
        if (nvs_open(NVS_NS_USB, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_u8(h, NVS_KEY_LAST_BATT, val) == ESP_OK) {
                (void)nvs_commit(h);
            }
            nvs_close(h);
        }
    }
}

void usb_gamepad_set_battery(int pct) {
    usb_gamepad_set_battery_slot(0, pct);
}

void usb_gamepad_set_headset(bool plugged) {
    bool kick = false;
    portENTER_CRITICAL(&s_hid_mux);
    if (s_headset_plugged != plugged) {
        s_headset_plugged = plugged;
        if (s_usb_installed && s_mode == USB_GAMEPAD_MODE_DS4) {
            s_dirty[0] = true;
            if (!s_kick_pending) {
                s_kick_pending = true;
                kick = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_hid_mux);
    if (kick) {
        usbd_defer_func(hid_flush_on_tusb_task, NULL, false);
    }
}

void usb_gamepad_disconnect(void) {
    if (s_usb_installed) {
        tud_disconnect();
    }
}

void usb_gamepad_uninstall(void) {
    if (s_usb_installed) {
        s_usb_installed = false;
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)tinyusb_driver_uninstall();
    }
}

void usb_gamepad_init(usb_rumble_cb_t rumble_cb) {
    s_rumble_cb = rumble_cb;
    s_mode = load_mode_from_nvs();
    s_max_players = load_max_players_from_nvs();

    for (int i = 0; i < USB_MAX_PLAYERS; i++) {
        s_latest_hid[i] = (stadia_hid_report_t){ .hat = 8, .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
        s_latest_xin[i] = (xinput_report_t){ .report_id = 0x00, .report_size = 0x14 };
        s_latest_canon[i] = (usb_canonical_input_t){ .battery_pct = 100 };
        s_battery_pct[i] = 100;
    }

    {
        nvs_handle_t h;
        uint16_t saved_dz = 0;
        uint8_t saved_batt = 100;
        if (nvs_open(NVS_NS_USB, NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u16(h, NVS_KEY_STICK_DZ, &saved_dz);
            if (nvs_get_u8(h, NVS_KEY_LAST_BATT, &saved_batt) == ESP_OK && saved_batt <= 100u) {
                s_battery_pct[0] = saved_batt;
                s_saved_batt_pct = (int8_t)saved_batt;
            }
            nvs_close(h);
        }
        if (saved_dz <= 8192u) s_stick_deadzone = saved_dz;
    }

    if (s_max_players > 1 && s_mode >= USB_GAMEPAD_MODE_DS4) {
        ESP_LOGW(TAG, "Experimental Multi-Controller (%u players) uses XInput or Stadia HID; defaulting to XInput",
                 (unsigned)s_max_players);
        s_mode = USB_GAMEPAD_MODE_XINPUT;
    }

    build_ms_os_20_descriptors(s_max_players);
    if (s_max_players > 1) {
        build_multi_config_descriptor(s_mode, s_max_players);
    }

    usb_modes_extra_init(s_mode, rumble_cb);
    usb_audio_init();

    tinyusb_config_t tusb_cfg;
    if (s_mode == USB_GAMEPAD_MODE_XINPUT) {
        tusb_cfg = (tinyusb_config_t){
            .device_descriptor        = &s_xinput_device_desc,
            .string_descriptor        = s_xinput_string_desc,
            .string_descriptor_count  = sizeof(s_xinput_string_desc) / sizeof(s_xinput_string_desc[0]),
            .external_phy             = false,
            .configuration_descriptor = (s_max_players > 1) ? s_multi_config_desc : s_xinput_config_desc,
        };
    } else if (s_max_players == 1 && s_mode >= USB_GAMEPAD_MODE_DS4 && usb_modes_extra_get_tusb_cfg(s_mode, &tusb_cfg)) {
        /* Configured by usb_modes_extra */
    } else {
        s_mode = USB_GAMEPAD_MODE_STADIA_HID;
        tusb_cfg = (tinyusb_config_t){
            .device_descriptor        = &s_hid_device_desc,
            .string_descriptor        = s_hid_string_desc,
            .string_descriptor_count  = sizeof(s_hid_string_desc) / sizeof(s_hid_string_desc[0]),
            .external_phy             = false,
            .configuration_descriptor = (s_max_players > 1) ? s_multi_config_desc : s_hid_config_desc,
        };
    }

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %d", (int)err);
        return;
    }
    s_usb_installed = true;
    ESP_LOGI(TAG, "USB mode=%s players=%u (%s) ready",
             usb_gamepad_mode_name(s_mode),
             (unsigned)s_max_players,
             usb_gamepad_mode_has_audio(s_mode) ? "+ UAC1 audio" : "HID-only");
}
