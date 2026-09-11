/*
 * test_dtls.c - host tests for ngx_rtc_dtls.c against the system OpenSSL.
 *
 * The unit was previously outside the host build. Two levels here:
 *
 *   - guard paths: NULL arguments, a session that never handshook, garbage
 *     records. Cheap, and they are where an early-return usually forgets to
 *     free something (LeakSanitizer checks that).
 *
 *   - a real handshake, in-process: our side runs as the DTLS server through
 *     the module's own API (ngx_rtc_dtls_create / _on_data / _is_done /
 *     _get_srtp_key), the peer is a plain OpenSSL DTLS client whose records
 *     are piped straight into ngx_rtc_dtls_on_data. The two memory BIOs make
 *     it loopback, no sockets.
 *
 * The handshake is what makes the key export testable at all: the module's
 * offsets into the exported material are only observable once the handshake
 * succeeds. The expected values are rebuilt here from RFC 5764 section 4.2
 * (client key, server key, client salt, server salt -- in that order) rather
 * than copied from the implementation, so a layout mistake cannot pass by
 * agreeing with itself.
 */

#include "ngx_rtc_test.h"

#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ngx_rtc_dtls.h"

#define MATERIAL_LEN NGX_RTC_SRTP_KEY_MATERIAL_LEN   /* 60 */
#define SRTP_LABEL   "EXTRACTOR-dtls_srtp"

static ngx_rtc_dtls_t  g_srv;
static SSL            *g_cli;
static BIO            *g_cli_in;
static SSL_CTX        *g_cli_ctx;
static int             g_srv_done_calls;
static int             g_srv_records;

/* Server -> client. This is the send_cb the module invokes for each record. */
static void
srv_send_cb(void *user, const uint8_t *data, size_t len)
{
    (void)user;

    g_srv_records++;
    if (NULL != g_cli_in) {
        (void)BIO_write(g_cli_in, data, (int)len);
    }
}

static void
srv_done_cb(void *user)
{
    (void)user;
    g_srv_done_calls++;
}

/* Client -> server: hand every outgoing record to the module. */
static long
cli_bio_out_cb(BIO *bio, int oper, const char *argp, size_t len,
        int argi, long argl, int ret, size_t *processed)
{
    (void)bio;
    (void)argi;
    (void)argl;
    (void)processed;

    if (BIO_CB_WRITE == oper && NULL != argp && len > 0) {
        if (ngx_rtc_dtls_on_data(&g_srv, (const uint8_t *)argp, len) != 0) {
            return -1;
        }
    }

    return ret;
}

static int
dtls_peer_create(void)
{
    BIO  *out;

    g_cli_ctx = SSL_CTX_new(DTLS_client_method());
    if (NULL == g_cli_ctx) {
        return -1;
    }

    if (SSL_CTX_set_min_proto_version(g_cli_ctx, DTLS1_2_VERSION) != 1
            || SSL_CTX_set_max_proto_version(g_cli_ctx, DTLS1_2_VERSION) != 1) {
        return -1;
    }

    SSL_CTX_set_verify(g_cli_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_read_ahead(g_cli_ctx, 1);

    /* The peer must offer the same SRTP profile or the extension is not
     * negotiated and the key export is meaningless. */
    if (SSL_CTX_set_tlsext_use_srtp(g_cli_ctx, "SRTP_AES128_CM_SHA1_80") != 0) {
        return -1;
    }

    g_cli = SSL_new(g_cli_ctx);
    if (NULL == g_cli) {
        return -1;
    }

    g_cli_in = BIO_new(BIO_s_mem());
    out = BIO_new(BIO_s_mem());
    if (NULL == g_cli_in || NULL == out) {
        return -1;
    }

    BIO_set_callback_ex(out, cli_bio_out_cb);

    SSL_set_bio(g_cli, g_cli_in, out);

    SSL_set_options(g_cli, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(g_cli, 1200);
    DTLS_set_link_mtu(g_cli, 1200);

    SSL_set_connect_state(g_cli);

    return 0;
}

/* Ping-pong the handshake to completion. Each client step emits records that
 * the callback feeds to the server, and the server's replies land in the
 * client's inbound BIO. */
static void
dtls_pump(void)
{
    int  i;
    int  rc;
    int  err;

    for (i = 0; i < 500; i++) {
        if (0 == SSL_is_init_finished(g_cli)) {
            rc = SSL_do_handshake(g_cli);
            if (rc <= 0) {
                err = SSL_get_error(g_cli, rc);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE
                        && err != SSL_ERROR_ZERO_RETURN) {
                    ERR_clear_error();
                }
            }
        }

        if (0 != SSL_is_init_finished(g_cli) && 0 != ngx_rtc_dtls_is_done(&g_srv)) {
            break;
        }
    }

    ERR_clear_error();
}

static void
dtls_peer_destroy(void)
{
    if (NULL != g_cli) {
        SSL_free(g_cli);         /* frees both BIOs */
        g_cli = NULL;
    }
    g_cli_in = NULL;

    if (NULL != g_cli_ctx) {
        SSL_CTX_free(g_cli_ctx);
        g_cli_ctx = NULL;
    }
}

/* ------------------------------------------------------------------ */

NGX_RTC_TEST(dtls_global_init_and_fingerprint)
{
    const char *fp;
    size_t      i;
    size_t      len;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_global_init(), 0);   /* idempotent */

    fp = ngx_rtc_dtls_fingerprint();
    NGX_RTC_TEST_ASSERT(NULL != fp);

    /* SHA-256: "AA:BB:..." -- 32 byte pairs + 31 colons. */
    len = strlen(fp);
    NGX_RTC_TEST_ASSERT_U64_EQ(len, 95u);

    for (i = 0; i < len; i++) {
        if ((i % 3u) == 2u) {
            NGX_RTC_TEST_ASSERT(':' == fp[i]);
        } else {
            NGX_RTC_TEST_ASSERT(('0' <= fp[i] && fp[i] <= '9')
                                || ('A' <= fp[i] && fp[i] <= 'F'));
        }
    }
}

