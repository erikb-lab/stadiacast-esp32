/* Phase 1: Wi-Fi SoftAP (STADIA_ESP32 or custom NVS SSID) for the Stadia controller. */
#include "wifi_ap.h"
#include "ble_provision.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"

#define TAG "AP"
#define NVS_NS_AP       "stadiacast"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_TX_QDBM "wifi_tx"

static char   s_ap_ssid[33] = WIFI_AP_DEFAULT_SSID;
static char   s_ap_pass[64] = WIFI_AP_DEFAULT_PASS;
static int8_t s_ap_tx_qdbm  = WIFI_AP_DEFAULT_TX_QDBM;

static void wifi_ap_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = data;
        ble_provision_pause_for_wifi_ms(20000); /* Give joining controller 100% exclusive 2.4 GHz airtime */
        ESP_LOGI(TAG, "STA connected: MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "STA disconnected: MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = data;
        char ip_str[32];
        esp_ip4addr_ntoa(&e->ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "STA IP assigned: %s", ip_str);
    }
}

static void load_ap_config_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AP, NVS_READONLY, &h) != ESP_OK) return;

    char tmp_ssid[33] = {0};
    size_t len = sizeof(tmp_ssid);
    if (nvs_get_str(h, NVS_KEY_SSID, tmp_ssid, &len) == ESP_OK && tmp_ssid[0] != '\0') {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", tmp_ssid);
    }

    char tmp_pass[64] = {0};
    len = sizeof(tmp_pass);
    if (nvs_get_str(h, NVS_KEY_PASS, tmp_pass, &len) == ESP_OK && strlen(tmp_pass) >= 8) {
        snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", tmp_pass);
    }

    int8_t tx = WIFI_AP_DEFAULT_TX_QDBM;
    if (nvs_get_i8(h, NVS_KEY_TX_QDBM, &tx) == ESP_OK && tx >= 32 && tx <= 80) {
        s_ap_tx_qdbm = tx;
    }

    nvs_close(h);
}

void wifi_ap_get_config(char *ssid, size_t ssid_len, int8_t *tx_qdbm) {
    if (ssid && ssid_len > 0) snprintf(ssid, ssid_len, "%s", s_ap_ssid);
    if (tx_qdbm) *tx_qdbm = s_ap_tx_qdbm;
}

void wifi_ap_get_pass(char *pass, size_t pass_len) {
    if (pass && pass_len > 0) snprintf(pass, pass_len, "%s", s_ap_pass);
}

bool wifi_ap_set_tx_power(int8_t tx_qdbm) {
    if (tx_qdbm < 32) tx_qdbm = 32; /* +8 dBm */
    if (tx_qdbm > 80) tx_qdbm = 80; /* +20 dBm */
    s_ap_tx_qdbm = tx_qdbm;
    (void)esp_wifi_set_max_tx_power(tx_qdbm);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_AP, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_i8(h, NVS_KEY_TX_QDBM, tx_qdbm);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool wifi_ap_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AP, NVS_READWRITE, &h) != ESP_OK) return false;

    esp_err_t err = ESP_OK;
    if (!ssid || ssid[0] == '\0') {
        /* Reset to defaults */
        (void)nvs_erase_key(h, NVS_KEY_SSID);
        (void)nvs_erase_key(h, NVS_KEY_PASS);
        err = nvs_commit(h);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", WIFI_AP_DEFAULT_SSID);
        snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", WIFI_AP_DEFAULT_PASS);
    } else {
        size_t slen = strlen(ssid);
        size_t plen = pass ? strlen(pass) : 0;
        if (slen == 0 || slen > 32 || (plen > 0 && (plen < 8 || plen > 63))) {
            nvs_close(h);
            return false;
        }
        err = nvs_set_str(h, NVS_KEY_SSID, ssid);
        if (err == ESP_OK && plen >= 8) {
            err = nvs_set_str(h, NVS_KEY_PASS, pass);
        }
        if (err == ESP_OK) err = nvs_commit(h);
        if (err == ESP_OK) {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
            if (plen >= 8) snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", pass);
        }
    }
    nvs_close(h);
    return err == ESP_OK;
}

void wifi_ap_init(void) {
    load_ap_config_from_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_ap_event_handler, NULL, NULL));

    wifi_config_t ap_config = {0};
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 6;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.pmf_cfg.capable = false;  /* CRITICAL: Stadia controller refuses PMF */
    ap_config.ap.pmf_cfg.required = false;

    size_t slen = strlen(s_ap_ssid);
    if (slen > sizeof(ap_config.ap.ssid)) slen = sizeof(ap_config.ap.ssid);
    memcpy(ap_config.ap.ssid, s_ap_ssid, slen);
    ap_config.ap.ssid_len = (uint8_t)slen;
    size_t plen = strlen(s_ap_pass);
    if (plen >= sizeof(ap_config.ap.password)) plen = sizeof(ap_config.ap.password) - 1;
    memcpy(ap_config.ap.password, s_ap_pass, plen);
    ap_config.ap.password[plen] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(s_ap_tx_qdbm));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "SoftAP up: SSID=%s, IP=192.168.4.1, HT20, +%d.%02ddBm, WPA2-PSK, no PMF",
             s_ap_ssid, s_ap_tx_qdbm / 4, (s_ap_tx_qdbm % 4) * 25);
}
