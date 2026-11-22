/* Phase 3-5: Display Link (UDP 50000) + Game Session Rumble (UDP 50002). */
#include "stadia_transport.h"
#include "ota_updater.h"
#include "usb_gamepad.h"
#include "usb_audio.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "controller_message.pb.h"
#include "input.pb.h"
#include "rpc_message.pb.h"
#include "webrtc_sdp.pb.h"

#define TAG "P3-RTC"

#define DISP_UDP_PORT             50000
#define GAME_UDP_PORT             50002
#define DISP_ICE_UFRAG            "stadia"
#define DISP_ICE_PWD              "stadiaesp32pwd"
#define GAME_ICE_UFRAG            "stadiag"
#define GAME_ICE_PWD              "stadiaesp32gamepwd"
#define GAME_KEY                  "stadia-game-1"

#define STUN_MAGIC                0x2112A442u
static const uint8_t STUN_MAGIC_BYTES[4] = { 0x21, 0x12, 0xA4, 0x42 };
#define STUN_TYPE_BINDING_REQUEST 0x0001u
#define STUN_TYPE_BINDING_SUCCESS 0x0101u
#define ATTR_USERNAME             0x0006u
#define ATTR_MESSAGE_INTEGRITY    0x0008u
#define ATTR_XOR_MAPPED_ADDRESS   0x0020u
#define ATTR_PRIORITY             0x0024u
#define ATTR_USE_CANDIDATE        0x0025u
#define ATTR_ICE_CONTROLLED       0x8029u
#define ATTR_FINGERPRINT          0x8028u

#define WIRE_HDR_LEN              8
#define WIRE_ID_RTP               0x0066u
#define WIRE_ID_RPC_CONTROLLER    0x0074u
#define WIRE_ID_RPC_DISPLAY       0x0075u
#define WIRE_ID_RPC_GAME          0xFFFAu
#define MIC_PAYLOAD_TYPE          111

#define RX_BUF_MAX                2048
#define TX_BUF_MAX                2048
#define TRANSPORT_STACK           20480

extern const char fullchain_pem_start[] asm("_binary_fullchain_pem_start");
extern const char fullchain_pem_end[]   asm("_binary_fullchain_pem_end");
extern const char privkey_pem_start[]   asm("_binary_privkey_pem_start");
extern const char privkey_pem_end[]     asm("_binary_privkey_pem_end");

#include "stadia_srtp.h"
#include "stadia_opus.h"

typedef struct {
    int64_t int_ms;
    int64_t fin_ms;
    int64_t start_ms;
    bool    active;
} dtls_timer_t;

typedef struct {
    const char *name;
    uint16_t    local_port;
    const char *our_ufrag;
    const char *our_pwd;
    int         sock;
    struct sockaddr_in peer;
    bool        peer_seen;
    char        ctrl_ufrag[64];
    char        ctrl_pwd[64];
    mbedtls_ssl_context ssl;
    dtls_timer_t timer;
    uint8_t     rx_buf[RX_BUF_MAX];
    size_t      rx_len;
    bool        dtls_seen;
    bool        handshake_done;
    int64_t     dtls_done_us;
    int64_t     last_stun_tx_us;
    bool        first_stun_logged;
    stadia_srtp_ctx_t srtp;
} dtls_link_t;

/* Shared mbedTLS config/cert/RNG */
static mbedtls_ssl_config g_conf;
static mbedtls_x509_crt g_srv_cert;
static mbedtls_pk_context g_srv_key;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_entropy_context g_entropy;
static char g_cert_fp[112];
static bool g_tls_shared_init;

static dtls_link_t g_disp = {
    .name = "DISP", .local_port = DISP_UDP_PORT,
    .our_ufrag = DISP_ICE_UFRAG, .our_pwd = DISP_ICE_PWD, .sock = -1
};
static dtls_link_t g_game = {
    .name = "GAME", .local_port = GAME_UDP_PORT,
    .our_ufrag = GAME_ICE_UFRAG, .our_pwd = GAME_ICE_PWD, .sock = -1
};

/* Task-owned scratch buffers (off stack) */
static uint8_t g_udp_buf[RX_BUF_MAX];
static uint8_t g_app_buf[RX_BUF_MAX];
static uint8_t g_wire_tx[WIRE_HDR_LEN + TX_BUF_MAX];
static uint8_t g_rpc_tx[TX_BUF_MAX];

/* Display link -> Game session state */
static volatile bool g_remote_updated;
static char g_pending_ufrag[64];
static char g_pending_pwd[64];
static struct sockaddr_in g_pending_cand;
static bool g_pending_cand_valid;

static bool g_rpc_ready;
static bool g_start_game_sent;
static int64_t g_last_state_req_us;
static uint32_t g_rpc_ticket;

/* Phase 6A: SRTP bidirectional audio state */
static bool     g_audio_enabled;
static int64_t  g_audio_next_tx_us;
static uint16_t g_audio_seq;
static uint32_t g_audio_ts;
static uint32_t g_audio_tx_pkts;
static uint32_t g_mic_rx_pkts;
static uint32_t g_mic_rx_bytes;
static uint8_t  g_audio_tx_buf[512];

/* Game session rumble state */
static volatile uint16_t g_rumble_left_req;
static volatile uint16_t g_rumble_right_req;
static volatile bool g_rumble_dirty;
static uint16_t g_rumble_left_sent;
static uint16_t g_rumble_right_sent;
static int64_t g_last_rumble_tx_us;
static uint64_t g_game_tx_seq;
static bool g_game_use_rtp;
static uint16_t g_game_rtp_seq;
static char g_game_session_id[64];
static uint32_t g_game_pad_id;

/* Last decoded input state */
static bool g_last_set;
static uint32_t g_last_buttons;
static int32_t g_last_lx, g_last_ly, g_last_rx, g_last_ry;
static uint32_t g_last_lt, g_last_rt;

/* Controller battery & headset telemetry */
static int  g_battery_pct = -1; /* -1 = waiting for first gauge sample */
static bool g_batt_warn_15 = false;
static bool g_batt_warn_5  = false;
static bool g_headset_plugged = false;
static void haptic_start(int pulses, uint16_t strength);

bool stadia_transport_is_ready(void) { return g_rpc_ready; }
int  stadia_transport_get_battery_pct(void) { return g_battery_pct; }
bool stadia_transport_is_headset_plugged(void) { return g_headset_plugged; }

void stadia_transport_wake(void);

void stadia_transport_set_remote(const char *ice_ufrag, const char *ice_pwd,
                                 const char *cand_ip, uint16_t cand_port) {
    if (ice_ufrag && ice_ufrag[0]) snprintf(g_pending_ufrag, sizeof(g_pending_ufrag), "%s", ice_ufrag);
    if (ice_pwd && ice_pwd[0])     snprintf(g_pending_pwd,   sizeof(g_pending_pwd),   "%s", ice_pwd);
    g_pending_cand_valid = false;
    if (cand_ip && cand_ip[0] && cand_port > 0) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(cand_port);
        if (inet_pton(AF_INET, cand_ip, &a.sin_addr) == 1) {
            g_pending_cand = a;
            g_pending_cand_valid = true;
        }
    }
    g_remote_updated = true;
    stadia_transport_wake();
}

static int g_wake_sock = -1;

void stadia_transport_wake(void) {
    if (g_wake_sock >= 0) {
        struct sockaddr_in lo = {0};
        lo.sin_family = AF_INET;
        lo.sin_port = htons(GAME_UDP_PORT);
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uint8_t b = 0xFF;
        sendto(g_wake_sock, &b, 1, 0, (const struct sockaddr *)&lo, sizeof(lo));
    }
}

void stadia_transport_set_rumble(uint16_t left_strong, uint16_t right_weak) {
    g_rumble_left_req = left_strong;
    g_rumble_right_req = right_weak;
    g_rumble_dirty = true;
    stadia_transport_wake();
}

/* ---------- Minimal Protobuf Wire Helpers (bypasses 128B nanopb wrapper caps) ---------- */
static size_t pbw_put_varint(uint8_t *out, uint64_t v) {
    size_t n = 0;
    while (v >= 0x80u) { out[n++] = (uint8_t)((v & 0x7Fu) | 0x80u); v >>= 7; }
    out[n++] = (uint8_t)v;
    return n;
}

static size_t pbw_put_bytes(uint8_t *out, size_t cap, uint32_t field_num,
                            const uint8_t *data, size_t len) {
    uint8_t hdr[16];
    size_t h = pbw_put_varint(hdr, ((uint64_t)field_num << 3) | 2u);
    h += pbw_put_varint(hdr + h, (uint64_t)len);
    if (h + len > cap) return 0;
    memcpy(out, hdr, h);
    if (len > 0) memcpy(out + h, data, len);
    return h + len;
}

static size_t pbw_put_u64(uint8_t *out, size_t cap, uint32_t field_num, uint64_t v) {
    uint8_t tmp[20];
    size_t n = pbw_put_varint(tmp, ((uint64_t)field_num << 3) | 0u);
    n += pbw_put_varint(tmp + n, v);
    if (n > cap) return 0;
    memcpy(out, tmp, n);
    return n;
}

