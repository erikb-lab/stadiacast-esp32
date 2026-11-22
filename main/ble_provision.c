/* On-Board NimBLE Stadia Controller Auto-Provisioner (Fast Pair 0xFE2C + Setup Service 0xFEA0).
 * Automatically scans while no controller is connected over Wi-Fi (0% radio impact during play),
 * reads Window 1a (gzipped RSA-2048 public key), encrypts SoftAP password with PKCS#1 v1.5,
 * and writes Wi-Fi credentials to CHAR_CONNECT so any Stadia Controller pairs with zero PC/phone app! */
#include "ble_provision.h"
#include "stadia_transport.h"
#include "wifi_ap.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "miniz.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

#define TAG "BLE-PROV"
#define NVS_NS_PROV       "stadiacast"
#define NVS_KEY_AUTO_PROV "ble_auto"

#define FAST_PAIR_UUID16  0xFE2Cu
#define SETUP_SVC_UUID16  0xFEA0u

/* 128-bit GATT UUIDs in NimBLE little-endian byte order */
static const ble_uuid128_t UUID_CHAR_CONNECT = BLE_UUID128_INIT(
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0xd0, 0x87,
    0xe5, 0x11, 0x2f, 0x00, 0x40, 0xfe, 0x28, 0x03);
static const ble_uuid128_t UUID_CHAR_STATE = BLE_UUID128_INIT(
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0xbb, 0x9f,
    0xe4, 0x11, 0xb2, 0xfa, 0x60, 0x61, 0x7c, 0x0c);
static const ble_uuid128_t UUID_WIN_CRYPTO_CTRL = BLE_UUID128_INIT(
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0xbb, 0x9f,
    0xe4, 0x11, 0xb2, 0xfa, 0x60, 0x61, 0x6c, 0x1a);
static const ble_uuid128_t UUID_WIN_CRYPTO_DATA = BLE_UUID128_INIT(
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0xbb, 0x9f,
    0xe4, 0x11, 0xb2, 0xfa, 0x60, 0x61, 0x7c, 0x1a);

#define STATE_CONNECTED   0x3Cu
#define STATE_FAIL_JSON1  0x34u
#define STATE_FAIL_JSON2  0x4Eu
#define STATE_FAIL_JOIN   0x4Fu

#define WINDOW_PAGE_SIZE  512
#define WINDOW_MAX_PAGES  24
#if CONFIG_SPIRAM
#define CRYPTO_RAW_CAP    16384
#define CRYPTO_JSON_CAP   32768
#else
#define CRYPTO_RAW_CAP    8192
#define CRYPTO_JSON_CAP   14336
#endif

static void *ble_alloc(size_t sz) {
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_status_str[96] = "Initializing BLE...";
static volatile bool s_auto_enabled = true;
static volatile bool s_manual_trigger = false;
static volatile bool s_ble_synced = false;
static volatile int64_t s_wifi_pause_until_us = 0;

static SemaphoreHandle_t s_op_sem = NULL;
static volatile bool     s_found_target = false;
static ble_addr_t        s_target_addr;
static volatile bool     s_connected = false;
static volatile bool     s_disconnected = false;
static uint16_t          s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t          s_mtu = 23;
static volatile int      s_op_status = 0;

/* Discovered service & characteristic handles */
static uint16_t s_svc_start = 0, s_svc_end = 0;
static uint16_t s_h_connect = 0;
static uint16_t s_h_state   = 0;
static uint16_t s_h_wctrl   = 0;
static uint16_t s_h_wdata   = 0;

/* Scratch buffer for long read callback */
static uint8_t s_read_buf[1024];
static size_t  s_read_len = 0;

static void set_status(const char *msg) {
    portENTER_CRITICAL(&s_stat_mux);
    snprintf(s_status_str, sizeof(s_status_str), "%s", msg);
    portEXIT_CRITICAL(&s_stat_mux);
    ESP_LOGI(TAG, "%s", msg);
}

void ble_provision_get_status(char *buf, size_t len) {
    if (!buf || len == 0) return;
    portENTER_CRITICAL(&s_stat_mux);
    snprintf(buf, len, "%s", s_status_str);
    portEXIT_CRITICAL(&s_stat_mux);
}

bool ble_provision_get_auto(void) {
    return s_auto_enabled;
}

void ble_provision_set_auto(bool enabled) {
    s_auto_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_PROV, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, NVS_KEY_AUTO_PROV, enabled ? 1 : 0) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    if (!enabled) set_status("Auto-provisioning disabled");
}

void ble_provision_trigger_now(void) {
    s_wifi_pause_until_us = 0;
    s_manual_trigger = true;
}

