/* Phase 3-5: Display Link (UDP 50000+slot*4) + Game Session Rumble (UDP 50002+slot*4)
 * Supports 1..4 Concurrent Stadia Controllers. */
#include "stadia_transport.h"
#include "stadia_signaling.h"
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
#include "esp_attr.h"
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

#if CONFIG_SPIRAM
#define STADIA_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define STADIA_EXT_BSS
#endif

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
    int         slot;
    bool        is_game;
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

typedef struct {
    int         slot;
    dtls_link_t disp;
    dtls_link_t game;

    volatile bool remote_updated;
    char        pending_ufrag[64];
    char        pending_pwd[64];
    struct sockaddr_in pending_cand;
    bool        pending_cand_valid;

    bool        rpc_ready;
    bool        start_game_sent;
    bool        focus_game_sent;
    int64_t     last_state_req_us;
    uint32_t    rpc_ticket;

    volatile uint16_t rumble_left_req;
    volatile uint16_t rumble_right_req;
    volatile bool     rumble_dirty;
    uint16_t    rumble_left_sent;
    uint16_t    rumble_right_sent;
    int64_t     last_rumble_tx_us;
    uint64_t    game_tx_seq;
    uint64_t    game_last_rx_seq;
    uint64_t    game_last_ts_us;
    bool        game_use_rtp;
    uint8_t     game_rtp_pt;
    uint16_t    game_rtp_seq;
    char        game_session_id[64];
    uint32_t    game_pad_id;

    bool        last_set;
    uint32_t    last_buttons;
    int32_t     last_lx, last_ly, last_rx, last_ry;
    uint32_t    last_lt, last_rt;

    int         battery_pct;
    bool        batt_warn_15;
    bool        batt_warn_5;
    bool        headset_plugged;

    int         haptic_pulses_left;
    bool        haptic_on;
    uint16_t    haptic_strength;
    int64_t     haptic_next_us;

    int64_t     combo_start_us;
    uint32_t    combo_active;
    uint32_t    prev_btn;
    int64_t     asst_down_us;
    bool        asst_chord_used;
    bool        both_down;
} stadia_player_t;

/* Shared mbedTLS config/cert/RNG */
static mbedtls_ssl_config g_conf;
static mbedtls_x509_crt g_srv_cert;
static mbedtls_pk_context g_srv_key;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_entropy_context g_entropy;
static char g_cert_fp[112];
static bool g_tls_shared_init;

STADIA_EXT_BSS static stadia_player_t g_players[STADIA_MAX_PLAYERS];

/* Task-owned scratch buffers (off stack) */
STADIA_EXT_BSS static uint8_t g_udp_buf[RX_BUF_MAX];
STADIA_EXT_BSS static uint8_t g_app_buf[RX_BUF_MAX];
STADIA_EXT_BSS static uint8_t g_wire_tx[WIRE_HDR_LEN + TX_BUF_MAX];
STADIA_EXT_BSS static uint8_t g_rpc_tx[TX_BUF_MAX];

/* Phase 6A: SRTP bidirectional audio state (Player 1 when max_players == 1) */
static bool     g_audio_enabled;
static int64_t  g_audio_next_tx_us;
static uint16_t g_audio_seq;
static uint32_t g_audio_ts;
static uint32_t g_audio_tx_pkts;
static uint32_t g_mic_rx_pkts;
static uint32_t g_mic_rx_bytes;
static uint8_t  g_audio_tx_buf[512];

static void haptic_start(stadia_player_t *P, int pulses, uint16_t strength);

bool stadia_transport_is_slot_ready(int slot) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) return false;
    return g_players[slot].rpc_ready;
}

bool stadia_transport_is_ready(void) {
    for (int i = 0; i < STADIA_MAX_PLAYERS; i++) {
        if (g_players[i].rpc_ready) return true;
    }
    return false;
}

int stadia_transport_get_slot_battery_pct(int slot) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) return -1;
    return g_players[slot].battery_pct;
}

uint32_t stadia_transport_get_slot_peer_ip(int slot) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) return 0;
    if (g_players[slot].disp.peer_seen && g_players[slot].disp.peer.sin_addr.s_addr != 0) {
        return g_players[slot].disp.peer.sin_addr.s_addr;
    }
    if (g_players[slot].pending_cand_valid && g_players[slot].pending_cand.sin_addr.s_addr != 0) {
        return g_players[slot].pending_cand.sin_addr.s_addr;
    }
    return stadia_sessions[slot].peer_ip;
}

int stadia_transport_get_battery_pct(void) {
    return g_players[0].battery_pct;
}

bool stadia_transport_is_headset_plugged(void) {
    return g_players[0].headset_plugged;
}

int stadia_transport_get_connected_count(void) {
    int cnt = 0;
    int max_p = (int)usb_gamepad_get_max_players();
    if (max_p > STADIA_MAX_PLAYERS) max_p = STADIA_MAX_PLAYERS;
    for (int i = 0; i < max_p; i++) {
        if (g_players[i].rpc_ready) cnt++;
    }
    return cnt;
}

bool stadia_transport_is_slot_game_ready(int slot) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) return false;
    return g_players[slot].game.handshake_done;
}

void stadia_transport_wake(void);

void stadia_transport_set_remote_slot(int slot, const char *ice_ufrag, const char *ice_pwd,
                                      const char *cand_ip, uint16_t cand_port) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) slot = 0;
    stadia_player_t *P = &g_players[slot];
    if (ice_ufrag && ice_ufrag[0]) snprintf(P->pending_ufrag, sizeof(P->pending_ufrag), "%s", ice_ufrag);
    if (ice_pwd && ice_pwd[0])     snprintf(P->pending_pwd,   sizeof(P->pending_pwd),   "%s", ice_pwd);
    P->pending_cand_valid = false;
    if (cand_ip && cand_ip[0] && cand_port > 0) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(cand_port);
        if (inet_pton(AF_INET, cand_ip, &a.sin_addr) == 1) {
            P->pending_cand = a;
            P->pending_cand_valid = true;
        }
    }
    P->remote_updated = true;
    stadia_transport_wake();
}

