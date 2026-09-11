/**
 * @file    ngx_rtc_dtls.h
 * @brief   DTLS (RFC 5764) server side + SRTP key export, pure C11.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Translated from SRS: src/app/srs_app_rtc_dtls.cpp
 * (SrsDtlsCertificate / SrsDtlsImpl / SrsDtlsServerImpl / get_srtp_key).
 * OpenSSL 3.x DTLS + memory BIO + use_srtp extension.
 */

#ifndef NGX_RTC_DTLS_H
#define NGX_RTC_DTLS_H

#include <stdint.h>
#include <stddef.h>

#include <openssl/ssl.h>

/* SRTP master key/salt lengths (RFC 5764, AES_CM_128_HMAC_SHA1_80). */
#define NGX_RTC_SRTP_KEY_LEN  16
#define NGX_RTC_SRTP_SALT_LEN 14
#define NGX_RTC_SRTP_KEY_MATERIAL_LEN (2 * (NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN))

typedef struct ngx_rtc_dtls_s ngx_rtc_dtls_t;

/* Callback to send a DTLS record over UDP (implemented by the caller). */
typedef void (*ngx_rtc_dtls_send_cb)(void *user, const uint8_t *data, size_t len);

/* Callback fired once the DTLS handshake is finished. */
typedef void (*ngx_rtc_dtls_done_cb)(void *user);

struct ngx_rtc_dtls_s {
    SSL *ssl;
    BIO *bio_in;
    BIO *bio_out;
    uint8_t handshake_done;
    ngx_rtc_dtls_send_cb send_cb;
    ngx_rtc_dtls_done_cb done_cb;
    void *user;
};

/*
 * Global init (idempotent): generate the self-signed DTLS certificate once.
 * Return 0 on success, -1 on OpenSSL error.
 */
int ngx_rtc_dtls_global_init(void);

/* Return the certificate SHA-256 fingerprint, "AA:BB:...". Valid after global init. */
const char *ngx_rtc_dtls_fingerprint(void);

/*
 * Create a DTLS server session. The SSL_CTX is shared globally.
 *   send_cb / done_cb / user : callbacks for outgoing records and handshake done.
 * Return 0 on success, -1 on error.
 */
int ngx_rtc_dtls_create(ngx_rtc_dtls_t *dtls,
        ngx_rtc_dtls_send_cb send_cb, ngx_rtc_dtls_done_cb done_cb, void *user);

/*
 * Feed a received DTLS record and drive the handshake.
 * Return 0 on success, -1 on fatal error.
 */
int ngx_rtc_dtls_on_data(ngx_rtc_dtls_t *dtls, const uint8_t *data, size_t len);

/* Whether the handshake has finished. */
int ngx_rtc_dtls_is_done(const ngx_rtc_dtls_t *dtls);

/* Return the next OpenSSL DTLS retransmission deadline in milliseconds, or 0
 * when no timer is pending. */
int64_t ngx_rtc_dtls_next_timeout_ms(const ngx_rtc_dtls_t *dtls);

/* Drive OpenSSL's DTLS retransmission timer. Returns 0 when the timeout was
 * handled or no timeout is pending, -1 on a fatal DTLS error. */
int ngx_rtc_dtls_handle_timeout(ngx_rtc_dtls_t *dtls);

/*
 * Export SRTP keying material after the handshake.
 *   recv_key / send_key : caller buffers of NGX_RTC_SRTP_KEY_LEN + SALT_LEN (30 bytes).
 * Return 0 on success, -1 if the handshake is not done.
 */
int ngx_rtc_dtls_get_srtp_key(ngx_rtc_dtls_t *dtls, uint8_t *recv_key, uint8_t *send_key);

void ngx_rtc_dtls_destroy(ngx_rtc_dtls_t *dtls);

#endif /* NGX_RTC_DTLS_H */