static bool pbw_read_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *val) {
    uint64_t r = 0;
    int shift = 0;
    while (*off < len && shift < 64) {
        uint8_t b = buf[(*off)++];
        r |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) { *val = r; return true; }
        shift += 7;
    }
    return false;
}

/* Extract the first length-delimited field matching target_field from a raw protobuf message */
static bool pbw_find_bytes(const uint8_t *buf, size_t len, uint32_t target_field,
                           const uint8_t **out_ptr, size_t *out_len) {
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        if (!pbw_read_varint(buf, len, &off, &tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (wt == 0) {
            uint64_t dummy;
            if (!pbw_read_varint(buf, len, &off, &dummy)) return false;
        } else if (wt == 1) {
            if (off + 8 > len) return false;
            off += 8;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pbw_read_varint(buf, len, &off, &flen) || off + flen > len) return false;
            if (fn == target_field) {
                *out_ptr = buf + off;
                *out_len = (size_t)flen;
                return true;
            }
            off += (size_t)flen;
        } else if (wt == 5) {
            if (off + 4 > len) return false;
            off += 4;
        } else {
            return false;
        }
    }
    return false;
}

/* ---------- STUN helpers (RFC 5389) ---------- */
static uint32_t stun_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static bool stun_hmac_sha1(const uint8_t *key, size_t keylen,
                           const uint8_t *data, size_t len, uint8_t out[20]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md) return false;
    return mbedtls_md_hmac(md, key, keylen, data, len, out) == 0;
}

static void stun_pack_attr(uint8_t *out, uint16_t type, const uint8_t *val, size_t len) {
    out[0] = (uint8_t)(type >> 8);
    out[1] = (uint8_t)(type & 0xFF);
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len & 0xFF);
    memcpy(out + 4, val, len);
}

static void handle_stun(dtls_link_t *L, const uint8_t *data, size_t len,
                        const struct sockaddr_in *peer) {
    if (len < 20 || memcmp(data + 4, STUN_MAGIC_BYTES, 4) != 0) return;
    uint16_t type = (uint16_t)((data[0] << 8) | data[1]);
    if (type != STUN_TYPE_BINDING_REQUEST) return;

    uint8_t resp[64];
    uint8_t xa[8];
    uint16_t xport = (uint16_t)(ntohs(peer->sin_port) ^ (uint16_t)(STUN_MAGIC >> 16));
    uint32_t xip   = ntohl(peer->sin_addr.s_addr) ^ STUN_MAGIC;
    xa[0] = 0; xa[1] = 1;
    xa[2] = (uint8_t)(xport >> 8); xa[3] = (uint8_t)(xport & 0xFF);
    xa[4] = (uint8_t)(xip >> 24);  xa[5] = (uint8_t)(xip >> 16);
    xa[6] = (uint8_t)(xip >> 8);   xa[7] = (uint8_t)(xip & 0xFF);

    resp[0] = (uint8_t)(STUN_TYPE_BINDING_SUCCESS >> 8);
    resp[1] = (uint8_t)(STUN_TYPE_BINDING_SUCCESS & 0xFF);
    memcpy(resp + 4, STUN_MAGIC_BYTES, 4);
    memcpy(resp + 8, data + 8, 12);

    size_t off = 20;
    stun_pack_attr(resp + off, ATTR_XOR_MAPPED_ADDRESS, xa, sizeof(xa));
    off += 12;

    uint16_t mi_len = (uint16_t)((off - 20) + 24);
    resp[2] = (uint8_t)(mi_len >> 8); resp[3] = (uint8_t)(mi_len & 0xFF);
    uint8_t mac[20];
    if (!stun_hmac_sha1((const uint8_t *)L->our_pwd, strlen(L->our_pwd), resp, off, mac)) return;
    stun_pack_attr(resp + off, ATTR_MESSAGE_INTEGRITY, mac, 20);
    off += 24;

    uint16_t fp_len = (uint16_t)((off - 20) + 8);
    resp[2] = (uint8_t)(fp_len >> 8); resp[3] = (uint8_t)(fp_len & 0xFF);
    uint32_t fp = stun_crc32(resp, off) ^ 0x5354554Eu;
    uint8_t fpb[4] = { (uint8_t)(fp >> 24), (uint8_t)(fp >> 16), (uint8_t)(fp >> 8), (uint8_t)(fp & 0xFF) };
    stun_pack_attr(resp + off, ATTR_FINGERPRINT, fpb, 4);
    off += 8;

    sendto(L->sock, resp, off, 0, (const struct sockaddr *)peer, sizeof(*peer));
    if (!L->first_stun_logged) {
        L->first_stun_logged = true;
        ESP_LOGI(TAG, "[%s] STUN Binding Success sent", L->name);
    }
}

static void stun_send_check(dtls_link_t *L) {
    if (!L->peer_seen || !L->ctrl_ufrag[0] || !L->ctrl_pwd[0] || L->sock < 0) return;
    uint8_t req[128];
    memset(req, 0, sizeof(req));
    req[0] = (uint8_t)(STUN_TYPE_BINDING_REQUEST >> 8);
    req[1] = (uint8_t)(STUN_TYPE_BINDING_REQUEST & 0xFF);
    memcpy(req + 4, STUN_MAGIC_BYTES, 4);
    esp_fill_random(req + 8, 12);

    size_t off = 20;
    char uname[80];
    int ulen = snprintf(uname, sizeof(uname), "%s:%s", L->ctrl_ufrag, L->our_ufrag);
    if (ulen <= 0 || off + 4 + (size_t)ulen + 4 > sizeof(req)) return;
    stun_pack_attr(req + off, ATTR_USERNAME, (const uint8_t *)uname, (size_t)ulen);
    off += 4 + (size_t)ulen + ((4 - ((size_t)ulen % 4)) % 4);

    uint8_t prio[4] = { 0x6E, 0xFF, 0xFF, 0xFF };
    stun_pack_attr(req + off, ATTR_PRIORITY, prio, 4);
    off += 8;

    uint8_t tb[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    stun_pack_attr(req + off, ATTR_ICE_CONTROLLED, tb, 8);
    off += 12;

    uint16_t mi_len = (uint16_t)((off - 20) + 24);
    req[2] = (uint8_t)(mi_len >> 8); req[3] = (uint8_t)(mi_len & 0xFF);
    uint8_t mac[20];
    if (!stun_hmac_sha1((const uint8_t *)L->ctrl_pwd, strlen(L->ctrl_pwd), req, off, mac)) return;
    stun_pack_attr(req + off, ATTR_MESSAGE_INTEGRITY, mac, 20);
    off += 24;

    uint16_t fp_len = (uint16_t)((off - 20) + 8);
    req[2] = (uint8_t)(fp_len >> 8); req[3] = (uint8_t)(fp_len & 0xFF);
    uint32_t fp = stun_crc32(req, off) ^ 0x5354554Eu;
    uint8_t fpb[4] = { (uint8_t)(fp >> 24), (uint8_t)(fp >> 16), (uint8_t)(fp >> 8), (uint8_t)(fp & 0xFF) };
    stun_pack_attr(req + off, ATTR_FINGERPRINT, fpb, 4);
    off += 8;

    sendto(L->sock, req, off, 0, (const struct sockaddr *)&L->peer, sizeof(L->peer));
}

/* ---------- mbedTLS DTLS BIO & Timer callbacks (per-link) ---------- */
static int dtls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    dtls_link_t *L = (dtls_link_t *)ctx;
    if (!L->peer_seen || L->sock < 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    int r = sendto(L->sock, buf, len, 0, (const struct sockaddr *)&L->peer, sizeof(L->peer));
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return r;
}

static int dtls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    dtls_link_t *L = (dtls_link_t *)ctx;
    if (L->rx_len == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = L->rx_len < len ? L->rx_len : len;
    memcpy(buf, L->rx_buf, n);
    L->rx_len = 0;
    return (int)n;
}

static void dtls_set_timer(void *ctx, uint32_t int_ms, uint32_t fin_ms) {
    dtls_timer_t *t = (dtls_timer_t *)ctx;
    if (fin_ms == 0) { t->active = false; return; }
    t->start_ms = esp_timer_get_time() / 1000;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    t->active = true;
}

static int dtls_get_timer(void *ctx) {
    dtls_timer_t *t = (dtls_timer_t *)ctx;
    if (!t->active) return -1;
    int64_t elapsed = (esp_timer_get_time() / 1000) - t->start_ms;
    if (elapsed >= t->fin_ms) return 2;
    if (elapsed >= t->int_ms) return 1;
    return 0;
}

static bool tls_init_shared(void) {
    if (g_tls_shared_init) return true;
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_x509_crt_init(&g_srv_cert);
    mbedtls_pk_init(&g_srv_key);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_entropy_init(&g_entropy);

    const char *pers = "stadia-esp32-dtls";
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) return false;
    if (mbedtls_x509_crt_parse(&g_srv_cert, (const unsigned char *)fullchain_pem_start,
                               (size_t)(fullchain_pem_end - fullchain_pem_start)) < 0) return false;
    if (mbedtls_pk_parse_key(&g_srv_key, (const unsigned char *)privkey_pem_start,
                             (size_t)(privkey_pem_end - privkey_pem_start),
                             NULL, 0, mbedtls_ctr_drbg_random, &g_drbg) != 0) return false;

    /* Precompute "sha-256 AA:BB:..." fingerprint for Game Session SDP answer */
    uint8_t hash[32];
    mbedtls_sha256(g_srv_cert.raw.p, g_srv_cert.raw.len, hash, 0);
    size_t pos = (size_t)snprintf(g_cert_fp, sizeof(g_cert_fp), "sha-256 ");
    for (int i = 0; i < 32 && pos + 3 < sizeof(g_cert_fp); i++) {
        pos += (size_t)snprintf(g_cert_fp + pos, sizeof(g_cert_fp) - pos, "%s%02X", i ? ":" : "", hash[i]);
    }

    if (mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) return false;
    mbedtls_ssl_conf_min_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&g_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_own_cert(&g_conf, &g_srv_cert, &g_srv_key);
    mbedtls_ssl_conf_dtls_cookies(&g_conf, NULL, NULL, NULL);

    static const mbedtls_ssl_srtp_profile profs[] = {
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET
    };
    mbedtls_ssl_conf_dtls_srtp_protection_profiles(&g_conf, profs);

    g_tls_shared_init = true;
    return true;
}

