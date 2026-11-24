/* Configurable On-Board Status LED:
 *   - Plain GPIO Active-LOW  (default: Seeed XIAO ESP32-S3 GPIO 21)
 *   - Plain GPIO Active-HIGH (generic dev boards)
 *   - WS2812 RGB NeoPixel    (ESP32-S3 SuperMini / DevKitC GPIO 48, Waveshare S3-Zero GPIO 21, etc.)
 *   - Disabled
 */
#include "status_led.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "nvs.h"

#define TAG "STATUS_LED"

#define NVS_NS_LED       "stadiacast"
#define NVS_KEY_LED_TYPE "led_type"
#define NVS_KEY_LED_GPIO "led_gpio"

#define DEFAULT_LED_TYPE STATUS_LED_TYPE_GPIO_ACTIVE_LOW
#define DEFAULT_LED_GPIO 21

static status_led_type_t s_led_type = DEFAULT_LED_TYPE;
static int8_t            s_led_gpio = DEFAULT_LED_GPIO;

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_rmt_enc  = NULL;
static uint32_t             s_last_rgb = 0xFFFFFFFFu;
static int                  s_last_lvl = -1;
static portMUX_TYPE         s_led_mux  = portMUX_INITIALIZER_UNLOCKED;

static bool gpio_is_safe(int8_t pin) {
    if (pin < 0 || pin > 48) return false;
    /* Never touch native USB-OTG D-/D+ (19, 20) or SPI flash pins (26..32) */
    if (pin == 19 || pin == 20) return false;
    if (pin >= 26 && pin <= 32) return false;
#if CONFIG_SPIRAM_MODE_OCT
    /* OPI PSRAM uses GPIO 33..37 on S3R8 boards */
    if (pin >= 33 && pin <= 37) return false;
#endif
    return true;
}

static void ws2812_deinit(void) {
    if (s_rmt_chan) {
        (void)rmt_disable(s_rmt_chan);
        (void)rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
    }
    if (s_rmt_enc) {
        (void)rmt_del_encoder(s_rmt_enc);
        s_rmt_enc = NULL;
    }
}

static bool ws2812_init(int8_t pin) {
    ws2812_deinit();
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz -> 100 ns/tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_rmt_chan) != ESP_OK) {
        s_rmt_chan = NULL;
        return false;
    }
    rmt_bytes_encoder_config_t enc_cfg = {
        /* Standard WS2812 timing @ 10 MHz: T0H=0.3us/T0L=0.9us, T1H=0.9us/T1L=0.3us */
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc_cfg, &s_rmt_enc) != ESP_OK) {
        ws2812_deinit();
        return false;
    }
    if (rmt_enable(s_rmt_chan) != ESP_OK) {
        ws2812_deinit();
        return false;
    }
    return true;
}

static void ws2812_write_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_rmt_chan || !s_rmt_enc) return;
    /* WS2812 wire order is GRB */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tcfg = { .loop_count = 0 };
    if (rmt_transmit(s_rmt_chan, s_rmt_enc, grb, sizeof(grb), &tcfg) == ESP_OK) {
        (void)rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(10));
    }
}

static void apply_hardware_locked(status_led_type_t type, int8_t pin) {
    ws2812_deinit();
    s_last_rgb = 0xFFFFFFFFu;
    s_last_lvl = -1;

    if (type == STATUS_LED_TYPE_DISABLED || !gpio_is_safe(pin)) {
        return;
    }

    if (type == STATUS_LED_TYPE_WS2812) {
        if (ws2812_init(pin)) {
            ws2812_write_rgb(0, 0, 0);
            ESP_LOGI(TAG, "WS2812 RGB status LED ready on GPIO %d", (int)pin);
        } else {
            ESP_LOGW(TAG, "WS2812 RMT init failed on GPIO %d", (int)pin);
        }
    } else {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << (uint32_t)pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        int off_lvl = (type == STATUS_LED_TYPE_GPIO_ACTIVE_LOW) ? 1 : 0;
        gpio_set_level((gpio_num_t)pin, off_lvl);
        s_last_lvl = off_lvl;
        ESP_LOGI(TAG, "Plain GPIO status LED ready on GPIO %d (%s)",
                 (int)pin, type == STATUS_LED_TYPE_GPIO_ACTIVE_LOW ? "active-LOW" : "active-HIGH");
    }
}

