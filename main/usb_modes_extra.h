/* Extra USB HID Controller Modes: DualShock 4 v2 (054c:09cc), Switch Pro (057e:2009), PS3 DS3 (054c:0268). */
#ifndef USB_MODES_EXTRA_H
#define USB_MODES_EXTRA_H

#include <stdbool.h>
#include <stdint.h>
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "usb_gamepad.h"

typedef struct {
    uint32_t buttons;         /* Stadia button bitmask */
    int32_t  lx;              /* -32768..32767 (right = +) */
    int32_t  ly;              /* -32768..32767 (up = +) */
    int32_t  rx;              /* -32768..32767 (right = +) */
    int32_t  ry;              /* -32768..32767 (up = +) */
    uint8_t  lt;              /* 0..255 */
    uint8_t  rt;              /* 0..255 */
    uint8_t  battery_pct;     /* 0..100 */
    bool     headset_plugged;
} usb_canonical_input_t;

void usb_modes_extra_init(usb_gamepad_mode_t mode, usb_rumble_cb_t rumble_cb);
bool usb_modes_extra_get_tusb_cfg(usb_gamepad_mode_t mode, tinyusb_config_t *out_cfg);
const uint8_t *usb_modes_extra_get_report_desc(usb_gamepad_mode_t mode);

uint16_t usb_modes_extra_get_report(usb_gamepad_mode_t mode,
                                    uint8_t report_id,
                                    hid_report_type_t report_type,
                                    uint8_t *buffer,
                                    uint16_t reqlen,
                                    const usb_canonical_input_t *in);

void usb_modes_extra_set_report(usb_gamepad_mode_t mode,
                                uint8_t report_id,
                                hid_report_type_t report_type,
                                const uint8_t *buffer,
                                uint16_t bufsize,
                                const usb_canonical_input_t *in,
                                bool *out_kick_tx);

/* Called on Core-1 TinyUSB task. Returns true if a report was transmitted,
 * false if endpoint was busy (caller should keep dirty bit) or nothing to send. */
bool usb_modes_extra_flush_tx(usb_gamepad_mode_t mode,
                              const usb_canonical_input_t *in,
                              bool input_dirty,
                              bool *out_sent_input);

bool usb_modes_extra_has_pending_tx(usb_gamepad_mode_t mode);
int64_t usb_modes_extra_keepalive_us(usb_gamepad_mode_t mode);

#endif /* USB_MODES_EXTRA_H */
