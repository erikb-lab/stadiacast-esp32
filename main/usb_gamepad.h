/* Wired Google Stadia Controller (18d1:9400) HID OR Xbox 360 XInput + UAC1 Audio. */
#ifndef USB_GAMEPAD_H
#define USB_GAMEPAD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_GAMEPAD_MODE_STADIA_HID = 0,
    USB_GAMEPAD_MODE_XINPUT     = 1,
} usb_gamepad_mode_t;

typedef void (*usb_rumble_cb_t)(uint16_t left_strong, uint16_t right_weak);

void               usb_gamepad_init(usb_rumble_cb_t rumble_cb);
usb_gamepad_mode_t usb_gamepad_get_mode(void);
bool               usb_gamepad_save_mode(usb_gamepad_mode_t mode);
void               usb_gamepad_disconnect(void);

void usb_gamepad_send(uint32_t stadia_buttons,
                      int32_t lx, int32_t ly,
                      int32_t rx, int32_t ry,
                      uint32_t lt, uint32_t rt);
void usb_gamepad_reset(void);
void usb_gamepad_keepalive_tick(void);

#endif /* USB_GAMEPAD_H */