void status_led_init(void) {
    status_led_type_t t = DEFAULT_LED_TYPE;
    int8_t g = DEFAULT_LED_GPIO;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_LED, NVS_READONLY, &h) == ESP_OK) {
        uint8_t nt = (uint8_t)t;
        int8_t  ng = g;
        if (nvs_get_u8(h, NVS_KEY_LED_TYPE, &nt) == ESP_OK && nt < (uint8_t)STATUS_LED_TYPE_COUNT) {
            t = (status_led_type_t)nt;
        }
        if (nvs_get_i8(h, NVS_KEY_LED_GPIO, &ng) == ESP_OK && gpio_is_safe(ng)) {
            g = ng;
        }
        nvs_close(h);
    }

    portENTER_CRITICAL(&s_led_mux);
    s_led_type = t;
    s_led_gpio = g;
    portEXIT_CRITICAL(&s_led_mux);

    apply_hardware_locked(t, g);
}

bool status_led_configure(status_led_type_t type, int8_t gpio) {
    if ((unsigned)type >= (unsigned)STATUS_LED_TYPE_COUNT) return false;
    if (type != STATUS_LED_TYPE_DISABLED && !gpio_is_safe(gpio)) return false;

    /* Turn off previous pin cleanly if switching pins */
    status_led_type_t old_type;
    int8_t old_gpio;
    portENTER_CRITICAL(&s_led_mux);
    old_type = s_led_type;
    old_gpio = s_led_gpio;
    s_led_type = STATUS_LED_TYPE_DISABLED; /* Pause updates during reconfigure */
    portEXIT_CRITICAL(&s_led_mux);

    if (old_type == STATUS_LED_TYPE_WS2812) {
        ws2812_write_rgb(0, 0, 0);
        ws2812_deinit();
    } else if ((old_type == STATUS_LED_TYPE_GPIO_ACTIVE_LOW || old_type == STATUS_LED_TYPE_GPIO_ACTIVE_HIGH) &&
               gpio_is_safe(old_gpio)) {
        gpio_set_level((gpio_num_t)old_gpio, old_type == STATUS_LED_TYPE_GPIO_ACTIVE_LOW ? 1 : 0);
    }

    apply_hardware_locked(type, gpio);

    portENTER_CRITICAL(&s_led_mux);
    s_led_type = type;
    s_led_gpio = gpio;
    portEXIT_CRITICAL(&s_led_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_LED, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, NVS_KEY_LED_TYPE, (uint8_t)type);
        (void)nvs_set_i8(h, NVS_KEY_LED_GPIO, gpio);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

void status_led_get_config(status_led_type_t *type, int8_t *gpio) {
    portENTER_CRITICAL(&s_led_mux);
    if (type) *type = s_led_type;
    if (gpio) *gpio = s_led_gpio;
    portEXIT_CRITICAL(&s_led_mux);
}

void status_led_update(int64_t now_us, bool ready, bool mic_muted, bool low_batt, bool combo_held) {
    status_led_type_t type;
    int8_t pin;
    portENTER_CRITICAL(&s_led_mux);
    type = s_led_type;
    pin  = s_led_gpio;
    portEXIT_CRITICAL(&s_led_mux);

    if (type == STATUS_LED_TYPE_DISABLED || !gpio_is_safe(pin)) return;

    bool on;
    uint8_t r = 0, g = 0, b = 0;

    if (combo_held) {
        on = ((now_us / 100000) & 1) == 0;
        if (on) { r = 24; g = 0; b = 32; } /* Fast purple blink */
    } else if (!ready) {
        on = (now_us % 1000000) < 120000;
        if (on) { r = 0; g = 8; b = 28; }  /* Heartbeat blue */
    } else if (mic_muted) {
        on = ((now_us / 500000) & 1) == 0;
        if (on) { r = 28; g = 14; b = 0; } /* Amber blink */
    } else if (low_batt) {
        int64_t t = now_us % 2000000;
        on = !((t < 120000) || (t >= 240000 && t < 360000));
        if (on) { r = 32; g = 0; b = 0; }  /* Red double-dip */
    } else {
        on = true;
        r = 0; g = 24; b = 0;              /* Solid green */
    }

    if (type == STATUS_LED_TYPE_WS2812) {
        uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        if (rgb != s_last_rgb) {
            ws2812_write_rgb(r, g, b);
            s_last_rgb = rgb;
        }
    } else {
        int level = (type == STATUS_LED_TYPE_GPIO_ACTIVE_LOW) ? (on ? 0 : 1) : (on ? 1 : 0);
        if (level != s_last_lvl) {
            gpio_set_level((gpio_num_t)pin, level);
            s_last_lvl = level;
        }
    }
}
