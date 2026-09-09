/*
 * ngx_rtc_dtls.c - DTLS (RFC 5764) server side + SRTP key export, pure C11.
 *
 * Translated from SRS: src/app/srs_app_rtc_dtls.cpp
 * (SrsDtlsCertificate::initialize / srs_build_dtls_ctx / SrsDtlsImpl).
 */

#include "ngx_rtc_dtls.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <openssl/bio.h>
#include <openssl/dtls1.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#define NGX_RTC_DTLS_FRAGMENT_MAX 1200

/* Global, process-wide: self-signed certificate + shared SSL_CTX. */
static X509     *ngx_rtc_dtls_cert = NULL;
static EVP_PKEY *ngx_rtc_dtls_pkey = NULL;
static SSL_CTX  *ngx_rtc_dtls_ctx  = NULL;
static char      ngx_rtc_dtls_fp[128];

/* BIO write callback: OpenSSL calls it with each outgoing DTLS record. */
static long
ngx_rtc_dtls_bio_out_cb(BIO *bio, int oper, const char *argp, size_t len,
        int argi, long argl, int ret, size_t *processed)
{
    ngx_rtc_dtls_t *dtls;

    (void)argi;
    (void)argl;
    (void)processed;

    dtls = (ngx_rtc_dtls_t *)BIO_get_callback_arg(bio);
    if (BIO_CB_WRITE == oper && NULL != argp && len > 0 && NULL != dtls) {
        if (NULL != dtls->send_cb) {
            dtls->send_cb(dtls->user, (const uint8_t *)argp, len);
        }
    }
    return ret;
}

