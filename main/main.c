#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "stadia_signaling.h"

extern void wifi_ap_init(void);
extern void net_services_init(void);
extern void https_boot_init(void);
extern void stadia_transport_init(void);
extern void stadia_transport_set_rumble(uint16_t left_strong, uint16_t right_weak);
extern void usb_gamepad_init(void (*rumble_cb)(uint16_t, uint16_t));
extern stadia_session_t stadia_sess;

#define TAG "STADIACAST"

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    stadia_signaling_init(&stadia_sess);
    wifi_ap_init();
    net_services_init();
    https_boot_init();
    usb_gamepad_init(stadia_transport_set_rumble);
    stadia_transport_init();

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