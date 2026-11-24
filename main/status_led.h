/* Configurable On-Board Status LED (Plain GPIO Active-Low/High or WS2812 RGB via RMT) */
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STATUS_LED_TYPE_GPIO_ACTIVE_LOW  = 0, /* Default: Seeed XIAO ESP32-S3 orange LED (GPIO 21) */
    STATUS_LED_TYPE_GPIO_ACTIVE_HIGH = 1, /* Generic active-HIGH plain LED */
    STATUS_LED_TYPE_WS2812           = 2, /* WS2812/SK6812 RGB LED (SuperMini/DevKitC GPIO 48, S3-Zero GPIO 21) */
    STATUS_LED_TYPE_DISABLED         = 3, /* LED disabled */
    STATUS_LED_TYPE_COUNT            = 4,
} status_led_type_t;

void status_led_init(void);
void status_led_update(int64_t now_us, bool ready, bool mic_muted, bool low_batt, bool combo_held);
bool status_led_configure(status_led_type_t type, int8_t gpio);
void status_led_get_config(status_led_type_t *type, int8_t *gpio);

#endif /* STATUS_LED_H */