static bool link_init(dtls_link_t *L) {
    mbedtls_ssl_init(&L->ssl);
    if (mbedtls_ssl_setup(&L->ssl, &g_conf) != 0) return false;
    mbedtls_ssl_set_mtu(&L->ssl, 1200);
    mbedtls_ssl_set_bio(&L->ssl, L, dtls_bio_send, dtls_bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&L->ssl, &L->timer, dtls_set_timer, dtls_get_timer);
    stadia_srtp_init(&L->srtp);
    mbedtls_ssl_set_export_keys_cb(&L->ssl, stadia_srtp_export_cb, &L->srtp);

    L->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (L->sock < 0) return false;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(L->local_port);
    if (bind(L->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(L->sock);
        L->sock = -1;
        return false;
    }
    return true;
}

static void link_reset(dtls_link_t *L) {
    mbedtls_ssl_session_reset(&L->ssl);
    stadia_srtp_free(&L->srtp);
    mbedtls_ssl_set_export_keys_cb(&L->ssl, stadia_srtp_export_cb, &L->srtp);
    if (L == &g_disp) {
        g_audio_enabled = false;
        g_audio_next_tx_us = 0;
    }
    mbedtls_ssl_set_mtu(&L->ssl, 1200);
    L->timer.active = false;
    L->rx_len = 0;
    L->dtls_seen = false;
    L->handshake_done = false;
    L->dtls_done_us = 0;
    L->last_stun_tx_us = 0;
    L->first_stun_logged = false;
}

/* ---------- WireHeader2 + RPC send helpers ---------- */
static bool link_send_wire(dtls_link_t *L, uint16_t wire_id, const uint8_t *payload, size_t len) {
    if (!L->handshake_done || len > TX_BUF_MAX) return false;
    size_t total = WIRE_HDR_LEN + len;
    g_wire_tx[0] = 0x00; g_wire_tx[1] = 0x00;
    g_wire_tx[2] = (uint8_t)(wire_id & 0xFF);
    g_wire_tx[3] = (uint8_t)((wire_id >> 8) & 0xFF);
    g_wire_tx[4] = (uint8_t)(total & 0xFF);
    g_wire_tx[5] = (uint8_t)((total >> 8) & 0xFF);
    g_wire_tx[6] = (uint8_t)((total >> 16) & 0xFF);
    g_wire_tx[7] = 0x00;
    if (len > 0) memcpy(g_wire_tx + WIRE_HDR_LEN, payload, len);
    return mbedtls_ssl_write(&L->ssl, g_wire_tx, total) == (int)total;
}

static void link_send_rpc_request(dtls_link_t *L, uint16_t wire_id, uint32_t ticket,
                                  const uint8_t *payload, size_t len) {
    gotham_rpc_RpcMessage *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) return;
    rpc->which_payload = gotham_rpc_RpcMessage_request_tag;
    rpc->payload.request.has_ticket = true;
    rpc->payload.request.ticket = ticket;
    if (payload && len > 0 && len <= sizeof(rpc->payload.request.payload.bytes)) {
        rpc->payload.request.has_payload = true;
        memcpy(rpc->payload.request.payload.bytes, payload, len);
        rpc->payload.request.payload.size = (pb_size_t)len;
    }
    pb_ostream_t os = pb_ostream_from_buffer(g_rpc_tx, sizeof(g_rpc_tx));
    if (pb_encode(&os, gotham_rpc_RpcMessage_fields, rpc)) {
        link_send_wire(L, wire_id, g_rpc_tx, os.bytes_written);
    }
    free(rpc);
}

static void link_send_rpc_response(dtls_link_t *L, uint16_t wire_id, uint32_t ticket,
                                   uint32_t status_code, const uint8_t *payload, size_t len) {
    gotham_rpc_RpcMessage *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) return;
    rpc->which_payload = gotham_rpc_RpcMessage_response_tag;
    rpc->payload.response.has_ticket = true;
    rpc->payload.response.ticket = ticket;
    rpc->payload.response.has_code = true;
    rpc->payload.response.code = status_code;
    rpc->payload.response.has_error_message = true;
    rpc->payload.response.error_message[0] = '\0';
    if (payload && len > 0 && len <= sizeof(rpc->payload.response.payload.bytes)) {
        rpc->payload.response.has_payload = true;
        memcpy(rpc->payload.response.payload.bytes, payload, len);
        rpc->payload.response.payload.size = (pb_size_t)len;
    }
    pb_ostream_t os = pb_ostream_from_buffer(g_rpc_tx, sizeof(g_rpc_tx));
    if (pb_encode(&os, gotham_rpc_RpcMessage_fields, rpc)) {
        link_send_wire(L, wire_id, g_rpc_tx, os.bytes_written);
    }
    free(rpc);
}

static void send_get_controller_state_request(void) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_get_controller_state = true;
    dtc->get_controller_state.display_supports_send_log_report = false;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&g_disp, WIRE_ID_RPC_DISPLAY, ++g_rpc_ticket, buf, os.bytes_written);
    }
    free(dtc);
}

static void send_start_game_request(void) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_start_game = true;
    dtc->start_game.has_value = true;
    snprintf(dtc->start_game.value, sizeof(dtc->start_game.value), "%s", GAME_KEY);
    dtc->start_game.has_number = true;
    dtc->start_game.number = 1;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&g_disp, WIRE_ID_RPC_DISPLAY, ++g_rpc_ticket, buf, os.bytes_written);
        g_start_game_sent = true;
        ESP_LOGI(TAG, "Sent StartGame (key=%s) for rumble channel", GAME_KEY);
    }
    free(dtc);
}

static void send_set_audio_mode(uint32_t mode, bool f3) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_set_audio_mode = true;
    dtc->set_audio_mode.has_mode = true;
    dtc->set_audio_mode.mode = mode;
    dtc->set_audio_mode.has_f3 = true;
    dtc->set_audio_mode.f3 = f3;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&g_disp, WIRE_ID_RPC_DISPLAY, ++g_rpc_ticket, buf, os.bytes_written);
        ESP_LOGI(TAG, "Sent SetAudioMode(mode=%lu, f3=%d)", (unsigned long)mode, f3 ? 1 : 0);
    }
    free(dtc);
}

static void send_set_microphone(bool enable) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_set_microphone = true;
    dtc->set_microphone.has_value = true;
    dtc->set_microphone.value = enable;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&g_disp, WIRE_ID_RPC_DISPLAY, ++g_rpc_ticket, buf, os.bytes_written);
        ESP_LOGI(TAG, "Sent SetMicrophone(%d)", enable ? 1 : 0);
    }
    free(dtc);
}

static void service_audio_tx(void) {
    if (!g_audio_enabled || !g_disp.srtp.keys_ready || !g_disp.peer_seen) return;

    uint8_t opus_pkt[STADIA_MAX_OPUS_PKT_BYTES];
    size_t flen = 0;
    while (stadia_opus_pop_tx_packet(opus_pkt, sizeof(opus_pkt), &flen)) {
        if (flen == 0 || 12u + flen + STADIA_SRTP_TAG_LEN > sizeof(g_audio_tx_buf)) continue;

        int64_t now = esp_timer_get_time();
        bool marker = (g_audio_tx_pkts == 0) || (g_audio_next_tx_us != 0 && (now - g_audio_next_tx_us) > 200000);
        g_audio_next_tx_us = now;

        uint16_t seq = g_audio_seq++;
        uint32_t ts  = g_audio_ts;
        g_audio_ts += 960; /* 20ms @ 48kHz */
        const uint32_t ssrc = 0x51A01234u;

        g_audio_tx_buf[0] = 0x80;
        g_audio_tx_buf[1] = marker ? (uint8_t)(0x80 | 111) : 111;
        g_audio_tx_buf[2] = (uint8_t)(seq >> 8);
        g_audio_tx_buf[3] = (uint8_t)(seq & 0xFF);
        g_audio_tx_buf[4] = (uint8_t)(ts >> 24);
        g_audio_tx_buf[5] = (uint8_t)(ts >> 16);
        g_audio_tx_buf[6] = (uint8_t)(ts >> 8);
        g_audio_tx_buf[7] = (uint8_t)(ts & 0xFF);
        g_audio_tx_buf[8] = (uint8_t)(ssrc >> 24);
        g_audio_tx_buf[9] = (uint8_t)(ssrc >> 16);
        g_audio_tx_buf[10] = (uint8_t)(ssrc >> 8);
        g_audio_tx_buf[11] = (uint8_t)(ssrc & 0xFF);
        memcpy(g_audio_tx_buf + 12, opus_pkt, flen);

        size_t srtp_len = 0;
        if (stadia_srtp_protect(&g_disp.srtp, g_audio_tx_buf, 12u + flen, &srtp_len)) {
            sendto(g_disp.sock, g_audio_tx_buf, srtp_len, 0,
                   (const struct sockaddr *)&g_disp.peer, sizeof(g_disp.peer));
            g_audio_tx_pkts++;
        }
    }
}