NGX_RTC_TEST(dtls_entry_points_reject_null)
{
    ngx_rtc_dtls_t  d;
    uint8_t         key[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];

    (void)memset(&d, 0, sizeof(d));

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_create(NULL, NULL, NULL, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_is_done(NULL), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_next_timeout_ms(NULL), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_handle_timeout(NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_on_data(NULL, key, sizeof(key)), -1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_get_srtp_key(NULL, key, key), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_get_srtp_key(&d, NULL, key), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_get_srtp_key(&d, key, NULL), -1);
    /* handshake_done is 0, so the export is refused regardless of buffers. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_get_srtp_key(&d, key, key), -1);

    ngx_rtc_dtls_destroy(NULL);   /* no-op, must not crash */
}

NGX_RTC_TEST(dtls_create_destroy_round_trip)
{
    ngx_rtc_dtls_t  d;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_create(&d, srv_send_cb, srv_done_cb, NULL), 0);

    NGX_RTC_TEST_ASSERT(NULL != d.ssl);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_is_done(&d), 0);

    ngx_rtc_dtls_destroy(&d);

    NGX_RTC_TEST_ASSERT(NULL == d.ssl);
    NGX_RTC_TEST_ASSERT(NULL == d.bio_in);
    NGX_RTC_TEST_ASSERT(NULL == d.bio_out);

    ngx_rtc_dtls_destroy(&d);   /* idempotent */
}

/* A record that is not DTLS must be refused or ignored, never fatal and never
 * read out of bounds. libsrtp2/OpenSSL are uninstrumented, so this asserts
 * behaviour rather than memory safety. */
NGX_RTC_TEST(dtls_on_data_rejects_garbage)
{
    ngx_rtc_dtls_t  d;
    uint8_t         junk[64];
    size_t          i;
    int             rc;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_create(&d, srv_send_cb, srv_done_cb, NULL), 0);

    for (i = 0; i < sizeof(junk); i++) {
        junk[i] = (uint8_t)(i * 7u + 1u);
    }

    /* Content type 0x17 (application data) on a fresh session: nothing to read. */
    junk[0] = 23;
    rc = ngx_rtc_dtls_on_data(&d, junk, sizeof(junk));
    NGX_RTC_TEST_ASSERT(0 == rc || -1 == rc);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_is_done(&d), 0);

    /* A timeout with no handshake in flight must not be fatal. */
    rc = ngx_rtc_dtls_handle_timeout(&d);
    NGX_RTC_TEST_ASSERT(0 == rc || -1 == rc);

    ngx_rtc_dtls_destroy(&d);
}

/* The real thing: a full DTLS 1.2 handshake, then check the exported SRTP
 * keying material against RFC 5764's layout.
 *
 *   material[0..16)   client_write_SRTP_master_key
 *   material[16..32)  server_write_SRTP_master_key
 *   material[32..46)  client_write_SRTP_master_salt
 *   material[46..60)  server_write_SRTP_master_salt
 *
 * We are the server, so recv = client key+salt and send = server key+salt. */
NGX_RTC_TEST(dtls_handshake_exports_rfc5764_key_material)
{
    unsigned char   material[MATERIAL_LEN];
    uint8_t         recv_key[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];
    uint8_t         send_key[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];
    uint8_t         expect[NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN];
    size_t          len;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_global_init(), 0);

    g_srv_done_calls = 0;
    g_srv_records = 0;
    (void)memset(&g_srv, 0, sizeof(g_srv));

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_dtls_create(&g_srv, srv_send_cb, srv_done_cb, NULL), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(dtls_peer_create(), 0);

    dtls_pump();

    NGX_RTC_TEST_ASSERT(1 == SSL_is_init_finished(g_cli));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_is_done(&g_srv), 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_srv_done_calls, 1);
    NGX_RTC_TEST_ASSERT(g_srv_records > 0);

    /* Once done, the module reports no pending retransmission deadline. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_dtls_next_timeout_ms(&g_srv), 0);

    /* Independent export on the peer, same label -- this is the reference. */
    len = MATERIAL_LEN;
    NGX_RTC_TEST_ASSERT_I64_EQ(SSL_export_keying_material(g_cli, material, len,
            SRTP_LABEL, sizeof(SRTP_LABEL) - 1, NULL, 0, 0), 1);

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_dtls_get_srtp_key(&g_srv, recv_key, send_key), 0);

    /* recv = client_write key + client_write salt */
    (void)memcpy(expect, material, NGX_RTC_SRTP_KEY_LEN);
    (void)memcpy(expect + NGX_RTC_SRTP_KEY_LEN,
                 material + 2 * NGX_RTC_SRTP_KEY_LEN, NGX_RTC_SRTP_SALT_LEN);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            memcmp(recv_key, expect, sizeof(expect)), 0);

    /* send = server_write key + server_write salt */
    (void)memcpy(expect, material + NGX_RTC_SRTP_KEY_LEN, NGX_RTC_SRTP_KEY_LEN);
    (void)memcpy(expect + NGX_RTC_SRTP_KEY_LEN,
                 material + 2 * NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN,
                 NGX_RTC_SRTP_SALT_LEN);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            memcmp(send_key, expect, sizeof(expect)), 0);

    /* The two directions must not be the same key material. */
    NGX_RTC_TEST_ASSERT(0 != memcmp(recv_key, send_key, sizeof(recv_key)));

    dtls_peer_destroy();
    ngx_rtc_dtls_destroy(&g_srv);
}