void ble_provision_pause_for_wifi_ms(uint32_t ms) {
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000LL;
    if (until > s_wifi_pause_until_us) s_wifi_pause_until_us = until;
    if (s_ble_synced) {
        (void)ble_gap_disc_cancel();
    }
}

static bool str_contains_ci(const uint8_t *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

static bool adv_is_stadia_setup(const uint8_t *data, uint8_t len) {
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = data[i];
        if (flen == 0 || i + 1 + flen > len) break;
        uint8_t ftype = data[i + 1];
        const uint8_t *val = data + i + 2;
        size_t vlen = (size_t)(flen - 1);

        /* Service Data - 16-bit UUID: Fast Pair 0xFE2C (LE: 0x2C, 0xFE) */
        if (ftype == BLE_HS_ADV_TYPE_SVC_DATA_UUID16 && vlen >= 2) {
            uint16_t uuid16 = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
            if (uuid16 == FAST_PAIR_UUID16) return true;
        }
        /* Complete or shortened local name containing "stadia" */
        if ((ftype == BLE_HS_ADV_TYPE_COMP_NAME || ftype == BLE_HS_ADV_TYPE_INCOMP_NAME) &&
            str_contains_ci(val, vlen, "stadia")) {
            return true;
        }
        i += 1 + flen;
    }
    return false;
}

static int gap_event_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s_found_target && adv_is_stadia_setup(ev->disc.data, ev->disc.length_data)) {
            s_target_addr = ev->disc.addr;
            s_found_target = true;
            ble_gap_disc_cancel();
            if (s_op_sem) xSemaphoreGive(s_op_sem);
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_op_sem) xSemaphoreGive(s_op_sem);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn_handle = ev->connect.conn_handle;
            s_connected = true;
            s_disconnected = false;
            s_op_status = 0;
        } else {
            s_connected = false;
            s_op_status = ev->connect.status;
        }
        if (s_op_sem) xSemaphoreGive(s_op_sem);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_disconnected = true;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_op_sem) xSemaphoreGive(s_op_sem);
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        break;
    default:
        break;
    }
    return 0;
}

static int on_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                     uint16_t mtu, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && mtu > 23) s_mtu = mtu;
    s_op_status = error->status;
    if (s_op_sem) xSemaphoreGive(s_op_sem);
    return 0;
}

static int on_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          const struct ble_gatt_svc *service, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && service) {
        s_svc_start = service->start_handle;
        s_svc_end   = service->end_handle;
    } else {
        s_op_status = (error->status == BLE_HS_EDONE) ? 0 : error->status;
        if (s_op_sem) xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_CHAR_CONNECT.u) == 0) {
            s_h_connect = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_CHAR_STATE.u) == 0) {
            s_h_state = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_WIN_CRYPTO_CTRL.u) == 0) {
            s_h_wctrl = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_WIN_CRYPTO_DATA.u) == 0) {
            s_h_wdata = chr->val_handle;
        }
    } else {
        s_op_status = (error->status == BLE_HS_EDONE) ? 0 : error->status;
        if (s_op_sem) xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)attr; (void)arg;
    s_op_status = error->status;
    if (s_op_sem) xSemaphoreGive(s_op_sem);
    return 0;
}

static int on_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && attr && attr->om) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(s_read_buf)) len = sizeof(s_read_buf);
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(attr->om, s_read_buf, len, &copied);
        s_read_len = copied;
    }
    s_op_status = error->status;
    if (s_op_sem) xSemaphoreGive(s_op_sem);
    return 0;
}

static int on_read_long_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle; (void)arg;
    if (error->status == 0 && attr && attr->om) {
        uint16_t clen = OS_MBUF_PKTLEN(attr->om);
        if (s_read_len + clen > sizeof(s_read_buf)) {
            clen = (uint16_t)(sizeof(s_read_buf) - s_read_len);
        }
        if (clen > 0) {
            uint16_t copied = 0;
            ble_hs_mbuf_to_flat(attr->om, s_read_buf + s_read_len, clen, &copied);
            s_read_len += copied;
        }
        return 0; /* Wait for more chunks until BLE_HS_EDONE */
    }
    s_op_status = (error->status == BLE_HS_EDONE) ? 0 : error->status;
    if (s_op_sem) xSemaphoreGive(s_op_sem);
    return 0;
}