void stadia_transport_set_remote(const char *ice_ufrag, const char *ice_pwd,
                                 const char *cand_ip, uint16_t cand_port) {
    stadia_transport_set_remote_slot(0, ice_ufrag, ice_pwd, cand_ip, cand_port);
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

void stadia_transport_set_rumble_slot(int slot, uint16_t left_strong, uint16_t right_weak) {
    if (slot < 0 || slot >= STADIA_MAX_PLAYERS) return;
    g_players[slot].rumble_left_req = left_strong;
    g_players[slot].rumble_right_req = right_weak;
    g_players[slot].rumble_dirty = true;
    stadia_transport_wake();
}

void stadia_transport_set_rumble(uint16_t left_strong, uint16_t right_weak) {
    stadia_transport_set_rumble_slot(0, left_strong, right_weak);
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
        ESP_LOGI(TAG, "[P%d:%s] STUN Binding Success sent", L->slot + 1, L->name);
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
    if (!L->is_game && L->slot == 0) {
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

static void send_get_controller_state_request(stadia_player_t *P) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_get_controller_state = true;
    dtc->get_controller_state.display_supports_send_log_report = false;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&P->disp, WIRE_ID_RPC_DISPLAY, ++P->rpc_ticket, buf, os.bytes_written);
    }
    free(dtc);
}

static void send_start_game_request(stadia_player_t *P) {
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
        link_send_rpc_request(&P->disp, WIRE_ID_RPC_DISPLAY, ++P->rpc_ticket, buf, os.bytes_written);
        P->start_game_sent = true;
        ESP_LOGI(TAG, "[P%d] Sent StartGame (key=%s) for rumble channel", P->slot + 1, GAME_KEY);
    }
    free(dtc);
}

static void send_set_audio_mode(stadia_player_t *P, uint32_t mode, bool f3) {
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
        link_send_rpc_request(&P->disp, WIRE_ID_RPC_DISPLAY, ++P->rpc_ticket, buf, os.bytes_written);
        ESP_LOGI(TAG, "[P%d] Sent SetAudioMode(mode=%lu, f3=%d)", P->slot + 1, (unsigned long)mode, f3 ? 1 : 0);
    }
    free(dtc);
}

static void send_set_microphone(stadia_player_t *P, bool enable) {
    gotham_display_DisplayToController *dtc = calloc(1, sizeof(*dtc));
    if (!dtc) return;
    dtc->has_set_microphone = true;
    dtc->set_microphone.has_value = true;
    dtc->set_microphone.value = enable;
    uint8_t buf[64];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&os, gotham_display_DisplayToController_fields, dtc)) {
        link_send_rpc_request(&P->disp, WIRE_ID_RPC_DISPLAY, ++P->rpc_ticket, buf, os.bytes_written);
        ESP_LOGI(TAG, "[P%d] Sent SetMicrophone(%d)", P->slot + 1, enable ? 1 : 0);
    }
    free(dtc);
}

static void service_audio_tx(void) {
    dtls_link_t *disp0 = &g_players[0].disp;
    if (usb_gamepad_get_max_players() > 1) return;
    if (!g_audio_enabled || !disp0->srtp.keys_ready || !disp0->peer_seen) return;

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
        if (stadia_srtp_protect(&disp0->srtp, g_audio_tx_buf, 12u + flen, &srtp_len)) {
            sendto(disp0->sock, g_audio_tx_buf, srtp_len, 0,
                   (const struct sockaddr *)&disp0->peer, sizeof(disp0->peer));
            g_audio_tx_pkts++;
        }
    }
}

/* ---------- Game Session: ProxyRequest -> ProxyResponse + Rumble TX ---------- */
static void handle_proxy_request_raw(stadia_player_t *P, uint32_t outer_ticket,
                                     const uint8_t *proxy_bytes, size_t proxy_len) {
    const uint8_t *body = NULL; size_t body_len = 0;
    if (!pbw_find_bytes(proxy_bytes, proxy_len, 4, &body, &body_len)) {
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        return;
    }

    gotham_rpc_RpcMessage *inner_req = calloc(1, sizeof(*inner_req));
    if (!inner_req) return;
    pb_istream_t is = pb_istream_from_buffer(body, body_len);
    if (!pb_decode(&is, gotham_rpc_RpcMessage_fields, inner_req) ||
        inner_req->which_payload != gotham_rpc_RpcMessage_request_tag ||
        !inner_req->payload.request.has_payload) {
        free(inner_req);
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
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
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, outer_ticket, gotham_rpc_StatusCode_OK, NULL, 0);
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
            snprintf(P->game.ctrl_ufrag, sizeof(P->game.ctrl_ufrag), "%.63s", offer->ice_ufrag);
            snprintf(P->game.ctrl_pwd,   sizeof(P->game.ctrl_pwd),   "%.63s", offer->ice_password);
            ESP_LOGI(TAG, "[P%d:GAME] Offer decoded: ufrag=%s raw_dtls=%d", P->slot + 1, P->game.ctrl_ufrag, (int)raw_dtls);
        }
        free(offer);
    }
    free(inner_req);

    link_reset(&P->game);
    P->game.peer_seen = false;
    P->game_tx_seq = 0;
    P->game_last_rx_seq = 0;
    P->game_last_ts_us = 0;
    P->game_use_rtp = false;
    P->game_rtp_pt = 0;
    P->focus_game_sent = false;

    /* Encode RtcSdpAnswer + RtcIceCandidate for Game Session (:50002 + slot*4) */
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
        ans->has_raw_dtls = true;           ans->raw_dtls = raw_dtls;
        ans->has_delayed_acks = true;       ans->delayed_acks = delayed_acks;
        ans->has_use_rtp_timestamps = true; ans->use_rtp_timestamps = use_rtp_ts;
        ans->has_disable_game_stats = true; ans->disable_game_stats = true;
        ans->has_disable_frame_tokens = true; ans->disable_frame_tokens = true;
        ans->has_network_type = true;
        ans->network_type = chrome_cloudcast_proto_api_common_RtcSdpNetworkType_NETWORK_TYPE_ETHERNET;

        cand->has_sdp_mid = true;           snprintf(cand->sdp_mid, sizeof(cand->sdp_mid), "0");
        cand->has_sdp_mline_index = true;   cand->sdp_mline_index = 0;
        cand->has_foundation = true;        snprintf(cand->foundation, sizeof(cand->foundation), "1");
        cand->has_priority = true;          cand->priority = (126u << 24) | (65535u << 8) | 255u;
        cand->has_local_endpoint_ip = true; snprintf(cand->local_endpoint_ip, sizeof(cand->local_endpoint_ip), "192.168.4.1");
        cand->has_local_endpoint_port = true; cand->local_endpoint_port = STADIA_SLOT_GAME_PORT(P->slot);

        pb_ostream_t aos = pb_ostream_from_buffer(ans_buf, 512);
        pb_ostream_t cos = pb_ostream_from_buffer(cand_buf, 256);
        if (pb_encode(&aos, chrome_cloudcast_proto_api_common_RtcSdpAnswer_fields, ans) &&
            pb_encode(&cos, chrome_cloudcast_proto_api_common_RtcIceCandidate_fields, cand)) {
            size_t il = pbw_put_bytes(init_resp, 896, 1, ans_buf, aos.bytes_written);
            il += pbw_put_bytes(init_resp + il, 896 - il, 2, cand_buf, cos.bytes_written);
            size_t sl = pbw_put_bytes(sresp, 960, 1, init_resp, il);
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
                uint8_t pr_buf[1056];
                size_t prl = pbw_put_bytes(pr_buf, sizeof(pr_buf), 1, inner_resp_buf, ros.bytes_written);
                size_t dtcl = pbw_put_bytes(dtc_buf, 1088, 1, pr_buf, prl);
                link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, outer_ticket,
                                       gotham_rpc_StatusCode_OK, dtc_buf, dtcl);
                ESP_LOGI(TAG, "[P%d:GAME] Sent ProxyResponse -> awaiting UDP :%u",
                         P->slot + 1, (unsigned)STADIA_SLOT_GAME_PORT(P->slot));
            }
        }
    }
    free(ans); free(cand); free(ans_buf); free(cand_buf);
    free(init_resp); free(sresp); free(inner_resp_buf); free(dtc_buf); free(inner_resp);
}