/* ---------- Game Session: ProxyRequest -> ProxyResponse + Rumble TX ---------- */
static void handle_proxy_request_raw(uint32_t outer_ticket, const uint8_t *proxy_bytes, size_t proxy_len) {
    /* ProxyRequest.body = field 4 -> inner RpcMessage request -> StreamerRequest.initialization(1).offer(2) */
    const uint8_t *body = NULL; size_t body_len = 0;
    if (!pbw_find_bytes(proxy_bytes, proxy_len, 4, &body, &body_len)) {
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        return;
    }

    gotham_rpc_RpcMessage *inner_req = calloc(1, sizeof(*inner_req));
    if (!inner_req) return;
    pb_istream_t is = pb_istream_from_buffer(body, body_len);
    if (!pb_decode(&is, gotham_rpc_RpcMessage_fields, inner_req) ||
        inner_req->which_payload != gotham_rpc_RpcMessage_request_tag ||
        !inner_req->payload.request.has_payload) {
        free(inner_req);
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        return;
    }
    uint32_t inner_ticket = inner_req->payload.request.ticket;
    const uint8_t *sreq = inner_req->payload.request.payload.bytes;
    size_t sreq_len = inner_req->payload.request.payload.size;

    const uint8_t *init_req = NULL; size_t init_req_len = 0;
    const uint8_t *offer_bytes = NULL; size_t offer_len = 0;
    if (!pbw_find_bytes(sreq, sreq_len, 1, &init_req, &init_req_len) ||
        !pbw_find_bytes(init_req, init_req_len, 2, &offer_bytes, &offer_len)) {
        free(inner_req);
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        return;
    }

    bool raw_dtls = true, delayed_acks = false, use_rtp_ts = false;
    chrome_cloudcast_proto_api_common_RtcSdpOffer *offer = calloc(1, sizeof(*offer));
    if (offer) {
        pb_istream_t ois = pb_istream_from_buffer(offer_bytes, offer_len);
        if (pb_decode(&ois, chrome_cloudcast_proto_api_common_RtcSdpOffer_fields, offer)) {
            if (offer->has_raw_dtls) raw_dtls = offer->raw_dtls;
            if (offer->has_delayed_acks) delayed_acks = offer->delayed_acks;
            if (offer->has_use_rtp_timestamps) use_rtp_ts = offer->use_rtp_timestamps;
            snprintf(g_game.ctrl_ufrag, sizeof(g_game.ctrl_ufrag), "%.63s", offer->ice_ufrag);
            snprintf(g_game.ctrl_pwd,   sizeof(g_game.ctrl_pwd),   "%.63s", offer->ice_password);
            ESP_LOGI(TAG, "[GAME] Offer decoded: ufrag=%s raw_dtls=%d", g_game.ctrl_ufrag, (int)raw_dtls);
        }
        free(offer);
    }
    free(inner_req);

    link_reset(&g_game);
    g_game.peer_seen = false;
    g_game_tx_seq = 0;
    g_game_use_rtp = false;

    /* Encode RtcSdpAnswer + RtcIceCandidate for Game Session (:50002) */
    chrome_cloudcast_proto_api_common_RtcSdpAnswer *ans = calloc(1, sizeof(*ans));
    chrome_cloudcast_proto_api_common_RtcIceCandidate *cand = calloc(1, sizeof(*cand));
    uint8_t *ans_buf = malloc(512);
    uint8_t *cand_buf = malloc(256);
    uint8_t *init_resp = malloc(896);
    uint8_t *sresp = malloc(960);
    uint8_t *inner_resp_buf = malloc(1024);
    uint8_t *dtc_buf = malloc(1088);
    gotham_rpc_RpcMessage *inner_resp = calloc(1, sizeof(*inner_resp));

    if (ans && cand && ans_buf && cand_buf && init_resp && sresp && inner_resp_buf && dtc_buf && inner_resp) {
        ans->has_ice_ufrag = true;
        snprintf(ans->ice_ufrag, sizeof(ans->ice_ufrag), "%s", GAME_ICE_UFRAG);
        ans->has_ice_password = true;
        snprintf(ans->ice_password, sizeof(ans->ice_password), "%s", GAME_ICE_PWD);
        ans->has_fingerprint = true;
        snprintf(ans->fingerprint, sizeof(ans->fingerprint), "%s", g_cert_fp);
        ans->has_raw_dtls = true;          ans->raw_dtls = raw_dtls;
        ans->has_delayed_acks = true;      ans->delayed_acks = delayed_acks;
        ans->has_use_rtp_timestamps = true; ans->use_rtp_timestamps = use_rtp_ts;
        ans->has_disable_game_stats = true; ans->disable_game_stats = true;
        ans->has_disable_frame_tokens = true; ans->disable_frame_tokens = true;
        ans->has_network_type = true;
        ans->network_type = chrome_cloudcast_proto_api_common_RtcSdpNetworkType_NETWORK_TYPE_ETHERNET;

        cand->has_sdp_mid = true;          snprintf(cand->sdp_mid, sizeof(cand->sdp_mid), "0");
        cand->has_sdp_mline_index = true;  cand->sdp_mline_index = 0;
        cand->has_foundation = true;       snprintf(cand->foundation, sizeof(cand->foundation), "1");
        cand->has_priority = true;         cand->priority = (126u << 24) | (65535u << 8) | 255u;
        cand->has_local_endpoint_ip = true; snprintf(cand->local_endpoint_ip, sizeof(cand->local_endpoint_ip), "192.168.4.1");
        cand->has_local_endpoint_port = true; cand->local_endpoint_port = GAME_UDP_PORT;

        pb_ostream_t aos = pb_ostream_from_buffer(ans_buf, 512);
        pb_ostream_t cos = pb_ostream_from_buffer(cand_buf, 256);
        if (pb_encode(&aos, chrome_cloudcast_proto_api_common_RtcSdpAnswer_fields, ans) &&
            pb_encode(&cos, chrome_cloudcast_proto_api_common_RtcIceCandidate_fields, cand)) {
            /* InitializationResponse { answer(1), candidate(2) } */
            size_t il = pbw_put_bytes(init_resp, 896, 1, ans_buf, aos.bytes_written);
            il += pbw_put_bytes(init_resp + il, 896 - il, 2, cand_buf, cos.bytes_written);
            /* StreamerResponse { initialization(1) } */
            size_t sl = pbw_put_bytes(sresp, 960, 1, init_resp, il);
            /* Inner RpcMessage response */
            inner_resp->which_payload = gotham_rpc_RpcMessage_response_tag;
            inner_resp->payload.response.has_ticket = true;
            inner_resp->payload.response.ticket = inner_ticket;
            inner_resp->payload.response.has_code = true;
            inner_resp->payload.response.code = gotham_rpc_StatusCode_OK;
            inner_resp->payload.response.has_error_message = true;
            inner_resp->payload.response.error_message[0] = '\0';
            inner_resp->payload.response.has_payload = true;
            memcpy(inner_resp->payload.response.payload.bytes, sresp, sl);
            inner_resp->payload.response.payload.size = (pb_size_t)sl;

            pb_ostream_t ros = pb_ostream_from_buffer(inner_resp_buf, 1024);
            if (pb_encode(&ros, gotham_rpc_RpcMessage_fields, inner_resp)) {
                /* ProxyResponse { data(1) } wrapped in DisplayToController { proxy_response(1) } */
                uint8_t pr_buf[1056];
                size_t prl = pbw_put_bytes(pr_buf, sizeof(pr_buf), 1, inner_resp_buf, ros.bytes_written);
                size_t dtcl = pbw_put_bytes(dtc_buf, 1088, 1, pr_buf, prl);
                link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, outer_ticket,
                                       gotham_rpc_StatusCode_OK, dtc_buf, dtcl);
                ESP_LOGI(TAG, "[GAME] Sent ProxyResponse -> awaiting UDP :%d", GAME_UDP_PORT);
            }
        }
    }
    free(ans); free(cand); free(ans_buf); free(cand_buf);
    free(init_resp); free(sresp); free(inner_resp_buf); free(dtc_buf); free(inner_resp);
}

