/* Minimal RFC 3711 SRTP_AES128_CM_SHA1_80 + DTLS-SRTP key exporter (mbedTLS HW AES/SHA1). */
#ifndef STADIA_SRTP_H
#define STADIA_SRTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/ssl.h"

#define STADIA_SRTP_TAG_LEN 10

typedef struct {
    bool     ready;
    uint8_t  enc_key[16];
    uint8_t  auth_key[20];
    uint8_t  salt[14];
    uint32_t roc;
    uint16_t s_l;
    bool     has_seq;
    mbedtls_aes_context aes;
    mbedtls_md_context_t hmac_ctx;
    bool     hmac_ready;
} srtp_dir_t;

typedef struct {
    bool       keys_ready;
    srtp_dir_t tx; /* ESP32 (DTLS server) -> Controller (headset audio) */
    srtp_dir_t rx; /* Controller (DTLS client) -> ESP32 (microphone audio) */
} stadia_srtp_ctx_t;

void stadia_srtp_init(stadia_srtp_ctx_t *ctx);
void stadia_srtp_free(stadia_srtp_ctx_t *ctx);

/* Register with mbedtls_ssl_set_export_keys_cb(&L->ssl, stadia_srtp_export_cb, &L->srtp) */
void stadia_srtp_export_cb(void *p_expkey,
                           mbedtls_ssl_key_export_type type,
                           const unsigned char *secret,
                           size_t secret_len,
                           const unsigned char client_random[32],
                           const unsigned char server_random[32],
                           mbedtls_tls_prf_types tls_prf_type);

/* Encrypts RTP payload in-place and appends 10-byte HMAC-SHA1-80 tag.
   buf must have capacity >= rtp_len + STADIA_SRTP_TAG_LEN. */
bool stadia_srtp_protect(stadia_srtp_ctx_t *ctx, uint8_t *buf, size_t rtp_len, size_t *out_len);

/* Verifies 10-byte HMAC-SHA1-80 tag and decrypts RTP payload in-place. */
bool stadia_srtp_unprotect(stadia_srtp_ctx_t *ctx, uint8_t *buf, size_t srtp_len, size_t *out_len);

#endif /* STADIA_SRTP_H */
