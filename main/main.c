#include <stdio.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_private/usb_phy.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "ble_provision.h"
#include "ota_updater.h"
#include "stadia_signaling.h"
#include "stadia_transport.h"
#include "usb_gamepad.h"
#include "wifi_ap.h"

extern void net_services_init(void);
extern void https_boot_init(void);
extern stadia_session_t stadia_sess;

#define TAG "STADIACAST"
#define STATUS_LED_GPIO GPIO_NUM_21

/* XIAO ESP32-S3 D0 = GPIO1: short to GND at power-on for Serial/Flash mode */
static bool boot_pin_is_serial_mode(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_1,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    esp_rom_delay_us(5000);
    return gpio_get_level(GPIO_NUM_1) == 0;
}

static void enter_serial_flash_mode(const char *reason) {
    /* Re-route internal USB PHY back to hardware USB-Serial/JTAG (303a:1001, /dev/ttyACM0) */
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_serial_jtag_ll_phy_enable_pad(true);
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target = USB_PHY_TARGET_INT,
    };
    usb_phy_handle_t phy_hdl = NULL;
    (void)usb_new_phy(&phy_cfg, &phy_hdl);

    gpio_config_t led_io = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_io);

    printf("\n=== %s -> USB SERIAL / FLASH MODE (ready for esptool or WebSerial) ===\n", reason);
    while (1) {
        /* Distinct double-flash pattern so Serial/Flash mode is obvious; replug exits cleanly */
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (boot_pin_is_serial_mode()) {
        enter_serial_flash_mode("D0 (GPIO1) grounded");
    }
    if (ota_updater_consume_serial_boot()) {
        enter_serial_flash_mode("Software USB Serial/Flash Mode");
    }

    /* One-shot Home Wi-Fi / LAN Auto-OTA if requested via controller combo or Web UI */
    ota_updater_check_and_run();

    stadia_signaling_init(&stadia_sess);
    wifi_ap_init();
    net_services_init();
    https_boot_init();
    usb_gamepad_init(stadia_transport_set_rumble);

    /* Confirm OTA image is healthy once Wi-Fi SoftAP, Web UI/OTA, and USB are up */
    (void)esp_ota_mark_app_valid_cancel_rollback();

    stadia_transport_init();
    ble_provision_init();

    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Boot OK. Internal DRAM free: %u bytes. PSRAM free: %u bytes.",
             (unsigned)int_free, (unsigned)psram_free);

    if (psram_free == 0) {
        ESP_LOGW(TAG, "PSRAM not detected! Check sdkconfig (OPI mode).");
    } else {
        ESP_LOGI(TAG, "OPI PSRAM OK.");
    }
}