static void send_game_rumble(uint16_t left, uint16_t right) {
    if (!g_game.handshake_done) return;
    /* InputProtocolMessage { input_message_series(7) { session_id(1), seq(2), base_ts(3), messages(4) { input_feedback_message_v2(10) { gamepad_vibration(1) { id(1), left(2), right(3) } } } } } */
    uint8_t vib[32], fb[48], slim[64], series[160], ipm[180];
    size_t vl = 0;
    if (g_game_pad_id > 0) vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 1, g_game_pad_id);
    if (left > 0)          vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 2, left);
    if (right > 0)         vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 3, right);
    size_t fbl = pbw_put_bytes(fb, sizeof(fb), 1, vib, vl);
    size_t sll = pbw_put_bytes(slim, sizeof(slim), 10, fb, fbl);
    size_t serl = 0;
    if (g_game_session_id[0]) {
        serl += pbw_put_bytes(series + serl, sizeof(series) - serl, 1,
                              (const uint8_t *)g_game_session_id, strlen(g_game_session_id));
    }
    serl += pbw_put_u64(series + serl, sizeof(series) - serl, 2, ++g_game_tx_seq);
    serl += pbw_put_u64(series + serl, sizeof(series) - serl, 3, (uint64_t)esp_timer_get_time());
    serl += pbw_put_bytes(series + serl, sizeof(series) - serl, 4, slim, sll);
    size_t ipml = pbw_put_bytes(ipm, sizeof(ipm), 7, series, serl);

    if (g_game_use_rtp) {
        uint8_t rtp[12 + 180];
        uint16_t seq = ++g_game_rtp_seq;
        rtp[0] = 0x80; rtp[1] = 96;
        rtp[2] = (uint8_t)(seq >> 8); rtp[3] = (uint8_t)(seq & 0xFF);
        memset(rtp + 4, 0, 8);
        rtp[8] = 0x53; rtp[9] = 0x54; rtp[10] = 0x41; rtp[11] = 0x44;
        memcpy(rtp + 12, ipm, ipml);
        link_send_wire(&g_game, WIRE_ID_RTP, rtp, 12 + ipml);
    } else {
        link_send_wire(&g_game, WIRE_ID_RTP, ipm, ipml);
    }
    g_rumble_left_sent = left;
    g_rumble_right_sent = right;
    g_last_rumble_tx_us = esp_timer_get_time();
}

/* ---------- Input dispatch ---------- */
static void dispatch_input(uint32_t buttons,
                           int32_t lx, int32_t ly, int32_t rx, int32_t ry,
                           uint32_t lt, uint32_t rt) {
    if (g_last_set && buttons == g_last_buttons &&
        lx == g_last_lx && ly == g_last_ly &&
        rx == g_last_rx && ry == g_last_ry &&
        lt == g_last_lt && rt == g_last_rt) {
        return;
    }
    g_last_set = true;
    g_last_buttons = buttons;
    g_last_lx = lx; g_last_ly = ly;
    g_last_rx = rx; g_last_ry = ry;
    g_last_lt = lt; g_last_rt = rt;
    usb_gamepad_send(buttons, lx, ly, rx, ry, lt, rt);
}

/* ---------- Display & Game RPC handlers ---------- */
static void handle_get_current_time(uint32_t ticket,
                                    const gotham_display_GetCurrentTimeRequest *req) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_current_time = true;
    dtc->current_time.has_controller_time_us = true;
    dtc->current_time.controller_time_us = req->has_controller_time_us ? req->controller_time_us : 0;
    dtc->current_time.has_display_time_us = true;
    dtc->current_time.display_time_us = esp_timer_get_time();

    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, ticket,
                               gotham_rpc_StatusCode_OK, buf, os.bytes_written);
    }
    free(dtc);
}

static void handle_disp_rpc_request(uint32_t ticket, const uint8_t *payload, size_t len) {
    /* Intercept ProxyRequest (field 1 of ControllerToDisplay) before nanopb 128B cap */
    const uint8_t *proxy = NULL; size_t proxy_len = 0;
    if (pbw_find_bytes(payload, len, 1, &proxy, &proxy_len)) {
        handle_proxy_request_raw(ticket, proxy, proxy_len);
        return;
    }

    gotham_display_ControllerToDisplay *ctd = calloc(1, sizeof(*ctd));
    if (!ctd) return;
    pb_istream_t is = pb_istream_from_buffer(payload, len);
    if (!pb_decode(&is, gotham_display_ControllerToDisplay_fields, ctd)) {
        free(ctd);
        return;
    }
    switch (ctd->which_variant) {
    case gotham_display_ControllerToDisplay_keepalive_tag:
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        break;
    case gotham_display_ControllerToDisplay_get_current_time_tag:
        handle_get_current_time(ticket, &ctd->variant.get_current_time);
        break;
    case gotham_display_ControllerToDisplay_disconnect_tag:
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        break;
    default:
        link_send_rpc_response(&g_disp, WIRE_ID_RPC_DISPLAY, ticket, 12 /* UNIMPLEMENTED */, NULL, 0);
        break;
    }
    free(ctd);
}

static void handle_battery_float(float raw, const char *src) {
    if (isnan(raw)) return;
    if (raw < 0.0f) raw = 0.0f;
    if (raw > 1.0f) raw = 1.0f;
    int pct = (int)lroundf(raw * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != g_battery_pct) {
        g_battery_pct = pct;
        ESP_LOGI(TAG, "Controller battery (%s): %d%%", src, pct);
        usb_gamepad_set_battery(pct);
    }
    if (pct > 10) g_batt_warn_5 = false;
    if (pct > 20) g_batt_warn_15 = false;
}

static void handle_disp_rpc_notification(const uint8_t *payload, size_t len) {
    gotham_display_ControllerToDisplay *ctd = calloc(1, sizeof(*ctd));
    if (!ctd) return;
    pb_istream_t is = pb_istream_from_buffer(payload, len);
    if (pb_decode(&is, gotham_display_ControllerToDisplay_fields, ctd)) {
        if (ctd->which_variant == gotham_display_ControllerToDisplay_message3_tag) {
            const gotham_display_Message3 *m = &ctd->variant.message3;
            dispatch_input(m->f1, m->f2, m->f3, m->f4, m->f5, m->f6, m->f7);
        } else if (ctd->which_variant == gotham_display_ControllerToDisplay_notification10_tag &&
                   ctd->variant.notification10.has_value) {
            handle_battery_float(ctd->variant.notification10.value, "notif10");
        } else if (ctd->which_variant == gotham_display_ControllerToDisplay_audio_configuration_changed_tag &&
                   ctd->variant.audio_configuration_changed.has_mode) {
            uint32_t m = ctd->variant.audio_configuration_changed.mode;
            if (m == 0 || m == 1) {
                g_headset_plugged = (m == 1);
                ESP_LOGI(TAG, "3.5mm headset %s", g_headset_plugged ? "PLUGGED IN" : "UNPLUGGED");
            }
        }
    }
    free(ctd);
}

static void handle_disp_rpc_response(const gotham_rpc_RpcResponse *resp) {
    if (!resp->has_payload || resp->payload.size == 0) return;
    gotham_display_ControllerToDisplay *ctd = calloc(1, sizeof(*ctd));
    if (!ctd) return;
    pb_istream_t is = pb_istream_from_buffer(resp->payload.bytes, resp->payload.size);
    if (pb_decode(&is, gotham_display_ControllerToDisplay_fields, ctd) &&
        ctd->which_variant == gotham_display_ControllerToDisplay_controller_state_tag) {
        const gotham_display_GetControllerStateResponse *cs = &ctd->variant.controller_state;
        if (cs->has_f4) g_headset_plugged = cs->f4;
        if (cs->has_f10) {
            float f10_val = 0.0f;
            memcpy(&f10_val, &cs->f10, sizeof(f10_val));
            ESP_LOGI(TAG, "ControllerState f10=0x%08lx (%.4f) f11=%lu",
                     (unsigned long)cs->f10, (double)f10_val,
                     (unsigned long)(cs->has_f11 ? cs->f11 : 0));
            if (!isnan(f10_val) && f10_val >= 0.005f && f10_val <= 1.0f) {
                handle_battery_float(f10_val, "state_f10");
            }
        }
        g_rpc_ready = true;
        ESP_LOGI(TAG, "===== READY! Starting game session for rumble =====");
        if (!g_start_game_sent) send_start_game_request();
        if (!g_audio_enabled) {
            send_set_audio_mode(2, true);
            send_set_microphone(true);
            g_audio_enabled = true;
            g_audio_next_tx_us = 0;
        }
    }
    free(ctd);
}

static void handle_rpc(dtls_link_t *L, uint16_t rx_wire_id, const uint8_t *data, size_t len) {
    gotham_rpc_RpcMessage *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) return;
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, gotham_rpc_RpcMessage_fields, rpc)) {
        free(rpc);
        return;
    }
    if (L == &g_game) {
        /* Game session RPCs: answer requests with empty OK on the same wire id */
        if (rpc->which_payload == gotham_rpc_RpcMessage_request_tag) {
            link_send_rpc_response(&g_game, rx_wire_id, rpc->payload.request.ticket,
                                   gotham_rpc_StatusCode_OK, NULL, 0);
        }
        free(rpc);
        return;
    }
    switch (rpc->which_payload) {
    case gotham_rpc_RpcMessage_request_tag:
        if (rpc->payload.request.has_payload) {
            handle_disp_rpc_request(rpc->payload.request.ticket,
                                    rpc->payload.request.payload.bytes,
                                    rpc->payload.request.payload.size);
        }
        break;
    case gotham_rpc_RpcMessage_notification_tag:
        if (rpc->payload.notification.has_payload) {
            handle_disp_rpc_notification(rpc->payload.notification.payload.bytes,
                                         rpc->payload.notification.payload.size);
        }
        break;
    case gotham_rpc_RpcMessage_response_tag:
        handle_disp_rpc_response(&rpc->payload.response);
        break;
    default:
        break;
    }
    free(rpc);
}

