/* Minimal RFC 3711 SRTP_AES128_CM_SHA1_80 + DTLS-SRTP key exporter (mbedTLS HW AES/SHA1). */
#include "stadia_srtp.h"

#include <string.h>
#include "esp_log.h"

#define TAG "SRTP"

static void srtp_kdf_label(const uint8_t master_key[16],
                           const uint8_t master_salt[14],
                           uint8_t label,
                           uint8_t *out, size_t out_len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, master_key, 128);

    uint8_t iv[16];
    memcpy(iv, master_salt, 14);
    iv[14] = 0;
    iv[15] = 0;
    iv[7] ^= label; /* RFC 3711 4.3.1 (kdr = 0) */

    uint8_t zeros[32] = {0};
    uint8_t stream_block[16] = {0};
    size_t nc_off = 0;
    mbedtls_aes_crypt_ctr(&aes, out_len, &nc_off, iv, stream_block, zeros, out);
    mbedtls_aes_free(&aes);
}

static void srtp_dir_free(srtp_dir_t *d) {
    if (d->ready) mbedtls_aes_free(&d->aes);
    if (d->hmac_ready) mbedtls_md_free(&d->hmac_ctx);
    memset(d, 0, sizeof(*d));
}

static void srtp_dir_setup(srtp_dir_t *d, const uint8_t mkey[16], const uint8_t msalt[14]) {
    srtp_dir_free(d);
    srtp_kdf_label(mkey, msalt, 0x00, d->enc_key, 16);
    srtp_kdf_label(mkey, msalt, 0x01, d->auth_key, 20);
    srtp_kdf_label(mkey, msalt, 0x02, d->salt, 14);
    mbedtls_aes_init(&d->aes);
    mbedtls_aes_setkey_enc(&d->aes, d->enc_key, 128);

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md) {
        mbedtls_md_init(&d->hmac_ctx);
        if (mbedtls_md_setup(&d->hmac_ctx, md, 1) == 0 &&
            mbedtls_md_hmac_starts(&d->hmac_ctx, d->auth_key, 20) == 0) {
            d->hmac_ready = true;
        } else {
            mbedtls_md_free(&d->hmac_ctx);
        }
    }
    d->ready = true;
}