static void send_game_rumble(stadia_player_t *P, uint16_t left, uint16_t right) {
    if (!P->game.handshake_done) return;
    uint8_t vib[32], fb[48], slim[64], series[160], ipm[180];
    size_t vl = 0;
    if (P->game_pad_id > 0) vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 1, P->game_pad_id);
    if (left > 0)           vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 2, left);
    if (right > 0)          vl += pbw_put_u64(vib + vl, sizeof(vib) - vl, 3, right);
    size_t fbl = pbw_put_bytes(fb, sizeof(fb), 1, vib, vl);
    size_t sll = pbw_put_bytes(slim, sizeof(slim), 10, fb, fbl);
    size_t serl = 0;
    if (P->game_session_id[0]) {
        serl += pbw_put_bytes(series + serl, sizeof(series) - serl, 1,
                              (const uint8_t *)P->game_session_id, strlen(P->game_session_id));
    }
    serl += pbw_put_u64(series + serl, sizeof(series) - serl, 2, ++P->game_tx_seq);
    serl += pbw_put_u64(series + serl, sizeof(series) - serl, 3, (uint64_t)esp_timer_get_time());
    serl += pbw_put_bytes(series + serl, sizeof(series) - serl, 4, slim, sll);
    size_t ipml = pbw_put_bytes(ipm, sizeof(ipm), 7, series, serl);

    if (P->game_use_rtp) {
        uint8_t rtp[12 + 180];
        uint16_t seq = ++P->game_rtp_seq;
        rtp[0] = 0x80; rtp[1] = P->game_rtp_pt ? P->game_rtp_pt : 96;
        rtp[2] = (uint8_t)(seq >> 8); rtp[3] = (uint8_t)(seq & 0xFF);
        memset(rtp + 4, 0, 4);
        rtp[8] = 0x53; rtp[9] = 0x54; rtp[10] = 0x41; rtp[11] = 0x44;
        memcpy(rtp + 12, ipm, ipml);
        link_send_wire(&P->game, WIRE_ID_RTP, rtp, 12 + ipml);
    } else {
        link_send_wire(&P->game, WIRE_ID_RTP, ipm, ipml);
    }
    P->rumble_left_sent = left;
    P->rumble_right_sent = right;
    P->last_rumble_tx_us = esp_timer_get_time();
}

/* ---------- Input dispatch ---------- */
static void dispatch_input(stadia_player_t *P, uint32_t buttons,
                           int32_t lx, int32_t ly, int32_t rx, int32_t ry,
                           uint32_t lt, uint32_t rt) {
    if (P->last_set && buttons == P->last_buttons &&
        lx == P->last_lx && ly == P->last_ly &&
        rx == P->last_rx && ry == P->last_ry &&
        lt == P->last_lt && rt == P->last_rt) {
        return;
    }
    P->last_set = true;
    P->last_buttons = buttons;
    P->last_lx = lx; P->last_ly = ly;
    P->last_rx = rx; P->last_ry = ry;
    P->last_lt = lt; P->last_rt = rt;
    usb_gamepad_send_slot(P->slot, buttons, lx, ly, rx, ry, lt, rt);
}

/* ---------- Display & Game RPC handlers ---------- */
static void handle_get_current_time(stadia_player_t *P, uint32_t ticket,
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
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, ticket,
                               gotham_rpc_StatusCode_OK, buf, os.bytes_written);
    }
    free(dtc);
}

static void handle_disp_rpc_request(stadia_player_t *P, uint32_t ticket, const uint8_t *payload, size_t len) {
    const uint8_t *proxy = NULL; size_t proxy_len = 0;
    if (pbw_find_bytes(payload, len, 1, &proxy, &proxy_len)) {
        handle_proxy_request_raw(P, ticket, proxy, proxy_len);
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
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        break;
    case gotham_display_ControllerToDisplay_get_current_time_tag:
        handle_get_current_time(P, ticket, &ctd->variant.get_current_time);
        break;
    case gotham_display_ControllerToDisplay_disconnect_tag:
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, ticket, gotham_rpc_StatusCode_OK, NULL, 0);
        break;
    default:
        link_send_rpc_response(&P->disp, WIRE_ID_RPC_DISPLAY, ticket, 12 /* UNIMPLEMENTED */, NULL, 0);
        break;
    }
    free(ctd);
}

static void handle_battery_float(stadia_player_t *P, float raw, const char *src) {
    if (isnan(raw)) return;
    if (raw < 0.0f) raw = 0.0f;
    if (raw > 1.0f) raw = 1.0f;
    int pct = (int)lroundf(raw * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != P->battery_pct) {
        P->battery_pct = pct;
        ESP_LOGI(TAG, "[P%d] Controller battery (%s): %d%%", P->slot + 1, src, pct);
        usb_gamepad_set_battery_slot(P->slot, pct);
    }
    if (pct > 10) P->batt_warn_5 = false;
    if (pct > 20) P->batt_warn_15 = false;
}