/* ---------- Input on WireHeader2 0x66 (RTP or bare InputProtocolMessage) ---------- */
static const uint8_t *strip_rtp_header(const uint8_t *data, size_t len, size_t *out_len) {
    if (len < 12 || (data[0] >> 6) != 2) return NULL;
    size_t off = 12 + 4u * (data[0] & 0x0Fu);
    if (off > len) return NULL;
    if (data[0] & 0x10u) {
        if (off + 4 > len) return NULL;
        off += 4 + 4u * (((size_t)data[off + 2] << 8) | data[off + 3]);
        if (off > len) return NULL;
    }
    if ((data[0] & 0x20u) && len > off) {
        uint8_t pad = data[len - 1];
        if (pad > 0 && pad <= len - off) len -= pad;
    }
    *out_len = len - off;
    return data + off;
}

static inline int32_t pbw_zigzag32(uint32_t n) {
    return (int32_t)((n >> 1) ^ (~(n & 1u) + 1u));
}

typedef struct {
    uint32_t id;
    uint32_t buttons;
    int32_t  lx, ly, rx, ry;
    uint32_t lt, rt;
    bool     is_reg;
} fast_gamepad_v2_t;

static bool parse_gamepad_v2_fast(const uint8_t *buf, size_t len, fast_gamepad_v2_t *out) {
    memset(out, 0, sizeof(*out));
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        if (!pbw_read_varint(buf, len, &off, &tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (wt == 0) {
            uint64_t v = 0;
            if (!pbw_read_varint(buf, len, &off, &v)) return false;
            switch (fn) {
            case 1:  out->id = (uint32_t)v; break;
            case 2:  out->buttons = (uint32_t)v; break;
            case 3:  out->lx = pbw_zigzag32((uint32_t)v); break;
            case 4:  out->ly = pbw_zigzag32((uint32_t)v); break;
            case 5:  out->rx = pbw_zigzag32((uint32_t)v); break;
            case 6:  out->ry = pbw_zigzag32((uint32_t)v); break;
            case 7:  out->lt = (uint32_t)v; break;
            case 8:  out->rt = (uint32_t)v; break;
            case 16: if (v != 0) out->is_reg = true; break; /* device_presence */
            default: break;
            }
        } else if (wt == 1) {
            if (off + 8 > len) return false;
            off += 8;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pbw_read_varint(buf, len, &off, &flen) || off + flen > len) return false;
            if (fn == 18) out->is_reg = true; /* registration_info */
            off += (size_t)flen;
        } else if (wt == 5) {
            if (off + 4 > len) return false;
            off += 4;
        } else {
            return false;
        }
    }
    return true;
}

static void handle_0x66_input(dtls_link_t *L, const uint8_t *payload, size_t len) {
    const uint8_t *pb_data = payload;
    size_t pb_len = len;
    bool was_rtp = false;

    size_t rtp_len = 0;
    const uint8_t *rtp_body = strip_rtp_header(payload, len, &rtp_len);
    if (rtp_body && rtp_len > 0) {
        if ((payload[1] & 0x7Fu) == MIC_PAYLOAD_TYPE) return;
        pb_data = rtp_body;
        pb_len = rtp_len;
        was_rtp = true;
    }

    /* Field 7 of InputProtocolMessage = input_message_series */
    const uint8_t *ser = NULL;
    size_t ser_len = 0;
    if (!pbw_find_bytes(pb_data, pb_len, 7, &ser, &ser_len)) {
        if (!was_rtp || !pbw_find_bytes(payload, len, 7, &ser, &ser_len)) return;
        was_rtp = false;
    }

    if (L == &g_game) g_game_use_rtp = was_rtp;

    uint64_t seq_num = 0;
    fast_gamepad_v2_t latest_g = {0};
    bool have_g = false;

    size_t off = 0;
    while (off < ser_len) {
        uint64_t tag = 0;
        if (!pbw_read_varint(ser, ser_len, &off, &tag)) break;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (wt == 0) {
            uint64_t v = 0;
            if (!pbw_read_varint(ser, ser_len, &off, &v)) break;
            if (fn == 2) seq_num = v; /* sequence_number */
        } else if (wt == 1) {
            if (off + 8 > ser_len) break;
            off += 8;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pbw_read_varint(ser, ser_len, &off, &flen) || off + flen > ser_len) break;
            const uint8_t *fptr = ser + off;
            off += (size_t)flen;
            if (fn == 1 && L == &g_game && flen > 0) {
                /* session_id */
                size_t cpy = flen < sizeof(g_game_session_id) - 1 ? (size_t)flen : sizeof(g_game_session_id) - 1;
                memcpy(g_game_session_id, fptr, cpy);
                g_game_session_id[cpy] = '\0';
            } else if (fn == 4) {
                /* InputMessageSlim -> field 5 = gamepad_message_v2 */
                const uint8_t *gp = NULL;
                size_t gp_len = 0;
                if (pbw_find_bytes(fptr, (size_t)flen, 5, &gp, &gp_len)) {
                    fast_gamepad_v2_t g;
                    if (parse_gamepad_v2_fast(gp, gp_len, &g)) {
                        if (g.id > 0) g_game_pad_id = g.id;
                        if (!g.is_reg) {
                            latest_g = g;
                            have_g = true;
                        }
                    }
                }
            }
        } else if (wt == 5) {
            if (off + 4 > ser_len) break;
            off += 4;
        } else {
            break;
        }
    }

    if (have_g) {
        dispatch_input(latest_g.buttons, latest_g.lx, latest_g.ly,
                       latest_g.rx, latest_g.ry, latest_g.lt, latest_g.rt);
    }

    /* Ack sequence_number (field 4 = ack_sequence_number in InputProtocolMessage) */
    uint8_t ack[16];
    size_t al = pbw_put_u64(ack, sizeof(ack), 4, seq_num);
    if (was_rtp) {
        uint8_t rtp[12 + 16];
        uint16_t seq = ++g_game_rtp_seq;
        rtp[0] = 0x80; rtp[1] = 96;
        rtp[2] = (uint8_t)(seq >> 8); rtp[3] = (uint8_t)(seq & 0xFF);
        memset(rtp + 4, 0, 8);
        memcpy(rtp + 12, ack, al);
        link_send_wire(L, WIRE_ID_RTP, rtp, 12 + al);
    } else {
        link_send_wire(L, WIRE_ID_RTP, ack, al);
    }
}

static void handle_app_data(dtls_link_t *L, const uint8_t *data, size_t len) {
    if (len < WIRE_HDR_LEN || (data[0] >> 4) != 0) return;
    uint16_t id = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    uint32_t total = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16);
    if (total != len) return;

    if (id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY ||
        (L == &g_game && id == WIRE_ID_RPC_GAME)) {
        handle_rpc(L, id, data + WIRE_HDR_LEN, len - WIRE_HDR_LEN);
    } else if (id == WIRE_ID_RTP) {
        handle_0x66_input(L, data + WIRE_HDR_LEN, len - WIRE_HDR_LEN);
    }
}