void stadia_srtp_init(stadia_srtp_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

void stadia_srtp_free(stadia_srtp_ctx_t *ctx) {
    srtp_dir_free(&ctx->tx);
    srtp_dir_free(&ctx->rx);
    ctx->keys_ready = false;
}

void stadia_srtp_export_cb(void *p_expkey,
                           mbedtls_ssl_key_export_type type,
                           const unsigned char *secret,
                           size_t secret_len,
                           const unsigned char client_random[32],
                           const unsigned char server_random[32],
                           mbedtls_tls_prf_types tls_prf_type) {
    stadia_srtp_ctx_t *ctx = (stadia_srtp_ctx_t *)p_expkey;
    if (!ctx || type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET) return;

    uint8_t randbytes[64];
    memcpy(randbytes, client_random, 32);
    memcpy(randbytes + 32, server_random, 32);

    /* RFC 5764 SRTP_AES128_CM_SHA1_80: 2*(16 key + 14 salt) = 60 bytes */
    uint8_t kb[60];
    int rc = mbedtls_ssl_tls_prf(tls_prf_type, secret, secret_len,
                                 "EXTRACTOR-dtls_srtp", randbytes, sizeof(randbytes),
                                 kb, sizeof(kb));
    if (rc != 0) {
        ESP_LOGE(TAG, "EXTRACTOR-dtls_srtp PRF failed: -0x%04x", -rc);
        return;
    }

    /* ESP32 is DTLS server: client_write = RX (mic), server_write = TX (headset) */
    srtp_dir_setup(&ctx->rx, kb + 0,  kb + 32);
    srtp_dir_setup(&ctx->tx, kb + 16, kb + 46);
    ctx->keys_ready = true;
    ESP_LOGI(TAG, "DTLS-SRTP keys derived (AES128_CM_SHA1_80, persistent HMAC)");
}

static size_t rtp_header_len(const uint8_t *pkt, size_t len) {
    if (len < 12 || (pkt[0] >> 6) != 2) return 0;
    size_t hlen = 12 + 4u * (pkt[0] & 0x0Fu);
    if (hlen > len) return 0;
    if (pkt[0] & 0x10u) { /* RTP header extension */
        if (hlen + 4 > len) return 0;
        uint16_t ext_words = (uint16_t)((pkt[hlen + 2] << 8) | pkt[hlen + 3]);
        hlen += 4u + 4u * ext_words;
        if (hlen > len) return 0;
    }
    return hlen;
}

static void srtp_build_iv(const uint8_t salt[14], uint32_t ssrc,
                          uint32_t roc, uint16_t seq, uint8_t iv[16]) {
    memcpy(iv, salt, 14);
    iv[14] = 0;
    iv[15] = 0;
    iv[4]  ^= (uint8_t)(ssrc >> 24);
    iv[5]  ^= (uint8_t)(ssrc >> 16);
    iv[6]  ^= (uint8_t)(ssrc >> 8);
    iv[7]  ^= (uint8_t)(ssrc);
    iv[8]  ^= (uint8_t)(roc >> 24);
    iv[9]  ^= (uint8_t)(roc >> 16);
    iv[10] ^= (uint8_t)(roc >> 8);
    iv[11] ^= (uint8_t)(roc);
    iv[12] ^= (uint8_t)(seq >> 8);
    iv[13] ^= (uint8_t)(seq);
}

/* Zero-alloc HMAC-SHA1-80 reusing pre-keyed persistent mbedtls_md_context_t */
static bool srtp_hmac_sha1_80(srtp_dir_t *d,
                              const uint8_t *pkt, size_t pkt_len,
                              uint32_t roc, uint8_t tag_out[STADIA_SRTP_TAG_LEN]) {
    if (!d->hmac_ready) return false;
    uint8_t roc_be[4] = {
        (uint8_t)(roc >> 24), (uint8_t)(roc >> 16),
        (uint8_t)(roc >> 8),  (uint8_t)(roc)
    };
    uint8_t full_mac[20];
    if (mbedtls_md_hmac_reset(&d->hmac_ctx) != 0) return false;
    mbedtls_md_hmac_update(&d->hmac_ctx, pkt, pkt_len);
    mbedtls_md_hmac_update(&d->hmac_ctx, roc_be, 4);
    mbedtls_md_hmac_finish(&d->hmac_ctx, full_mac);
    memcpy(tag_out, full_mac, STADIA_SRTP_TAG_LEN);
    return true;
}

static uint32_t estimate_roc(const srtp_dir_t *d, uint16_t seq) {
    if (!d->has_seq) return d->roc;
    if (d->s_l < 32768) {
        return (seq - d->s_l > 32768) ? (d->roc - 1u) : d->roc;
    }
    return (d->s_l - 32768 > seq) ? (d->roc + 1u) : d->roc;
}

static void update_roc(srtp_dir_t *d, uint16_t seq, uint32_t v_roc) {
    if (!d->has_seq) {
        d->s_l = seq;
        d->has_seq = true;
        return;
    }
    if (v_roc == d->roc && seq > d->s_l) {
        d->s_l = seq;
    } else if (v_roc == d->roc + 1u) {
        d->roc = v_roc;
        d->s_l = seq;
    }
}

bool stadia_srtp_protect(stadia_srtp_ctx_t *ctx, uint8_t *buf, size_t rtp_len, size_t *out_len) {
    if (!ctx || !ctx->keys_ready || !ctx->tx.ready) return false;
    size_t hlen = rtp_header_len(buf, rtp_len);
    if (hlen == 0) return false;

    uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
    uint32_t ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                    ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
    uint32_t roc = estimate_roc(&ctx->tx, seq);

    uint8_t iv[16], sb[16] = {0};
    size_t nc_off = 0;
    srtp_build_iv(ctx->tx.salt, ssrc, roc, seq, iv);
    size_t plen = rtp_len - hlen;
    if (plen > 0) {
        mbedtls_aes_crypt_ctr(&ctx->tx.aes, plen, &nc_off, iv, sb, buf + hlen, buf + hlen);
    }

    if (!srtp_hmac_sha1_80(&ctx->tx, buf, rtp_len, roc, buf + rtp_len)) return false;
    update_roc(&ctx->tx, seq, roc);
    if (out_len) *out_len = rtp_len + STADIA_SRTP_TAG_LEN;
    return true;
}

bool stadia_srtp_unprotect(stadia_srtp_ctx_t *ctx, uint8_t *buf, size_t srtp_len, size_t *out_len) {
    if (!ctx || !ctx->keys_ready || !ctx->rx.ready) return false;
    if (srtp_len < 12 + STADIA_SRTP_TAG_LEN) return false;
    size_t rtp_len = srtp_len - STADIA_SRTP_TAG_LEN;
    size_t hlen = rtp_header_len(buf, rtp_len);
    if (hlen == 0) return false;

    uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
    uint32_t ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                    ((uint32_t)buf[10] << 8) | (uint32_t)buf[11];
    uint32_t roc = estimate_roc(&ctx->rx, seq);

    uint8_t calc_tag[STADIA_SRTP_TAG_LEN];
    if (!srtp_hmac_sha1_80(&ctx->rx, buf, rtp_len, roc, calc_tag)) return false;

    uint8_t diff = 0;
    for (int i = 0; i < STADIA_SRTP_TAG_LEN; i++) diff |= (uint8_t)(calc_tag[i] ^ buf[rtp_len + i]);
    if (diff != 0) return false;

    uint8_t iv[16], sb[16] = {0};
    size_t nc_off = 0;
    srtp_build_iv(ctx->rx.salt, ssrc, roc, seq, iv);
    size_t plen = rtp_len - hlen;
    if (plen > 0) {
        mbedtls_aes_crypt_ctr(&ctx->rx.aes, plen, &nc_off, iv, sb, buf + hlen, buf + hlen);
    }

    update_roc(&ctx->rx, seq, roc);
    if (out_len) *out_len = rtp_len;
    return true;
}