static bool wait_op(uint32_t timeout_ms) {
    return xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* Strip RFC 1952 gzip header and inflate raw deflate via ESP32-S3 ROM miniz */
static size_t gunzip_to_buf(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    if (in_len < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) {
        /* Not gzip: pass through raw */
        size_t cpy = in_len < out_cap ? in_len : out_cap;
        memcpy(out, in, cpy);
        return cpy;
    }
    uint8_t flg = in[3];
    size_t off = 10;
    if (flg & 0x04) { /* FEXTRA */
        if (off + 2 > in_len) return 0;
        uint16_t xlen = (uint16_t)in[off] | ((uint16_t)in[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) { /* FNAME */
        while (off < in_len && in[off] != 0) off++;
        off++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (off < in_len && in[off] != 0) off++;
        off++;
    }
    if (flg & 0x02) off += 2; /* FHCRC */
    if (off + 8 >= in_len) return 0;

    size_t deflate_len = in_len - off - 8; /* Exclude 8-byte CRC32+ISIZE trailer */
    /* Allocate 11 KB tinfl_decompressor on heap (PSRAM when available) so we never overflow the task stack */
    tinfl_decompressor *decomp = ble_alloc(sizeof(tinfl_decompressor));
    if (!decomp) return 0;
    tinfl_init(decomp);
    size_t in_bytes = deflate_len;
    size_t out_bytes = out_cap;
    tinfl_status status = tinfl_decompress(decomp,
                                           (const mz_uint8 *)(in + off), &in_bytes,
                                           (mz_uint8 *)out, (mz_uint8 *)out, &out_bytes,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);
    return (status == TINFL_STATUS_DONE) ? out_bytes : 0;
}

static int rng_cb(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static bool encrypt_pass_from_bundle(const char *json_str, const char *pass,
                                     char *enc_b64_out, size_t enc_b64_cap) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Crypto bundle JSON parse failed");
        return false;
    }
    cJSON *di = cJSON_GetObjectItem(root, "device_info");
    cJSON *pk_item = di ? cJSON_GetObjectItem(di, "public_key") : NULL;
    if (!pk_item || !cJSON_IsString(pk_item)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "device_info.public_key missing");
        return false;
    }

    const char *pk_b64 = pk_item->valuestring;
    size_t der_len = 0;
    uint8_t *der = ble_alloc(1024);
    uint8_t *ct  = ble_alloc(512);
    if (!der || !ct) {
        free(der);
        free(ct);
        cJSON_Delete(root);
        return false;
    }
    int rc = mbedtls_base64_decode(der, 1024, &der_len,
                                   (const unsigned char *)pk_b64, strlen(pk_b64));
    cJSON_Delete(root);
    if (rc != 0 || der_len == 0) {
        free(der);
        free(ct);
        ESP_LOGE(TAG, "public_key base64 decode failed: %d", rc);
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    rc = mbedtls_pk_parse_public_key(&pk, der, der_len);
    free(der);
    if (rc != 0) {
        free(ct);
        mbedtls_pk_free(&pk);
        ESP_LOGE(TAG, "pk_parse_public_key failed: -0x%04x", -rc);
        return false;
    }

    size_t ct_len = 0;
    rc = mbedtls_pk_encrypt(&pk, (const unsigned char *)pass, strlen(pass),
                            ct, &ct_len, 512, rng_cb, NULL);
    mbedtls_pk_free(&pk);
    if (rc != 0 || ct_len == 0) {
        free(ct);
        ESP_LOGE(TAG, "RSA PKCS#1 v1.5 encrypt failed: -0x%04x", -rc);
        return false;
    }

    size_t out_len = 0;
    rc = mbedtls_base64_encode((unsigned char *)enc_b64_out, enc_b64_cap, &out_len, ct, ct_len);
    free(ct);
    if (rc != 0) return false;
    enc_b64_out[out_len] = '\0';
    return true;
}

static bool provision_target(void) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Connecting to Stadia controller (%02x:%02x:%02x:%02x:%02x:%02x)...",
             s_target_addr.val[5], s_target_addr.val[4], s_target_addr.val[3],
             s_target_addr.val[2], s_target_addr.val[1], s_target_addr.val[0]);
    set_status(msg);

    s_connected = false;
    s_disconnected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = 23;
    s_svc_start = s_svc_end = 0;
    s_h_connect = s_h_state = s_h_wctrl = s_h_wdata = 0;

    xSemaphoreTake(s_op_sem, 0);
    if (ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_target_addr, 15000, NULL, gap_event_cb, NULL) != 0 ||
        !wait_op(16000) || !s_connected) {
        set_status("BLE connect failed (will retry)");
        return false;
    }

    bool ok = false;
    uint8_t *raw_buf = ble_alloc(CRYPTO_RAW_CAP);
    uint8_t *json_buf = ble_alloc(CRYPTO_JSON_CAP);
    if (!raw_buf || !json_buf) {
        set_status("BLE provision OOM");
        goto cleanup;
    }

    /* 1. Negotiate larger MTU (up to 512) */
    xSemaphoreTake(s_op_sem, 0);
    if (ble_gattc_exchange_mtu(s_conn_handle, on_mtu_cb, NULL) == 0) {
        (void)wait_op(5000);
    }

    /* 2. Discover setup service 0xFEA0 */
    set_status("Discovering Stadia setup service...");
    xSemaphoreTake(s_op_sem, 0);
    if (ble_gattc_disc_svc_by_uuid(s_conn_handle, BLE_UUID16_DECLARE(SETUP_SVC_UUID16),
                                   on_disc_svc_cb, NULL) != 0 ||
        !wait_op(8000) || s_svc_start == 0) {
        set_status("Setup service 0xFEA0 not found");
        goto cleanup;
    }

    /* 3. Discover characteristics in 0xFEA0 */
    xSemaphoreTake(s_op_sem, 0);
    if (ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end,
                                on_disc_chr_cb, NULL) != 0 ||
        !wait_op(8000) || !s_h_connect || !s_h_wctrl || !s_h_wdata) {
        set_status("Missing setup characteristics");
        goto cleanup;
    }

    /* 4. Read Window 1a (gzipped crypto bundle containing RSA-2048 public key) */
    size_t raw_len = 0;
    for (int page = 0; page < WINDOW_MAX_PAGES; page++) {
        if (!s_connected) {
            set_status("Controller dropped BLE during key read");
            goto cleanup;
        }
        snprintf(msg, sizeof(msg), "Reading controller RSA key (page %d)...", page + 1);
        set_status(msg);

        uint8_t idx[2] = { (uint8_t)(page & 0xFF), (uint8_t)(page >> 8) };
        xSemaphoreTake(s_op_sem, 0);
        if (ble_gattc_write_flat(s_conn_handle, s_h_wctrl, idx, 2, on_write_cb, NULL) != 0 ||
            !wait_op(5000) || s_op_status != 0) {
            set_status("Window control write failed");
            goto cleanup;
        }

        vTaskDelay(pdMS_TO_TICKS(400));

        s_read_len = 0;
        xSemaphoreTake(s_op_sem, 0);
        if (ble_gattc_read_long(s_conn_handle, s_h_wdata, 0, on_read_long_cb, NULL) != 0 ||
            !wait_op(8000) || s_op_status != 0) {
            set_status("Window data read failed");
            goto cleanup;
        }
        if (s_read_len == 0) break;
        if (raw_len + s_read_len > CRYPTO_RAW_CAP) {
            set_status("Crypto bundle too large");
            goto cleanup;
        }
        memcpy(raw_buf + raw_len, s_read_buf, s_read_len);
        raw_len += s_read_len;
        if (s_read_len < WINDOW_PAGE_SIZE) break;
    }

    size_t json_len = gunzip_to_buf(raw_buf, raw_len, json_buf, CRYPTO_JSON_CAP - 1);
    if (json_len == 0) {
        set_status("Crypto bundle gunzip failed");
        goto cleanup;
    }
    json_buf[json_len] = '\0';

    /* 5. Encrypt our current SoftAP password under controller's RSA public key */
    char ap_ssid[33] = {0}, ap_pass[64] = {0};
    wifi_ap_get_config(ap_ssid, sizeof(ap_ssid), NULL);
    wifi_ap_get_pass(ap_pass, sizeof(ap_pass));

    set_status("Encrypting SoftAP credentials...");
    char enc_b64[600];
    if (!encrypt_pass_from_bundle((const char *)json_buf, ap_pass, enc_b64, sizeof(enc_b64))) {
        set_status("RSA encryption failed");
        goto cleanup;
    }

    /* 6. Build JSON connect payload & write to CHAR_CONNECT */
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "ssid", ap_ssid);
    cJSON_AddNumberToObject(p, "wpa_auth", 7);   /* WPA2-PSK */
    cJSON_AddNumberToObject(p, "wpa_cipher", 4); /* CCMP/AES */
    cJSON_AddStringToObject(p, "enc_passwd", enc_b64);
    char *payload = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!payload) goto cleanup;

    snprintf(msg, sizeof(msg), "Sending Wi-Fi credentials (%s)...", ap_ssid);
    set_status(msg);

    size_t plen = strlen(payload);
    xSemaphoreTake(s_op_sem, 0);
    int wrc;
    if (plen <= (size_t)(s_mtu > 3 ? s_mtu - 3 : 20)) {
        wrc = ble_gattc_write_flat(s_conn_handle, s_h_connect, payload, (uint16_t)plen, on_write_cb, NULL);
    } else {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, (uint16_t)plen);
        wrc = om ? ble_gattc_write_long(s_conn_handle, s_h_connect, 0, om, on_write_cb, NULL) : -1;
    }
    free(payload);

    if (wrc != 0 || !wait_op(10000) || s_op_status != 0) {
        set_status("Credentials write failed");
        goto cleanup;
    }

    /* 7. Wait up to 20s for controller to join SoftAP (drops BLE link or reports STATE 0x3C) */
    set_status("Credentials sent! Waiting for controller to join...");
    for (int sec = 0; sec < 20; sec++) {
        if (s_disconnected || stadia_transport_is_ready()) {
            ok = true;
            break;
        }
        if (s_h_state && s_connected) {
            s_read_len = 0;
            xSemaphoreTake(s_op_sem, 0);
            if (ble_gattc_read(s_conn_handle, s_h_state, on_read_cb, NULL) == 0 &&
                wait_op(2000) && s_op_status == 0 && s_read_len >= 1) {
                uint8_t st = s_read_buf[s_read_len - 1];
                ESP_LOGI(TAG, "Controller setup state: 0x%02x", st);
                if (st == STATE_CONNECTED) { ok = true; break; }
                if (st == STATE_FAIL_JSON1 || st == STATE_FAIL_JSON2 || st == STATE_FAIL_JOIN) {
                    set_status("Controller rejected credentials (state error)");
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (ok) {
        snprintf(msg, sizeof(msg), "Provisioned! Joining %s (power-cycle controller if blinking white)", ap_ssid);
        set_status(msg);
    } else {
        set_status("Timed out waiting for controller join");
    }

cleanup:
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    free(raw_buf);
    free(json_buf);
    return ok;
}

static void ble_prov_worker_task(void *arg) {
    (void)arg;
    while (!s_ble_synced) vTaskDelay(pdMS_TO_TICKS(100));

    /* Give an already-provisioned controller 6s on boot to connect over Wi-Fi first */
    set_status("Ready (waiting for controller)");
    vTaskDelay(pdMS_TO_TICKS(6000));

    for (;;) {
        bool manual = s_manual_trigger;
        s_manual_trigger = false;

        if (stadia_transport_is_ready() && !manual) {
            set_status("Idle (controller connected)");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!manual && esp_timer_get_time() < s_wifi_pause_until_us) {
            set_status("Paused (Wi-Fi client connecting)");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_auto_enabled && !manual) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Passive scan for 5s looking for Fast Pair 0xFE2C / Stadia Setup */
        s_found_target = false;
        struct ble_gap_disc_params dp = {0};
        dp.passive = 1;
        dp.itvl = 0x0060;   /* 60 ms interval (low duty cycle for Wi-Fi coexist) */
        dp.window = 0x0020; /* 20 ms window */
        dp.filter_duplicates = 1;

        set_status(manual ? "Scanning for blinking-orange Stadia controller..."
                          : "Auto-scanning for setup-mode controller...");
        xSemaphoreTake(s_op_sem, 0);
        int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &dp, gap_event_cb, NULL);
        if (rc == 0) {
            (void)wait_op(6000);
            (void)ble_gap_disc_cancel();
        }

        if (s_found_target) {
            bool ok = provision_target();
            /* Cooldown so controller can finish post-provisioning Wi-Fi double-boot */
            vTaskDelay(pdMS_TO_TICKS(ok ? 25000 : 5000));
        } else {
            set_status(s_auto_enabled ? "Waiting (hold Y+Stadia or reset controller for orange blink)"
                                      : "No setup-mode controller found");
            vTaskDelay(pdMS_TO_TICKS(6000));
        }
    }
}

static void nimble_on_sync(void) {
    s_ble_synced = true;
}

static void nimble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_provision_init(void) {
    nvs_handle_t h;
    uint8_t val = 1;
    if (nvs_open(NVS_NS_PROV, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, NVS_KEY_AUTO_PROV, &val);
        nvs_close(h);
    }
    s_auto_enabled = (val != 0);

    s_op_sem = xSemaphoreCreateBinary();
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        set_status("BLE init failed");
        return;
    }

    ble_hs_cfg.sync_cb = nimble_on_sync;
    nimble_port_freertos_init(nimble_host_task);
    xTaskCreatePinnedToCore(ble_prov_worker_task, "ble_prov", 8192, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "On-board BLE Auto-Provisioner ready (auto=%d)", (int)s_auto_enabled);
}