static void pump_dtls(dtls_link_t *L) {
    if (!L->handshake_done) {
        int rc = mbedtls_ssl_handshake(&L->ssl);
        if (rc == 0) {
            L->handshake_done = true;
            L->dtls_done_us = esp_timer_get_time();
            if (L == &g_disp) {
                g_rpc_ready = false;
                g_start_game_sent = false;
                g_last_state_req_us = 0;
            } else {
                g_rumble_dirty = true;
            }
            ESP_LOGI(TAG, "[%s] DTLS handshake complete!", L->name);
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            link_reset(L);
        }
        return;
    }

    for (;;) {
        int r = mbedtls_ssl_read(&L->ssl, g_app_buf, sizeof(g_app_buf));
        if (r > 0) {
            handle_app_data(L, g_app_buf, (size_t)r);
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        link_reset(L);
        if (L == &g_disp) {
            g_rpc_ready = false;
            g_start_game_sent = false;
            g_last_set = false;
            g_battery_pct = -1;
            g_batt_warn_15 = false;
            g_batt_warn_5 = false;
            g_headset_plugged = false;
            usb_gamepad_set_battery(-1);
            usb_gamepad_reset();
            link_reset(&g_game);
        }
        break;
    }
}

static void service_socket(dtls_link_t *L) {
    for (int burst = 0; burst < 8; burst++) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(L->sock, g_udp_buf, sizeof(g_udp_buf), MSG_DONTWAIT, (struct sockaddr *)&src, &slen);
        if (n <= 0) return;
        if (n == 1 && g_udp_buf[0] == 0xFF) continue;

        if (!L->peer_seen || L->peer.sin_addr.s_addr != src.sin_addr.s_addr ||
            L->peer.sin_port != src.sin_port) {
            L->peer = src;
            L->peer_seen = true;
        }

        uint8_t b0 = g_udp_buf[0];
        if (b0 <= 3 && n >= 20 && memcmp(g_udp_buf + 4, STUN_MAGIC_BYTES, 4) == 0) {
            handle_stun(L, g_udp_buf, (size_t)n, &src);
        } else if (b0 >= 20 && b0 <= 63) {
            L->dtls_seen = true;
            if (L->handshake_done && b0 == 22 && n >= 14 &&
                g_udp_buf[3] == 0 && g_udp_buf[4] == 0 && g_udp_buf[13] == 1) {
                link_reset(L);
                L->dtls_seen = true;
            }
            memcpy(L->rx_buf, g_udp_buf, (size_t)n);
            L->rx_len = (size_t)n;
            pump_dtls(L);
        } else if (b0 >= 128 && b0 <= 191 && L == &g_disp && L->srtp.keys_ready) {
            uint8_t pt = g_udp_buf[1] & 0x7Fu;
            if (pt < 64 || pt > 95) { /* RTP (skip RTCP 72..76) */
                size_t rtp_len = 0;
                if (stadia_srtp_unprotect(&L->srtp, g_udp_buf, (size_t)n, &rtp_len) &&
                    rtp_len > 12 && pt == 111) {
                    size_t hlen = 12u + 4u * (g_udp_buf[0] & 0x0Fu);
                    if ((g_udp_buf[0] & 0x10u) && hlen + 4u <= rtp_len) {
                        uint16_t ext_w = (uint16_t)((g_udp_buf[hlen + 2] << 8) | g_udp_buf[hlen + 3]);
                        hlen += 4u + 4u * ext_w;
                    }
                    if (rtp_len > hlen) {
                        size_t plen = rtp_len - hlen;
                        g_mic_rx_pkts++;
                        g_mic_rx_bytes += (uint32_t)plen;
                        stadia_opus_push_rx_packet(g_udp_buf + hlen, plen);
                    }
                }
            }
        }
    }
}

static void service_timers(dtls_link_t *L) {
    int64_t now = esp_timer_get_time();
    if (L->peer_seen && !L->handshake_done && L->timer.active && dtls_get_timer(&L->timer) == 2) {
        pump_dtls(L);
    }
    if (L->peer_seen && L->ctrl_ufrag[0] && L->ctrl_pwd[0]) {
        int64_t interval_us = L->dtls_seen ? 2500000 : 100000;
        if (L->last_stun_tx_us == 0 || (now - L->last_stun_tx_us) >= interval_us) {
            L->last_stun_tx_us = now;
            stun_send_check(L);
        }
    }
}

/* On-board XIAO ESP32-S3 orange user LED (GPIO 21, active-LOW) */
#define STATUS_LED_GPIO GPIO_NUM_21

static void status_led_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(STATUS_LED_GPIO, 1); /* OFF (active-low) */
}

static void status_led_update(int64_t now_us, bool ready, bool mic_muted, bool low_batt, bool combo_held) {
    static int s_last_level = -1;
    bool on;
    if (combo_held) {
        on = ((now_us / 100000) & 1) == 0; /* Fast 100ms blink while holding mode combo */
    } else if (!ready) {
        on = (now_us % 1000000) < 100000;  /* Slow heartbeat: 100ms ON / 900ms OFF */
    } else if (mic_muted) {
        on = ((now_us / 500000) & 1) == 0; /* 500ms blink when connected & mic muted */
    } else if (low_batt) {
        int64_t t = now_us % 2000000;      /* Double-dip OFF every 2s when battery <= 15% */
        on = !((t < 120000) || (t >= 240000 && t < 360000));
    } else {
        on = true;                         /* Solid ON when connected & mic active */
    }
    int level = on ? 0 : 1; /* Active-LOW */
    if (level != s_last_level) {
        gpio_set_level(STATUS_LED_GPIO, level);
        s_last_level = level;
    }
}

/* Non-blocking local haptic pulse overlay (never blocks UDP/audio/input) */
static int      s_haptic_pulses_left = 0;
static bool     s_haptic_on = false;
static uint16_t s_haptic_strength = 0;
static int64_t  s_haptic_next_us = 0;

static void haptic_start(int pulses, uint16_t strength) {
    if (pulses <= 0 || !g_game.handshake_done) return;
    s_haptic_pulses_left = pulses;
    s_haptic_strength = strength;
    s_haptic_on = true;
    s_haptic_next_us = esp_timer_get_time() + 75000; /* 75 ms ON */
    send_game_rumble(strength, strength);
}

static void haptic_tick(int64_t now_us) {
    if (s_haptic_pulses_left <= 0) return;
    if (now_us < s_haptic_next_us) return;
    if (s_haptic_on) {
        s_haptic_on = false;
        s_haptic_pulses_left--;
        if (g_game.handshake_done) send_game_rumble(0, 0);
        if (s_haptic_pulses_left > 0) {
            s_haptic_next_us = now_us + 75000; /* 75 ms gap */
        } else {
            g_rumble_dirty = true; /* Restore host rumble state */
        }
    } else {
        s_haptic_on = true;
        s_haptic_next_us = now_us + 75000;
        if (g_game.handshake_done) send_game_rumble(s_haptic_strength, s_haptic_strength);
    }
}