static void handle_disp_rpc_notification(stadia_player_t *P, const uint8_t *payload, size_t len) {
    gotham_display_ControllerToDisplay *ctd = calloc(1, sizeof(*ctd));
    if (!ctd) return;
    pb_istream_t is = pb_istream_from_buffer(payload, len);
    if (pb_decode(&is, gotham_display_ControllerToDisplay_fields, ctd)) {
        if (ctd->which_variant == gotham_display_ControllerToDisplay_message3_tag) {
            const gotham_display_Message3 *m = &ctd->variant.message3;
            dispatch_input(P, m->f1, m->f2, m->f3, m->f4, m->f5, m->f6, m->f7);
        } else if (ctd->which_variant == gotham_display_ControllerToDisplay_notification10_tag &&
                   ctd->variant.notification10.has_value) {
            handle_battery_float(P, ctd->variant.notification10.value, "notif10");
        } else if (ctd->which_variant == gotham_display_ControllerToDisplay_audio_configuration_changed_tag &&
                   ctd->variant.audio_configuration_changed.has_mode) {
            uint32_t m = ctd->variant.audio_configuration_changed.mode;
            if (m == 0 || m == 1) {
                P->headset_plugged = (m == 1);
                if (P->slot == 0) usb_gamepad_set_headset(P->headset_plugged);
                ESP_LOGI(TAG, "[P%d] 3.5mm headset %s", P->slot + 1, P->headset_plugged ? "PLUGGED IN" : "UNPLUGGED");
            }
        }
    }
    free(ctd);
}

static void handle_disp_rpc_response(stadia_player_t *P, const gotham_rpc_RpcResponse *resp) {
    if (!resp->has_payload || resp->payload.size == 0) return;
    gotham_display_ControllerToDisplay *ctd = calloc(1, sizeof(*ctd));
    if (!ctd) return;
    pb_istream_t is = pb_istream_from_buffer(resp->payload.bytes, resp->payload.size);
    if (pb_decode(&is, gotham_display_ControllerToDisplay_fields, ctd) &&
        ctd->which_variant == gotham_display_ControllerToDisplay_controller_state_tag) {
        const gotham_display_GetControllerStateResponse *cs = &ctd->variant.controller_state;
        if (cs->has_f4) {
            P->headset_plugged = cs->f4;
            if (P->slot == 0) usb_gamepad_set_headset(P->headset_plugged);
        }
        if (cs->has_f10) {
            float f10_val = 0.0f;
            memcpy(&f10_val, &cs->f10, sizeof(f10_val));
            ESP_LOGI(TAG, "[P%d] ControllerState f10=0x%08lx (%.4f) f11=%lu",
                     P->slot + 1, (unsigned long)cs->f10, (double)f10_val,
                     (unsigned long)(cs->has_f11 ? cs->f11 : 0));
            if (!isnan(f10_val) && f10_val >= 0.005f && f10_val <= 1.0f) {
                handle_battery_float(P, f10_val, "state_f10");
            }
        }
        P->rpc_ready = true;
        ESP_LOGI(TAG, "[P%d] ===== READY! Starting game session for rumble =====", P->slot + 1);
        if (!P->start_game_sent) send_start_game_request(P);
        if (P->slot == 0 && usb_gamepad_get_max_players() == 1 && !g_audio_enabled) {
            send_set_audio_mode(P, 2, true);
            send_set_microphone(P, true);
            g_audio_enabled = true;
            g_audio_next_tx_us = 0;
        }
    }
    free(ctd);
}

