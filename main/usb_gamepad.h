/* Phase 4/5: Wired Google Stadia Controller (18d1:9400) USB HID + Rumble (Report 5). */
#ifndef USB_GAMEPAD_H
#define USB_GAMEPAD_H

#include <stdint.h>

typedef void (*usb_rumble_cb_t)(uint16_t left_strong, uint16_t right_weak);

void usb_gamepad_init(usb_rumble_cb_t rumble_cb);
void usb_gamepad_send(uint32_t stadia_buttons,
                      int32_t lx, int32_t ly,
                      int32_t rx, int32_t ry,
                      uint32_t lt, uint32_t rt);
void usb_gamepad_reset(void);
void usb_gamepad_keepalive_tick(void);

#endif /* USB_GAMEPAD_H */