static void stadia_transport_task(void *arg) {
    status_led_init();
    stadia_opus_init();
    (void)arg;
    g_wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!tls_init_shared() || !link_init(&g_disp) || !link_init(&g_game)) {
        ESP_LOGE(TAG, "Transport init failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP STUN/DTLS listening on :%d (display) and :%d (game/rumble)",
             DISP_UDP_PORT, GAME_UDP_PORT);

    int maxfd = (g_disp.sock > g_game.sock ? g_disp.sock : g_game.sock) + 1;
    for (;;) {
        if (g_remote_updated) {
            g_remote_updated = false;
            link_reset(&g_disp);
            link_reset(&g_game);
            g_rpc_ready = false;
            g_start_game_sent = false;
            g_last_set = false;
            g_battery_pct = -1;
            g_batt_warn_15 = false;
            g_batt_warn_5 = false;
            g_headset_plugged = false;
            usb_gamepad_set_battery(-1);
            usb_gamepad_reset();
            snprintf(g_disp.ctrl_ufrag, sizeof(g_disp.ctrl_ufrag), "%s", g_pending_ufrag);
            snprintf(g_disp.ctrl_pwd,   sizeof(g_disp.ctrl_pwd),   "%s", g_pending_pwd);
            if (g_pending_cand_valid) {
                g_disp.peer = g_pending_cand;
                g_disp.peer_seen = true;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_disp.sock, &rfds);
        FD_SET(g_game.sock, &rfds);
        /* Event-driven: wakes instantly via UDP RX or g_wake_sock; tighter 15ms tick while connecting */
        struct timeval tv = { .tv_sec = 0, .tv_usec = g_rpc_ready ? 50000 : 15000 };
        int r = select(maxfd, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            if (FD_ISSET(g_disp.sock, &rfds)) service_socket(&g_disp);
            if (FD_ISSET(g_game.sock, &rfds)) service_socket(&g_game);
        }

        service_timers(&g_disp);
        service_timers(&g_game);

        /* Display RPC handshake: 40ms after DTLS, retry every 200ms until READY */
        if (g_disp.handshake_done && !g_rpc_ready) {
            int64_t now = esp_timer_get_time();
            if ((now - g_disp.dtls_done_us) >= 40000 &&
                (g_last_state_req_us == 0 || (now - g_last_state_req_us) >= 200000)) {
                g_last_state_req_us = now;
                send_get_controller_state_request();
            }
        }

        /* Game session rumble: non-blocking haptic overlay or host rumble */
        {
            int64_t now_r = esp_timer_get_time();
            haptic_tick(now_r);
            if (g_game.handshake_done && s_haptic_pulses_left <= 0 && g_battery_pct >= 0) {
                if (g_battery_pct <= 5 && !g_batt_warn_5) {
                    g_batt_warn_5 = true;
                    g_batt_warn_15 = true;
                    ESP_LOGW(TAG, "Controller battery CRITICAL (%d%%)", g_battery_pct);
                    haptic_start(5, 0xA000);
                } else if (g_battery_pct <= 15 && !g_batt_warn_15) {
                    g_batt_warn_15 = true;
                    ESP_LOGW(TAG, "Controller battery LOW (%d%%)", g_battery_pct);
                    haptic_start(3, 0x8000);
                }
            }
            if (g_game.handshake_done && s_haptic_pulses_left <= 0) {
                uint16_t rl = g_rumble_left_req;
                uint16_t rr = g_rumble_right_req;
                bool changed = g_rumble_dirty || rl != g_rumble_left_sent || rr != g_rumble_right_sent;
                bool refresh = (rl > 0 || rr > 0) && ((now_r - g_last_rumble_tx_us) >= 2000000);
                if (changed || refresh) {
                    g_rumble_dirty = false;
                    send_game_rumble(rl, rr);
                }
            }
        }

        /* Controller hotkeys & mode switching:
         *   Hold 2s: Assistant + Capture + A -> Stadia HID (1 buzz, fast deauth reboot)
         *   Hold 2s: Assistant + Capture + X -> XInput     (2 buzzes, fast deauth reboot)
         *   Hold 2s: Assistant + Capture + B -> Home Wi-Fi / LAN Auto-OTA (3 buzzes, reboot)
         *   Hold 2s: Assistant + Capture + Y -> USB ROM Bootloader Mode   (4 buzzes, reboot)
         *   Tap:     Assistant (XInput mode) OR Assistant + Capture (any mode) -> Mic mute toggle
         *   Tap:     Assistant + Capture + D-Pad Up / Down  -> Mic gain 1.0x / 1.5x / 2.0x / 2.5x
         *   Tap:     Assistant + Capture + D-Pad Right      -> Stick deadzone 0% / 4% / 8%
         *   Tap:     Assistant + Capture + D-Pad Left       -> Battery check (1-4 buzzes = quartile; soft buzz = unknown) */
        {
            const uint32_t btn_up     = (1u << 0);
            const uint32_t btn_down   = (1u << 1);
            const uint32_t btn_left   = (1u << 2);
            const uint32_t btn_right  = (1u << 3);
            const uint32_t btn_asst   = (1u << 10);
            const uint32_t btn_cap    = (1u << 11);
            const uint32_t btn_a      = (1u << 12);
            const uint32_t btn_b      = (1u << 13);
            const uint32_t btn_x      = (1u << 14);
            const uint32_t btn_y      = (1u << 15);
            const uint32_t face_mask  = btn_a | btn_b | btn_x | btn_y;
            const uint32_t combo_base   = btn_asst | btn_cap;
            const uint32_t combo_stadia = combo_base | btn_a;
            const uint32_t combo_xinput = combo_base | btn_x;
            const uint32_t combo_ota    = combo_base | btn_b;
            const uint32_t combo_boot   = combo_base | btn_y;

            static int64_t  s_combo_start_us = 0;
            static uint32_t s_combo_active = 0;
            static uint32_t s_prev_btn = 0;
            static int64_t  s_asst_down_us = 0;
            static bool     s_asst_chord_used = false;
            static bool     s_both_down = false;

            int64_t now = esp_timer_get_time();
            uint32_t cur = g_last_set ? g_last_buttons : 0;
            uint32_t rising = cur & ~s_prev_btn;

            /* Track Assistant press lifecycle so holding combos never triggers a mute tap */
            if (cur & btn_asst) {
                if (!(s_prev_btn & btn_asst)) {
                    s_asst_down_us = now;
                    s_asst_chord_used = false;
                    s_both_down = false;
                }
                if (cur & btn_cap) s_both_down = true;
                if (cur & ~(btn_asst | btn_cap)) s_asst_chord_used = true;
            } else if (s_prev_btn & btn_asst) {
                /* Assistant released: quick tap (<600ms) with no other action button = mic mute toggle */
                bool allow_mute = s_both_down || (usb_gamepad_get_mode() == USB_GAMEPAD_MODE_XINPUT);
                if (allow_mute && !s_asst_chord_used && (now - s_asst_down_us) < 600000) {
                    bool muted = !usb_audio_is_mic_muted();
                    usb_audio_set_mic_muted(muted);
                    ESP_LOGI(TAG, "Mic %s", muted ? "MUTED" : "UNMUTED");
                    haptic_start(muted ? 1 : 2, muted ? 0x6000 : 0x9000);
                }
                s_both_down = false;
                s_asst_chord_used = false;
            }

            /* Instant combo taps while holding Assistant + Capture */
            if ((cur & combo_base) == combo_base) {
                if (rising & (btn_up | btn_down)) {
                    static const uint16_t k_gains[] = { 256, 384, 512, 640 }; /* 1.0x, 1.5x, 2.0x, 2.5x */
                    uint16_t g = stadia_opus_get_mic_gain_q8();
                    int idx = 0;
                    for (int i = 0; i < 4; i++) {
                        if (g >= k_gains[i]) idx = i;
                    }
                    if ((rising & btn_up) && idx < 3) idx++;
                    else if ((rising & btn_down) && idx > 0) idx--;
                    stadia_opus_set_mic_gain_q8(k_gains[idx]);
                    ESP_LOGI(TAG, "Mic gain -> %u.%ux", k_gains[idx] / 256u, ((k_gains[idx] % 256u) * 10u) / 256u);
                    haptic_start(idx + 1, 0x7000);
                    s_asst_chord_used = true;
                } else if (rising & btn_right) {
                    static const uint16_t k_dz[] = { 0, 1310, 2621 }; /* 0%, 4%, 8% */
                    uint16_t dz = usb_gamepad_get_deadzone();
                    int idx = (dz == 0) ? 1 : (dz <= 1310 ? 2 : 0);
                    usb_gamepad_set_deadzone(k_dz[idx]);
                    ESP_LOGI(TAG, "Stick deadzone -> %u%%", (unsigned)(idx * 4));
                    haptic_start(idx + 1, 0x7000);
                    s_asst_chord_used = true;
                } else if (rising & btn_left) {
                    if (g_battery_pct < 0) {
                        ESP_LOGI(TAG, "Battery check: unknown (waiting for controller gauge)");
                        haptic_start(1, 0x3500);
                    } else {
                        int pulses = (g_battery_pct >= 76) ? 4 :
                                     (g_battery_pct >= 51) ? 3 :
                                     (g_battery_pct >= 26) ? 2 : 1;
                        ESP_LOGI(TAG, "Battery check: %d%% (%d pulses)", g_battery_pct, pulses);
                        haptic_start(pulses, 0x8000);
                    }
                    s_asst_chord_used = true;
                }
            }

            /* 2-second hold combos (USB mode switch, Home Wi-Fi Auto-OTA, USB Bootloader) */
            uint32_t matched = 0;
            if ((cur & combo_base) == combo_base) {
                uint32_t face = cur & face_mask;
                if (face == btn_a)      matched = combo_stadia;
                else if (face == btn_x) matched = combo_xinput;
                else if (face == btn_b) matched = combo_ota;
                else if (face == btn_y) matched = combo_boot;
            }

            if (matched != 0) {
                s_asst_chord_used = true;
                if (s_combo_active != matched) {
                    s_combo_active = matched;
                    s_combo_start_us = now;
                } else if ((now - s_combo_start_us) >= 2000000) {
                    if (matched == combo_ota) {
                        if (!ota_updater_schedule_sta_ota()) {
                            ESP_LOGW(TAG, "Auto-OTA combo ignored: configure Home Wi-Fi at http://192.168.4.1 first");
                            haptic_start(1, 0x3500);
                            s_combo_start_us = now + 5000000; /* debounce */
                        } else {
                            ESP_LOGI(TAG, "Auto-OTA combo triggered -> rebooting into Home Wi-Fi updater");
                            for (int p = 0; p < 3; p++) {
                                if (g_game.handshake_done) send_game_rumble(0x9000, 0x9000);
                                vTaskDelay(pdMS_TO_TICKS(100));
                                if (g_game.handshake_done) send_game_rumble(0, 0);
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }
                            ota_updater_reboot_clean(false);
                        }
                    } else if (matched == combo_boot) {
                        ESP_LOGI(TAG, "Bootloader combo triggered -> entering USB ROM Download Mode");
                        for (int p = 0; p < 4; p++) {
                            if (g_game.handshake_done) send_game_rumble(0xA000, 0xA000);
                            vTaskDelay(pdMS_TO_TICKS(80));
                            if (g_game.handshake_done) send_game_rumble(0, 0);
                            vTaskDelay(pdMS_TO_TICKS(80));
                        }
                        ota_updater_reboot_clean(true);
                    } else {
                        usb_gamepad_mode_t target = (matched == combo_xinput)
                                                    ? USB_GAMEPAD_MODE_XINPUT
                                                    : USB_GAMEPAD_MODE_STADIA_HID;
                        int pulses = (target == USB_GAMEPAD_MODE_XINPUT) ? 2 : 1;
                        ESP_LOGI(TAG, "Mode combo triggered -> switching USB mode to %s",
                                 (target == USB_GAMEPAD_MODE_XINPUT) ? "XInput" : "Stadia HID");
                        (void)usb_gamepad_save_mode(target);
                        for (int p = 0; p < pulses; p++) {
                            if (g_game.handshake_done) send_game_rumble(0xB000, 0xB000);
                            vTaskDelay(pdMS_TO_TICKS(150));
                            if (g_game.handshake_done) send_game_rumble(0, 0);
                            vTaskDelay(pdMS_TO_TICKS(150));
                        }
                        ota_updater_reboot_clean(false);
                    }
                }
            } else {
                s_combo_active = 0;
                s_combo_start_us = 0;
            }

            s_prev_btn = cur;
            bool low_batt = (g_battery_pct >= 0 && g_battery_pct <= 15);
            status_led_update(now, g_rpc_ready, usb_audio_is_mic_muted(), low_batt, s_combo_active != 0);
        }

        service_audio_tx();
        usb_gamepad_keepalive_tick();
    }
}

void stadia_transport_init(void) {
    xTaskCreatePinnedToCore(stadia_transport_task, "stadia_rtc", TRANSPORT_STACK, NULL, 6, NULL, 0);
}
