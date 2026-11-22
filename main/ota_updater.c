/* One-shot Home Wi-Fi / LAN Auto-OTA Updater + Clean Reboot / Software Bootloader */
#include "ota_updater.h"
#include "usb_gamepad.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "soc/rtc_cntl_reg.h"

#define TAG "OTA"

#define NVS_NS_OTA          "stadiacast"
#define NVS_KEY_OTA_RUN     "ota_run"
#define NVS_KEY_SERIAL_BOOT "serial_boot"
#define NVS_KEY_STA_SSID    "sta_ssid"
#define NVS_KEY_STA_PASS    "sta_pass"
#define NVS_KEY_OTA_URL     "ota_url"
#define NVS_KEY_OTA_MSG     "ota_msg"

#define STATUS_LED_GPIO  GPIO_NUM_21
#define WIFI_GOT_IP_BIT  BIT0
#define WIFI_FAIL_BIT    BIT1

static EventGroupHandle_t s_sta_ev = NULL;
static int s_sta_retries = 0;
static volatile bool s_led_blink_active = false;

static void ota_led_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    int lvl = 0;
    while (s_led_blink_active) {
        gpio_set_level(STATUS_LED_GPIO, lvl);
        lvl = !lvl;
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    gpio_set_level(STATUS_LED_GPIO, 1); /* OFF */
    vTaskDelete(NULL);
}

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_retries < 5) {
            s_sta_retries++;
            esp_wifi_connect();
        } else if (s_sta_ev) {
            xEventGroupSetBits(s_sta_ev, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_sta_ev) xEventGroupSetBits(s_sta_ev, WIFI_GOT_IP_BIT);
    }
}

void ota_updater_set_last_msg(const char *msg) {
    if (!msg) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, NVS_KEY_OTA_MSG, msg) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

void ota_updater_get_last_msg(char *msg, size_t msg_len) {
    if (!msg || msg_len == 0) return;
    msg[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READONLY, &h) == ESP_OK) {
        size_t len = msg_len;
        if (nvs_get_str(h, NVS_KEY_OTA_MSG, msg, &len) != ESP_OK) msg[0] = '\0';
        nvs_close(h);
    }
}

void ota_updater_get_config(char *sta_ssid, size_t ssid_len, char *ota_url, size_t url_len) {
    if (sta_ssid && ssid_len > 0) sta_ssid[0] = '\0';
    if (ota_url && url_len > 0) ota_url[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READONLY, &h) != ESP_OK) return;
    if (sta_ssid && ssid_len > 0) {
        size_t len = ssid_len;
        if (nvs_get_str(h, NVS_KEY_STA_SSID, sta_ssid, &len) != ESP_OK) sta_ssid[0] = '\0';
    }
    if (ota_url && url_len > 0) {
        size_t len = url_len;
        if (nvs_get_str(h, NVS_KEY_OTA_URL, ota_url, &len) != ESP_OK) ota_url[0] = '\0';
    }
    nvs_close(h);
}