static int
ngx_rtc_dtls_gen_certificate(void)
{
    EVP_PKEY_CTX *pctx;
    X509_NAME    *name;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n;
    char         *p;
    unsigned int  i;

    /* RSA-2048 key via the OpenSSL 3.x keygen API. */
    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (NULL == pctx) {
        return -1;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0
            || EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0
            || EVP_PKEY_keygen(pctx, &ngx_rtc_dtls_pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return -1;
    }
    EVP_PKEY_CTX_free(pctx);

    /* Self-signed certificate. */
    ngx_rtc_dtls_cert = X509_new();
    if (NULL == ngx_rtc_dtls_cert) {
        return -1;
    }

    X509_set_version(ngx_rtc_dtls_cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(ngx_rtc_dtls_cert), 1);
    X509_gmtime_adj(X509_get_notBefore(ngx_rtc_dtls_cert), 0);
    X509_gmtime_adj(X509_get_notAfter(ngx_rtc_dtls_cert), 60 * 60 * 24 * 365);
    if (X509_set_pubkey(ngx_rtc_dtls_cert, ngx_rtc_dtls_pkey) <= 0) {
        return -1;
    }

    name = X509_get_subject_name(ngx_rtc_dtls_cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            (const unsigned char *)"webrtc", -1, -1, 0);
    X509_set_issuer_name(ngx_rtc_dtls_cert, name);

    if (X509_sign(ngx_rtc_dtls_cert, ngx_rtc_dtls_pkey, EVP_sha256()) <= 0) {
        return -1;
    }

    /* SHA-256 fingerprint, uppercase colon-separated ("AA:BB:..."). */
    n = 0;
    if (X509_digest(ngx_rtc_dtls_cert, EVP_sha256(), md, &n) <= 0) {
        return -1;
    }

    p = ngx_rtc_dtls_fp;
    for (i = 0; i < n; i++) {
        p += (unsigned int)snprintf(p, sizeof(ngx_rtc_dtls_fp) - (size_t)(p - ngx_rtc_dtls_fp),
                (0 == i) ? "%02X" : ":%02X", md[i]);
    }

    return 0;
}

static int
ngx_rtc_dtls_build_ctx(void)
{
    ngx_rtc_dtls_ctx = SSL_CTX_new(DTLS_server_method());
    if (NULL == ngx_rtc_dtls_ctx) {
        return -1;
    }

    /* ECDSA curves for the DTLS key exchange. */
    if (SSL_CTX_set1_curves_list(ngx_rtc_dtls_ctx, "P-256:P-384:P-521") <= 0) {
        return -1;
    }

    if (SSL_CTX_set_cipher_list(ngx_rtc_dtls_ctx,
            "DEFAULT:!aNULL:!eNULL:!LOW:!EXP:!RC4:!DES:!3DES:!MD5") <= 0) {
        return -1;
    }

    /* WebRTC requires DTLS 1.2 (RFC 5764 / RFC 6347). */
    if (SSL_CTX_set_min_proto_version(ngx_rtc_dtls_ctx, DTLS1_2_VERSION) != 1
            || SSL_CTX_set_max_proto_version(ngx_rtc_dtls_ctx,
                                             DTLS1_2_VERSION) != 1) {
        return -1;
    }

    if (SSL_CTX_use_certificate(ngx_rtc_dtls_ctx, ngx_rtc_dtls_cert) <= 0
            || SSL_CTX_use_PrivateKey(ngx_rtc_dtls_ctx, ngx_rtc_dtls_pkey) <= 0) {
        return -1;
    }

    /* Browser DTLS certs are self-signed ephemeral; skip peer verification. */
    SSL_CTX_set_verify(ngx_rtc_dtls_ctx, SSL_VERIFY_NONE, NULL);

    SSL_CTX_set_read_ahead(ngx_rtc_dtls_ctx, 1);

    /* RFC 5764: negotiate SRTP_AES128_CM_SHA1_80. */
    if (SSL_CTX_set_tlsext_use_srtp(ngx_rtc_dtls_ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        return -1;
    }

    return 0;
}

int
ngx_rtc_dtls_global_init(void)
{
    if (NULL != ngx_rtc_dtls_ctx) {
        return 0;
    }

    if (ngx_rtc_dtls_gen_certificate() != 0) {
        return -1;
    }

    if (ngx_rtc_dtls_build_ctx() != 0) {
        return -1;
    }

    return 0;
}

const char *
ngx_rtc_dtls_fingerprint(void)
{
    return ngx_rtc_dtls_fp;
}

int
ngx_rtc_dtls_create(ngx_rtc_dtls_t *dtls,
        ngx_rtc_dtls_send_cb send_cb, ngx_rtc_dtls_done_cb done_cb, void *user)
{
    if (NULL == dtls || NULL == ngx_rtc_dtls_ctx) {
        return -1;
    }

    memset(dtls, 0, sizeof(*dtls));
    dtls->send_cb = send_cb;
    dtls->done_cb = done_cb;
    dtls->user = user;

    dtls->ssl = SSL_new(ngx_rtc_dtls_ctx);
    if (NULL == dtls->ssl) {
        return -1;
    }

    /* Fragment the DTLS handshake to fit the UDP MTU. */
    SSL_set_options(dtls->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(dtls->ssl, NGX_RTC_DTLS_FRAGMENT_MAX);
    DTLS_set_link_mtu(dtls->ssl, NGX_RTC_DTLS_FRAGMENT_MAX);

    dtls->bio_in = BIO_new(BIO_s_mem());
    dtls->bio_out = BIO_new(BIO_s_mem());
    if (NULL == dtls->bio_in || NULL == dtls->bio_out) {
        ngx_rtc_dtls_destroy(dtls);
        return -1;
    }

    BIO_set_callback_ex(dtls->bio_out, ngx_rtc_dtls_bio_out_cb);
    BIO_set_callback_arg(dtls->bio_out, (char *)dtls);

    SSL_set_bio(dtls->ssl, dtls->bio_in, dtls->bio_out);

    /* Server role (SRS SrsDtlsServerImpl::initialize does the same). */
    SSL_set_accept_state(dtls->ssl);

    return 0;
}

int
ngx_rtc_dtls_on_data(ngx_rtc_dtls_t *dtls, const uint8_t *data, size_t len)
{
    char    buf[1500];
    int     r0;
    int     r1;

    if (NULL == dtls || NULL == dtls->ssl) {
        return -1;
    }

    if (BIO_write(dtls->bio_in, data, (int)len) <= 0) {
        return -1;
    }

    /* SSL_read drives the handshake and consumes application data. */
    r0 = SSL_read(dtls->ssl, buf, sizeof(buf));
    r1 = SSL_get_error(dtls->ssl, r0);
    ERR_clear_error();

    if (r0 <= 0) {
        if (r1 != SSL_ERROR_WANT_READ && r1 != SSL_ERROR_WANT_WRITE
                && r1 != SSL_ERROR_ZERO_RETURN) {
            return -1;
        }
    }

    if (0 == dtls->handshake_done && SSL_is_init_finished(dtls->ssl) == 1) {
        dtls->handshake_done = 1;
        if (NULL != dtls->done_cb) {
            dtls->done_cb(dtls->user);
        }
    }

    return 0;
}

int
ngx_rtc_dtls_is_done(const ngx_rtc_dtls_t *dtls)
{
    return (NULL != dtls) ? (int)dtls->handshake_done : 0;
}

int64_t
ngx_rtc_dtls_next_timeout_ms(const ngx_rtc_dtls_t *dtls)
{
    struct timeval tv;

    if (NULL == dtls || NULL == dtls->ssl || 0 != dtls->handshake_done) {
        return 0;
    }

    if (DTLSv1_get_timeout(dtls->ssl, &tv) == 0) {
        return 0;
    }

    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int
ngx_rtc_dtls_handle_timeout(ngx_rtc_dtls_t *dtls)
{
    int rc;

    if (NULL == dtls || NULL == dtls->ssl) {
        return -1;
    }

    rc = DTLSv1_handle_timeout(dtls->ssl);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0) {
        return 0;
    }

    if (0 == dtls->handshake_done && SSL_is_init_finished(dtls->ssl) == 1) {
        dtls->handshake_done = 1;
        if (NULL != dtls->done_cb) {
            dtls->done_cb(dtls->user);
        }
    }

    return 0;
}

int
ngx_rtc_dtls_get_srtp_key(ngx_rtc_dtls_t *dtls, uint8_t *recv_key, uint8_t *send_key)
{
    static const char label[] = "EXTRACTOR-dtls_srtp";
    unsigned char     material[NGX_RTC_SRTP_KEY_MATERIAL_LEN];
    size_t            offset;

    if (NULL == dtls || NULL == recv_key || NULL == send_key
            || 0 == dtls->handshake_done) {
        return -1;
    }

    if (SSL_export_keying_material(dtls->ssl, material, sizeof(material),
            label, sizeof(label) - 1, NULL, 0, 0) != 1) {
        return -1;
    }

    /* Server role: recv = client key+salt, send = server key+salt. */
    offset = 0;
    memcpy(recv_key, material + offset, NGX_RTC_SRTP_KEY_LEN);
    offset += NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_KEY_LEN;
    memcpy(recv_key + NGX_RTC_SRTP_KEY_LEN, material + offset, NGX_RTC_SRTP_SALT_LEN);
    offset += NGX_RTC_SRTP_SALT_LEN;

    memcpy(send_key, material + NGX_RTC_SRTP_KEY_LEN, NGX_RTC_SRTP_KEY_LEN);
    memcpy(send_key + NGX_RTC_SRTP_KEY_LEN, material + offset, NGX_RTC_SRTP_SALT_LEN);

    return 0;
}

void
ngx_rtc_dtls_destroy(ngx_rtc_dtls_t *dtls)
{
    if (NULL == dtls) {
        return;
    }

    if (NULL != dtls->ssl) {
        SSL_free(dtls->ssl);
        dtls->ssl = NULL;
    }
    /* bio_in / bio_out are owned and freed by SSL_free. */
    dtls->bio_in = NULL;
    dtls->bio_out = NULL;
    dtls->handshake_done = 0;
}