static void handle_rpc(dtls_link_t *L, uint16_t rx_wire_id, const uint8_t *data, size_t len) {
    stadia_player_t *P = &g_players[L->slot];
    gotham_rpc_RpcMessage *rpc = calloc(1, sizeof(*rpc));
    if (!rpc) return;
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, gotham_rpc_RpcMessage_fields, rpc)) {
        free(rpc);
        return;
    }
    if (L->is_game) {
        if (rpc->which_payload == gotham_rpc_RpcMessage_request_tag) {
            link_send_rpc_response(&P->game, rx_wire_id, rpc->payload.request.ticket,
                                   gotham_rpc_StatusCode_OK, NULL, 0);
        }
        free(rpc);
        return;
    }
    switch (rpc->which_payload) {
    case gotham_rpc_RpcMessage_request_tag:
        if (rpc->payload.request.has_payload) {
            handle_disp_rpc_request(P, rpc->payload.request.ticket,
                                    rpc->payload.request.payload.bytes,
                                    rpc->payload.request.payload.size);
        }
        break;
    case gotham_rpc_RpcMessage_notification_tag:
        if (rpc->payload.notification.has_payload) {
            handle_disp_rpc_notification(P, rpc->payload.notification.payload.bytes,
                                         rpc->payload.notification.payload.size);
        }
        break;
    case gotham_rpc_RpcMessage_response_tag:
        handle_disp_rpc_response(P, &rpc->payload.response);
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
    bool     has_buttons;
    bool     has_lx, has_ly, has_rx, has_ry;
    bool     has_lt, has_rt;
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
            case 2:  out->buttons = (uint32_t)v; out->has_buttons = true; break;
            case 3:  out->lx = pbw_zigzag32((uint32_t)v); out->has_lx = true; break;
            case 4:  out->ly = pbw_zigzag32((uint32_t)v); out->has_ly = true; break;
            case 5:  out->rx = pbw_zigzag32((uint32_t)v); out->has_rx = true; break;
            case 6:  out->ry = pbw_zigzag32((uint32_t)v); out->has_ry = true; break;
            case 7:  out->lt = (uint32_t)v; out->has_lt = true; break;
            case 8:  out->rt = (uint32_t)v; out->has_rt = true; break;
            case 16: if (v != 0) out->is_reg = true; break;
            default: break;
            }
        } else if (wt == 1) {
            if (off + 8 > len) return false;
            off += 8;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pbw_read_varint(buf, len, &off, &flen) || off + flen > len) return false;
            if (fn == 18) out->is_reg = true;
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
    stadia_player_t *P = &g_players[L->slot];
    const uint8_t *pb_data = payload;
    size_t pb_len = len;
    bool was_rtp = false;

    size_t rtp_len = 0;
    uint8_t rx_pt = 96;
    const uint8_t *rtp_body = strip_rtp_header(payload, len, &rtp_len);
    if (rtp_body && rtp_len > 0) {
        rx_pt = payload[1] & 0x7Fu;
        if (rx_pt == MIC_PAYLOAD_TYPE) return;
        pb_data = rtp_body;
        pb_len = rtp_len;
        was_rtp = true;
    }

    const uint8_t *ser = NULL;
    size_t ser_len = 0;
    if (!pbw_find_bytes(pb_data, pb_len, 7, &ser, &ser_len)) {
        if (!was_rtp || !pbw_find_bytes(payload, len, 7, &ser, &ser_len)) return;
        was_rtp = false;
    }

    if (L->is_game) {
        P->game_use_rtp = was_rtp;
        if (was_rtp && rx_pt != 0) P->game_rtp_pt = rx_pt;
    }

    uint64_t seq_num = 0;
    bool have_seq = false;
    uint64_t base_ts_us = 0;
    uint64_t max_ts_us = P->game_last_ts_us;

    /* GamepadMessage_V2 is delta/sparse-encoded: omitted fields keep their previous value! */
    uint32_t cur_buttons = P->last_set ? P->last_buttons : 0;
    int32_t  cur_lx      = P->last_set ? P->last_lx : 0;
    int32_t  cur_ly      = P->last_set ? P->last_ly : 0;
    int32_t  cur_rx      = P->last_set ? P->last_rx : 0;
    int32_t  cur_ry      = P->last_set ? P->last_ry : 0;
    uint32_t cur_lt      = P->last_set ? P->last_lt : 0;
    uint32_t cur_rt      = P->last_set ? P->last_rt : 0;
    bool applied_any = false;

    /* First pass: grab sequence_number (2) and base_timestamp_us (3) before iterating messages (4) */
    {
        size_t p1 = 0;
        while (p1 < ser_len) {
            uint64_t tag = 0;
            if (!pbw_read_varint(ser, ser_len, &p1, &tag)) break;
            uint32_t fn = (uint32_t)(tag >> 3);
            uint32_t wt = (uint32_t)(tag & 7u);
            if (wt == 0) {
                uint64_t v = 0;
                if (!pbw_read_varint(ser, ser_len, &p1, &v)) break;
                if (fn == 2) { seq_num = v; have_seq = true; }
                else if (fn == 3) { base_ts_us = v; }
            } else if (wt == 1) {
                if (p1 + 8 > ser_len) break;
                p1 += 8;
            } else if (wt == 2) {
                uint64_t flen = 0;
                if (!pbw_read_varint(ser, ser_len, &p1, &flen) || p1 + flen > ser_len) break;
                p1 += (size_t)flen;
            } else if (wt == 5) {
                if (p1 + 4 > ser_len) break;
                p1 += 4;
            } else {
                break;
            }
        }
    }

    size_t off = 0;
    while (off < ser_len) {
        uint64_t tag = 0;
        if (!pbw_read_varint(ser, ser_len, &off, &tag)) break;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint32_t wt = (uint32_t)(tag & 7u);
        if (wt == 0) {
            uint64_t v = 0;
            if (!pbw_read_varint(ser, ser_len, &off, &v)) break;
        } else if (wt == 1) {
            if (off + 8 > ser_len) break;
            off += 8;
        } else if (wt == 2) {
            uint64_t flen = 0;
            if (!pbw_read_varint(ser, ser_len, &off, &flen) || off + flen > ser_len) break;
            const uint8_t *fptr = ser + off;
            off += (size_t)flen;
            if (fn == 1 && L->is_game && flen > 0) {
                size_t cpy = flen < sizeof(P->game_session_id) - 1 ? (size_t)flen : sizeof(P->game_session_id) - 1;
                memcpy(P->game_session_id, fptr, cpy);
                P->game_session_id[cpy] = '\0';
            } else if (fn == 4) {
                /* InputMessageSlim: field 4 = relative_time_us, field 5 = gamepad_message_v2 */
                const uint8_t *gp = NULL;
                size_t gp_len = 0;
                uint32_t rel_us = 0;
                size_t soff = 0;
                while (soff < (size_t)flen) {
                    uint64_t stag = 0;
                    if (!pbw_read_varint(fptr, (size_t)flen, &soff, &stag)) break;
                    uint32_t sfn = (uint32_t)(stag >> 3);
                    uint32_t swt = (uint32_t)(stag & 7u);
                    if (swt == 0) {
                        uint64_t sv = 0;
                        if (!pbw_read_varint(fptr, (size_t)flen, &soff, &sv)) break;
                        if (sfn == 4) rel_us = (uint32_t)sv;
                    } else if (swt == 1) {
                        if (soff + 8 > (size_t)flen) break;
                        soff += 8;
                    } else if (swt == 2) {
                        uint64_t sflen = 0;
                        if (!pbw_read_varint(fptr, (size_t)flen, &soff, &sflen) || soff + sflen > (size_t)flen) break;
                        if (sfn == 5) { gp = fptr + soff; gp_len = (size_t)sflen; }
                        soff += (size_t)sflen;
                    } else if (swt == 5) {
                        if (soff + 4 > (size_t)flen) break;
                        soff += 4;
                    } else {
                        break;
                    }
                }
                if (gp && gp_len > 0) {
                    fast_gamepad_v2_t g;
                    if (parse_gamepad_v2_fast(gp, gp_len, &g)) {
                        if (g.id > 0) P->game_pad_id = g.id;
                        if (!g.is_reg) {
                            uint64_t t_us = base_ts_us + (uint64_t)rel_us;
                            bool is_new = (base_ts_us > 0)
                                          ? (t_us > P->game_last_ts_us)
                                          : (!have_seq || seq_num > P->game_last_rx_seq);
                            if (is_new) {
                                if (g.has_buttons) cur_buttons = g.buttons;
                                if (g.has_lx)      cur_lx      = g.lx;
                                if (g.has_ly)      cur_ly      = g.ly;
                                if (g.has_rx)      cur_rx      = g.rx;
                                if (g.has_ry)      cur_ry      = g.ry;
                                if (g.has_lt)      cur_lt      = g.lt;
                                if (g.has_rt)      cur_rt      = g.rt;
                                if (t_us > max_ts_us) max_ts_us = t_us;
                                applied_any = true;
                            }
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

    if (have_seq && seq_num > P->game_last_rx_seq) {
        P->game_last_rx_seq = seq_num;
    }
    if (max_ts_us > P->game_last_ts_us) {
        P->game_last_ts_us = max_ts_us;
    }

    if (applied_any) {
        dispatch_input(P, cur_buttons, cur_lx, cur_ly, cur_rx, cur_ry, cur_lt, cur_rt);
    }

    if (have_seq) {
        uint8_t ack[16];
        size_t al = pbw_put_u64(ack, sizeof(ack), 4, seq_num);
        if (was_rtp) {
            uint8_t rtp[12 + 16];
            uint16_t seq = ++P->game_rtp_seq;
            uint32_t ts = (uint32_t)((esp_timer_get_time() / 1000LL) * 90LL);
            rtp[0] = 0x80; rtp[1] = P->game_rtp_pt ? P->game_rtp_pt : 96;
            rtp[2] = (uint8_t)(seq >> 8); rtp[3] = (uint8_t)(seq & 0xFF);
            rtp[4] = (uint8_t)(ts >> 24); rtp[5] = (uint8_t)(ts >> 16);
            rtp[6] = (uint8_t)(ts >> 8);  rtp[7] = (uint8_t)(ts & 0xFF);
            rtp[8] = 0x53; rtp[9] = 0x54; rtp[10] = 0x41; rtp[11] = 0x44;
            memcpy(rtp + 12, ack, al);
            link_send_wire(L, WIRE_ID_RTP, rtp, 12 + al);
        } else {
            link_send_wire(L, WIRE_ID_RTP, ack, al);
        }
    }
}

static void handle_app_data(dtls_link_t *L, const uint8_t *data, size_t len) {
    if (len < WIRE_HDR_LEN || (data[0] >> 4) != 0) return;
    uint16_t id = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    uint32_t total = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16);
    if (total != len) return;

    if (id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY ||
        (L->is_game && id == WIRE_ID_RPC_GAME)) {
        handle_rpc(L, id, data + WIRE_HDR_LEN, len - WIRE_HDR_LEN);
    } else if (id == WIRE_ID_RTP) {
        handle_0x66_input(L, data + WIRE_HDR_LEN, len - WIRE_HDR_LEN);
    }
}

static void pump_dtls(dtls_link_t *L) {
    stadia_player_t *P = &g_players[L->slot];
    if (!L->handshake_done) {
        int rc = mbedtls_ssl_handshake(&L->ssl);
        if (rc == 0) {
            L->handshake_done = true;
            L->dtls_done_us = esp_timer_get_time();
            if (!L->is_game) {
                P->rpc_ready = false;
                P->start_game_sent = false;
                P->focus_game_sent = false;
                P->last_state_req_us = 0;
            } else {
                P->focus_game_sent = false;
                P->game_last_rx_seq = 0;
                P->game_last_ts_us = 0;
                P->rumble_dirty = true;
            }
            ESP_LOGI(TAG, "[P%d:%s] DTLS handshake complete!", L->slot + 1, L->name);
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
        if (!L->is_game) {
            P->rpc_ready = false;
            P->start_game_sent = false;
            P->focus_game_sent = false;
            P->last_set = false;
            P->battery_pct = -1;
            P->batt_warn_15 = false;
            P->batt_warn_5 = false;
            P->headset_plugged = false;
            if (P->slot == 0) usb_gamepad_set_headset(false);
            usb_gamepad_set_battery_slot(P->slot, -1);
            usb_gamepad_reset_slot(P->slot);
            link_reset(&P->game);
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
        } else if (b0 >= 128 && b0 <= 191 && !L->is_game && L->slot == 0 &&
                   usb_gamepad_get_max_players() == 1 && L->srtp.keys_ready) {
            uint8_t pt = g_udp_buf[1] & 0x7Fu;
            if (pt < 64 || pt > 95) {
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
        on = ((now_us / 100000) & 1) == 0;
    } else if (!ready) {
        on = (now_us % 1000000) < 100000;
    } else if (mic_muted) {
        on = ((now_us / 500000) & 1) == 0;
    } else if (low_batt) {
        int64_t t = now_us % 2000000;
        on = !((t < 120000) || (t >= 240000 && t < 360000));
    } else {
        on = true;
    }
    int level = on ? 0 : 1;
    if (level != s_last_level) {
        gpio_set_level(STATUS_LED_GPIO, level);
        s_last_level = level;
    }
}

static void haptic_start(stadia_player_t *P, int pulses, uint16_t strength) {
    if (pulses <= 0 || !P->game.handshake_done) return;
    P->haptic_pulses_left = pulses;
    P->haptic_strength = strength;
    P->haptic_on = true;
    P->haptic_next_us = esp_timer_get_time() + 75000;
    send_game_rumble(P, strength, strength);
}

static void haptic_tick(stadia_player_t *P, int64_t now_us) {
    if (P->haptic_pulses_left <= 0) return;
    if (now_us < P->haptic_next_us) return;
    if (P->haptic_on) {
        P->haptic_on = false;
        P->haptic_pulses_left--;
        if (P->game.handshake_done) send_game_rumble(P, 0, 0);
        if (P->haptic_pulses_left > 0) {
            P->haptic_next_us = now_us + 75000;
        } else {
            P->rumble_dirty = true;
        }
    } else {
        P->haptic_on = true;
        P->haptic_next_us = now_us + 75000;
        if (P->game.handshake_done) send_game_rumble(P, P->haptic_strength, P->haptic_strength);
    }
}

static void stadia_transport_task(void *arg) {
    status_led_init();
    int max_p = (int)usb_gamepad_get_max_players();
    if (max_p < 1) max_p = 1;
    if (max_p > STADIA_MAX_PLAYERS) max_p = STADIA_MAX_PLAYERS;
    if (max_p == 1) {
        stadia_opus_init();
    }
    (void)arg;
    g_wake_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!tls_init_shared()) {
        ESP_LOGE(TAG, "TLS shared init failed");
        vTaskDelete(NULL);
        return;
    }

    int maxfd = -1;
    for (int slot = 0; slot < max_p; slot++) {
        stadia_player_t *P = &g_players[slot];
        memset(P, 0, sizeof(*P));
        P->slot = slot;
        P->battery_pct = -1;

        P->disp.name = "DISP";
        P->disp.slot = slot;
        P->disp.is_game = false;
        P->disp.local_port = STADIA_SLOT_DISP_PORT(slot);
        P->disp.our_ufrag = DISP_ICE_UFRAG;
        P->disp.our_pwd = DISP_ICE_PWD;
        P->disp.sock = -1;

        P->game.name = "GAME";
        P->game.slot = slot;
        P->game.is_game = true;
        P->game.local_port = STADIA_SLOT_GAME_PORT(slot);
        P->game.our_ufrag = GAME_ICE_UFRAG;
        P->game.our_pwd = GAME_ICE_PWD;
        P->game.sock = -1;

        if (!link_init(&P->disp) || !link_init(&P->game)) {
            ESP_LOGE(TAG, "[P%d] Transport link_init failed", slot + 1);
            vTaskDelete(NULL);
            return;
        }
        if (P->disp.sock > maxfd) maxfd = P->disp.sock;
        if (P->game.sock > maxfd) maxfd = P->game.sock;
        ESP_LOGI(TAG, "[P%d] UDP STUN/DTLS listening on :%u (display) and :%u (game/rumble)",
                 slot + 1, (unsigned)P->disp.local_port, (unsigned)P->game.local_port);
    }

    for (;;) {
        for (int slot = 0; slot < max_p; slot++) {
            stadia_player_t *P = &g_players[slot];
            if (P->remote_updated) {
                P->remote_updated = false;
                link_reset(&P->disp);
                link_reset(&P->game);
                P->rpc_ready = false;
                P->start_game_sent = false;
                P->focus_game_sent = false;
                P->last_set = false;
                P->battery_pct = -1;
                P->batt_warn_15 = false;
                P->batt_warn_5 = false;
                P->headset_plugged = false;
                if (slot == 0) usb_gamepad_set_headset(false);
                usb_gamepad_set_battery_slot(slot, -1);
                usb_gamepad_reset_slot(slot);
                snprintf(P->disp.ctrl_ufrag, sizeof(P->disp.ctrl_ufrag), "%s", P->pending_ufrag);
                snprintf(P->disp.ctrl_pwd,   sizeof(P->disp.ctrl_pwd),   "%s", P->pending_pwd);
                if (P->pending_cand_valid) {
                    P->disp.peer = P->pending_cand;
                    P->disp.peer_seen = true;
                }
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        bool any_connecting = false;
        for (int slot = 0; slot < max_p; slot++) {
            stadia_player_t *P = &g_players[slot];
            FD_SET(P->disp.sock, &rfds);
            FD_SET(P->game.sock, &rfds);
            if (!P->rpc_ready) any_connecting = true;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = any_connecting ? 15000 : 50000 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            for (int slot = 0; slot < max_p; slot++) {
                stadia_player_t *P = &g_players[slot];
                if (FD_ISSET(P->disp.sock, &rfds)) service_socket(&P->disp);
                if (FD_ISSET(P->game.sock, &rfds)) service_socket(&P->game);
            }
        }

        int64_t now = esp_timer_get_time();
        bool any_ready = false;
        bool any_low_batt = false;
        bool any_combo_held = false;

        for (int slot = 0; slot < max_p; slot++) {
            stadia_player_t *P = &g_players[slot];
            service_timers(&P->disp);
            service_timers(&P->game);

            if (P->disp.handshake_done && !P->rpc_ready) {
                if ((now - P->disp.dtls_done_us) >= 40000 &&
                    (P->last_state_req_us == 0 || (now - P->last_state_req_us) >= 200000)) {
                    P->last_state_req_us = now;
                    send_get_controller_state_request(P);
                }
            }
            if (P->game.handshake_done && !P->focus_game_sent && P->disp.handshake_done &&
                (now - P->game.dtls_done_us) >= 150000) {
                P->focus_game_sent = true;
                /* DisplayToController { focus_game(4) = Empty{} } -> switches controller from 20 Hz menu to 125 Hz game input */
                static const uint8_t fg[2] = { 0x22, 0x00 };
                link_send_rpc_request(&P->disp, WIRE_ID_RPC_DISPLAY, ++P->rpc_ticket, fg, sizeof(fg));
                ESP_LOGI(TAG, "[P%d] Auto-sent focus_game (125 Hz Game Session input enabled)", slot + 1);
            }

            haptic_tick(P, now);
            if (P->game.handshake_done && P->haptic_pulses_left <= 0 && P->battery_pct >= 0) {
                if (P->battery_pct <= 5 && !P->batt_warn_5) {
                    P->batt_warn_5 = true;
                    P->batt_warn_15 = true;
                    ESP_LOGW(TAG, "[P%d] Controller battery CRITICAL (%d%%)", slot + 1, P->battery_pct);
                    haptic_start(P, 5, 0xA000);
                } else if (P->battery_pct <= 15 && !P->batt_warn_15) {
                    P->batt_warn_15 = true;
                    ESP_LOGW(TAG, "[P%d] Controller battery LOW (%d%%)", slot + 1, P->battery_pct);
                    haptic_start(P, 3, 0x8000);
                }
            }
            if (P->game.handshake_done && P->haptic_pulses_left <= 0) {
                uint16_t rl = P->rumble_left_req;
                uint16_t rr = P->rumble_right_req;
                bool changed = P->rumble_dirty || rl != P->rumble_left_sent || rr != P->rumble_right_sent;
                bool refresh = (rl > 0 || rr > 0) && ((now - P->last_rumble_tx_us) >= 2000000);
                if (changed || refresh) {
                    P->rumble_dirty = false;
                    send_game_rumble(P, rl, rr);
                }
            }

            /* Controller hotkeys & mode switching (per player) */
            const uint32_t btn_up     = (1u << 0);
            const uint32_t btn_down   = (1u << 1);
            const uint32_t btn_left   = (1u << 2);
            const uint32_t btn_right  = (1u << 3);
            const uint32_t btn_opt    = (1u << 5);
            const uint32_t btn_l1     = (1u << 8);
            const uint32_t btn_r1     = (1u << 9);
            const uint32_t btn_asst   = (1u << 10);
            const uint32_t btn_cap    = (1u << 11);
            const uint32_t btn_a      = (1u << 12);
            const uint32_t btn_b      = (1u << 13);
            const uint32_t btn_x      = (1u << 14);
            const uint32_t btn_y      = (1u << 15);
            const uint32_t hold_mask  = btn_a | btn_b | btn_x | btn_y | btn_l1 | btn_r1 | btn_opt;
            const uint32_t combo_base   = btn_asst | btn_cap;
            const uint32_t combo_stadia = combo_base | btn_a;
            const uint32_t combo_xinput = combo_base | btn_x;
            const uint32_t combo_ds4    = combo_base | btn_l1;
            const uint32_t combo_switch = combo_base | btn_r1;
            const uint32_t combo_ps3    = combo_base | btn_opt;
            const uint32_t combo_ota    = combo_base | btn_b;
            const uint32_t combo_boot   = combo_base | btn_y;

            uint32_t cur = P->last_set ? P->last_buttons : 0;
            uint32_t rising = cur & ~P->prev_btn;

            if (cur & btn_asst) {
                if (!(P->prev_btn & btn_asst)) {
                    P->asst_down_us = now;
                    P->asst_chord_used = false;
                    P->both_down = false;
                }
                if (cur & btn_cap) P->both_down = true;
                if (cur & ~(btn_asst | btn_cap)) P->asst_chord_used = true;
            } else if (P->prev_btn & btn_asst) {
                usb_gamepad_mode_t cur_mode = usb_gamepad_get_mode();
                bool allow_mute = P->both_down || (cur_mode == USB_GAMEPAD_MODE_XINPUT || cur_mode == USB_GAMEPAD_MODE_DS4);
                if (allow_mute && !P->asst_chord_used && (now - P->asst_down_us) < 600000) {
                    bool muted = !usb_audio_is_mic_muted();
                    usb_audio_set_mic_muted(muted);
                    ESP_LOGI(TAG, "Mic %s", muted ? "MUTED" : "UNMUTED");
                    haptic_start(P, muted ? 1 : 2, muted ? 0x6000 : 0x9000);
                }
                P->both_down = false;
                P->asst_chord_used = false;
            }

            if ((cur & combo_base) == combo_base) {
                if (rising & (btn_up | btn_down)) {
                    static const uint16_t k_gains[] = { 256, 384, 512, 640 };
                    uint16_t g = stadia_opus_get_mic_gain_q8();
                    int idx = 0;
                    for (int i = 0; i < 4; i++) {
                        if (g >= k_gains[i]) idx = i;
                    }
                    if ((rising & btn_up) && idx < 3) idx++;
                    else if ((rising & btn_down) && idx > 0) idx--;
                    stadia_opus_set_mic_gain_q8(k_gains[idx]);
                    ESP_LOGI(TAG, "Mic gain -> %u.%ux", k_gains[idx] / 256u, ((k_gains[idx] % 256u) * 10u) / 256u);
                    haptic_start(P, idx + 1, 0x7000);
                    P->asst_chord_used = true;
                } else if (rising & btn_right) {
                    static const uint16_t k_dz[] = { 0, 1310, 2621 };
                    uint16_t dz = usb_gamepad_get_deadzone();
                    int idx = (dz == 0) ? 1 : (dz <= 1310 ? 2 : 0);
                    usb_gamepad_set_deadzone(k_dz[idx]);
                    ESP_LOGI(TAG, "Stick deadzone -> %u%%", (unsigned)(idx * 4));
                    haptic_start(P, idx + 1, 0x7000);
                    P->asst_chord_used = true;
                } else if (rising & btn_left) {
                    if (P->battery_pct < 0) {
                        ESP_LOGI(TAG, "[P%d] Battery check: unknown (waiting for controller gauge)", slot + 1);
                        haptic_start(P, 1, 0x3500);
                    } else {
                        int pulses = (P->battery_pct >= 76) ? 4 :
                                     (P->battery_pct >= 51) ? 3 :
                                     (P->battery_pct >= 26) ? 2 : 1;
                        ESP_LOGI(TAG, "[P%d] Battery check: %d%% (%d pulses)", slot + 1, P->battery_pct, pulses);
                        haptic_start(P, pulses, 0x8000);
                    }
                    P->asst_chord_used = true;
                }
            }

            uint32_t matched = 0;
            if ((cur & combo_base) == combo_base) {
                uint32_t hold = cur & hold_mask;
                if (hold == btn_a)        matched = combo_stadia;
                else if (hold == btn_x)   matched = combo_xinput;
                else if (hold == btn_l1)  matched = combo_ds4;
                else if (hold == btn_r1)  matched = combo_switch;
                else if (hold == btn_opt) matched = combo_ps3;
                else if (hold == btn_b)   matched = combo_ota;
                else if (hold == btn_y)   matched = combo_boot;
            }

            if (matched != 0) {
                P->asst_chord_used = true;
                if (P->combo_active != matched) {
                    P->combo_active = matched;
                    P->combo_start_us = now;
                } else if ((now - P->combo_start_us) >= 2000000) {
                    if (matched == combo_ota) {
                        if (!ota_updater_schedule_sta_ota()) {
                            ESP_LOGW(TAG, "Auto-OTA combo ignored: configure Home Wi-Fi at http://192.168.4.1 first");
                            haptic_start(P, 1, 0x3500);
                            P->combo_start_us = now + 5000000;
                        } else {
                            ESP_LOGI(TAG, "Auto-OTA combo triggered -> rebooting into Home Wi-Fi updater");
                            for (int p = 0; p < 3; p++) {
                                if (P->game.handshake_done) send_game_rumble(P, 0x9000, 0x9000);
                                vTaskDelay(pdMS_TO_TICKS(100));
                                if (P->game.handshake_done) send_game_rumble(P, 0, 0);
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }
                            ota_updater_reboot_clean(false);
                        }
                    } else if (matched == combo_boot) {
                        ESP_LOGI(TAG, "Bootloader combo triggered -> entering USB ROM Download Mode");
                        for (int p = 0; p < 4; p++) {
                            if (P->game.handshake_done) send_game_rumble(P, 0xA000, 0xA000);
                            vTaskDelay(pdMS_TO_TICKS(80));
                            if (P->game.handshake_done) send_game_rumble(P, 0, 0);
                            vTaskDelay(pdMS_TO_TICKS(80));
                        }
                        ota_updater_reboot_clean(true);
                    } else {
                        usb_gamepad_mode_t target = USB_GAMEPAD_MODE_STADIA_HID;
                        int pulses = 1;
                        if (matched == combo_xinput)      { target = USB_GAMEPAD_MODE_XINPUT;     pulses = 2; }
                        else if (matched == combo_ds4)    { target = USB_GAMEPAD_MODE_DS4;        pulses = 2; }
                        else if (matched == combo_switch) { target = USB_GAMEPAD_MODE_SWITCH_PRO; pulses = 3; }
                        else if (matched == combo_ps3)    { target = USB_GAMEPAD_MODE_PS3;        pulses = 4; }
                        ESP_LOGI(TAG, "[P%d] Mode combo triggered -> switching USB mode to %s",
                                 slot + 1, usb_gamepad_mode_name(target));
                        (void)usb_gamepad_save_mode(target);
                        for (int p = 0; p < pulses; p++) {
                            if (P->game.handshake_done) send_game_rumble(P, 0xB000, 0xB000);
                            vTaskDelay(pdMS_TO_TICKS(150));
                            if (P->game.handshake_done) send_game_rumble(P, 0, 0);
                            vTaskDelay(pdMS_TO_TICKS(150));
                        }
                        ota_updater_reboot_clean(false);
                    }
                }
            } else {
                P->combo_active = 0;
                P->combo_start_us = 0;
            }

            P->prev_btn = cur;
            if (P->rpc_ready) any_ready = true;
            if (P->battery_pct >= 0 && P->battery_pct <= 15) any_low_batt = true;
            if (P->combo_active != 0) any_combo_held = true;
        }

        status_led_update(now, any_ready, usb_audio_is_mic_muted(), any_low_batt, any_combo_held);
        service_audio_tx();
        usb_gamepad_keepalive_tick();
    }
}

void stadia_transport_init(void) {
    xTaskCreatePinnedToCore(stadia_transport_task, "stadia_rtc", TRANSPORT_STACK, NULL, 6, NULL, 0);
}