bool ota_updater_save_config(const char *sta_ssid, const char *sta_pass, const char *ota_url) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = ESP_OK;

    if (sta_ssid) {
        if (sta_ssid[0] == '\0') {
            (void)nvs_erase_key(h, NVS_KEY_STA_SSID);
            (void)nvs_erase_key(h, NVS_KEY_STA_PASS);
        } else if (strlen(sta_ssid) <= 32) {
            err = nvs_set_str(h, NVS_KEY_STA_SSID, sta_ssid);
            if (err == ESP_OK && sta_pass && sta_pass[0] != '\0' && strlen(sta_pass) <= 63) {
                err = nvs_set_str(h, NVS_KEY_STA_PASS, sta_pass);
            }
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    if (err == ESP_OK && ota_url) {
        if (ota_url[0] == '\0') {
            (void)nvs_erase_key(h, NVS_KEY_OTA_URL);
        } else if (strlen(ota_url) < 192 &&
                   (strncmp(ota_url, "http://", 7) == 0 || strncmp(ota_url, "https://", 8) == 0)) {
            err = nvs_set_str(h, NVS_KEY_OTA_URL, ota_url);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool ota_updater_schedule_sta_ota(void) {
    char ssid[33] = {0};
    ota_updater_get_config(ssid, sizeof(ssid), NULL, 0);
    if (ssid[0] == '\0') return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, NVS_KEY_OTA_RUN, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool ota_updater_consume_serial_boot(void) {
    nvs_handle_t h;
    uint8_t flag = 0;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) return false;
    (void)nvs_get_u8(h, NVS_KEY_SERIAL_BOOT, &flag);
    if (flag != 0) {
        (void)nvs_set_u8(h, NVS_KEY_SERIAL_BOOT, 0);
        (void)nvs_commit(h);
    }
    nvs_close(h);
    return flag != 0;
}

void ota_updater_reboot_clean(bool to_bootloader) {
    ESP_LOGI(TAG, "Clean reboot requested (serial_flash=%d)", (int)to_bootloader);
    if (to_bootloader) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_set_u8(h, NVS_KEY_SERIAL_BOOT, 1);
            (void)nvs_commit(h);
            nvs_close(h);
        }
    }
    (void)esp_wifi_deauth_sta(0);
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)esp_wifi_stop();
    if (to_bootloader) {
        usb_gamepad_uninstall(); /* Release USB-OTG PHY so USB-Serial/JTAG re-enumerates cleanly */
    } else {
        usb_gamepad_disconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void ota_updater_check_and_run(void) {
    nvs_handle_t h;
    uint8_t run_flag = 0;
    if (nvs_open(NVS_NS_OTA, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_get_u8(h, NVS_KEY_OTA_RUN, &run_flag);
    if (run_flag == 0) {
        nvs_close(h);
        return;
    }

    /* Immediately clear one-shot flag so power loss or failure always boots back to normal mode */
    (void)nvs_set_u8(h, NVS_KEY_OTA_RUN, 0);
    (void)nvs_commit(h);

    char ssid[33] = {0};
    char pass[65] = {0};
    char url[192] = {0};
    size_t len = sizeof(ssid);
    (void)nvs_get_str(h, NVS_KEY_STA_SSID, ssid, &len);
    len = sizeof(pass);
    (void)nvs_get_str(h, NVS_KEY_STA_PASS, pass, &len);
    len = sizeof(url);
    (void)nvs_get_str(h, NVS_KEY_OTA_URL, url, &len);
    nvs_close(h);

    if (ssid[0] == '\0') {
        ota_updater_set_last_msg("STA OTA skipped: no Home Wi-Fi configured");
        return;
    }
    bool custom_url = (url[0] != '\0');
    const char *target_url = custom_url ? url : OTA_DEFAULT_GITHUB_URL;

    ESP_LOGI(TAG, "=== One-Shot STA Auto-OTA: SSID=%s URL=%s ===", ssid, target_url);
    s_led_blink_active = true;
    xTaskCreate(ota_led_task, "ota_led", 2048, NULL, 2, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    s_sta_ev = xEventGroupCreate();
    s_sta_retries = 0;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, NULL));

    wifi_config_t sta_cfg = {0};
    size_t slen = strlen(ssid);
    if (slen > sizeof(sta_cfg.sta.ssid)) slen = sizeof(sta_cfg.sta.ssid);
    memcpy(sta_cfg.sta.ssid, ssid, slen);
    size_t plen = strlen(pass);
    if (plen >= sizeof(sta_cfg.sta.password)) plen = sizeof(sta_cfg.sta.password) - 1;
    memcpy(sta_cfg.sta.password, pass, plen);
    sta_cfg.sta.password[plen] = '\0';
    sta_cfg.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_sta_ev, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_GOT_IP_BIT)) {
        ESP_LOGE(TAG, "Home Wi-Fi connection failed");
        ota_updater_set_last_msg("Auto-OTA failed: could not connect to Home Wi-Fi");
        s_led_blink_active = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    esp_http_client_config_t http_cfg = {
        .url = target_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .max_redirection_count = 6,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Auto-OTA begin failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        ota_updater_set_last_msg(msg);
        s_led_blink_active = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    esp_app_desc_t new_desc;
    const esp_app_desc_t *cur_desc = esp_app_get_description();
    err = esp_https_ota_get_img_desc(ota_handle, &new_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid firmware header (%s)", esp_err_to_name(err));
        ota_updater_set_last_msg("Auto-OTA failed: invalid app image header");
        esp_https_ota_abort(ota_handle);
        s_led_blink_active = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    if (!custom_url && strcmp(new_desc.version, cur_desc->version) == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Already up to date (v%s)", cur_desc->version);
        ESP_LOGI(TAG, "%s", msg);
        ota_updater_set_last_msg(msg);
        esp_https_ota_abort(ota_handle);
        s_led_blink_active = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    ESP_LOGI(TAG, "Downloading v%s (current v%s)...", new_desc.version, cur_desc->version);
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle)) {
        err = esp_https_ota_finish(ota_handle);
        if (err == ESP_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Updated to v%s via Home Wi-Fi!", new_desc.version);
            ESP_LOGI(TAG, "%s", msg);
            ota_updater_set_last_msg(msg);
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "Auto-OTA finish failed (%s)", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", msg);
            ota_updater_set_last_msg(msg);
        }
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "Auto-OTA download failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        ota_updater_set_last_msg(msg);
        esp_https_ota_abort(ota_handle);
    }

    s_led_blink_active = false;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}
