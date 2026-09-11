/*
 * test_srtp.c - host tests for ngx_rtc_srtp.c against the real libsrtp2.
 *
 * This unit was previously excluded from the host build (only its header was
 * stubbed for ngx_rtc_core.h). It is linked here against the same
 * libsrtp2.a the nginx module links, so protect/unprotect run the real AES-CM
 * + HMAC-SHA1-80 paths rather than a mock.
 *
 * What this buys over "it compiles": the round trip proves the key material and
 * the in-place buffer arithmetic are right (nb is the buffer length on the way
 * in and the new length on the way out), the negative cases prove the auth tag
 * is actually verified, and the create/destroy pairs let LeakSanitizer check
 * that every libsrtp2 context is deallocated.
 */

#include "ngx_rtc_test.h"

#include <string.h>

#include "ngx_rtc_dtls.h"
#include "ngx_rtc_srtp.h"

#define KEY_MAT_LEN (NGX_RTC_SRTP_KEY_LEN + NGX_RTC_SRTP_SALT_LEN)  /* 30 */
#define SRTP_TAG_LEN 10    /* RTP:  aes_cm_128_hmac_sha1_80 auth tag only   */
#define SRTCP_TAG_LEN 14   /* RTCP: 4-byte E+SRTCP-index plus the same tag */

/* Backing store for one packet: the packet plus slack for the auth tag, so a
 * neighbouring object is never what the overflow lands on. */
#define PKT_BUF 128

static void
fill_key(uint8_t *key, uint8_t seed)
{
    size_t i;

    for (i = 0; i < KEY_MAT_LEN; i++) {
        key[i] = (uint8_t)(seed + i);
    }
}

/* Minimal well-formed RTP: V=2, PT=96, no padding/extension/CSRCs. */
static int
build_rtp(uint8_t *buf, uint32_t ssrc)
{
    memset(buf, 0, PKT_BUF);

    buf[0] = 0x80;              /* V=2, P=0, X=0, CC=0 */
    buf[1] = 96;                /* M=0, PT=96 */
    buf[2] = 0x12;  buf[3] = 0x34;                    /* seq   */
    buf[4] = 0x00;  buf[5] = 0x00;
    buf[6] = 0x27;  buf[7] = 0x10;                    /* ts    */
    buf[8]  = (uint8_t)(ssrc >> 24); buf[9]  = (uint8_t)(ssrc >> 16);
    buf[10] = (uint8_t)(ssrc >> 8);  buf[11] = (uint8_t)ssrc;

    /* 20 bytes of payload so the tag lands on real data. */
    memset(buf + 12, 0xA5, 20);

    return 12 + 20;
}

/* Minimal well-formed RTCP RR: V=2, PT=201, length=1 (2 words = 8 bytes). */
static int
build_rtcp(uint8_t *buf, uint32_t ssrc)
{
    memset(buf, 0, PKT_BUF);

    buf[0] = 0x80;              /* V=2, P=0, RC=0 */
    buf[1] = 201;               /* RR */
    buf[2] = 0x00;  buf[3] = 0x01;                    /* length in words - 1 */
    buf[4] = (uint8_t)(ssrc >> 24); buf[5] = (uint8_t)(ssrc >> 16);
    buf[6] = (uint8_t)(ssrc >> 8);  buf[7] = (uint8_t)ssrc;

    return 8;
}

NGX_RTC_TEST(srtp_global_init_is_idempotent)
{
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
}

NGX_RTC_TEST(srtp_create_rejects_null_arguments)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         key[KEY_MAT_LEN];

    fill_key(key, 1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(NULL, key, key), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, NULL, key), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, NULL), -1);
}

NGX_RTC_TEST(srtp_destroy_null_is_a_noop)
{
    ngx_rtc_srtp_destroy(NULL);
}

/* Every entry point must reject a NULL context / buffer / length, and a context
 * that was never created. */
NGX_RTC_TEST(srtp_entry_points_reject_null_and_uninitialised)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         buf[PKT_BUF];
    uint8_t         key[KEY_MAT_LEN];
    int             n;

    fill_key(key, 2);
    n = build_rtp(buf, 0x11223344u);

    memset(&ctx, 0, sizeof(ctx));   /* created-but-empty, as after a failed create */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(NULL, buf, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&ctx, NULL, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&ctx, buf, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&ctx, buf, &n), -1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(NULL, buf, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, NULL, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, buf, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, buf, &n), -1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtcp(NULL, buf, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtcp(&ctx, NULL, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtcp(&ctx, buf, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtcp(&ctx, buf, &n), -1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtcp(NULL, buf, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtcp(&ctx, NULL, &n), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtcp(&ctx, buf, NULL), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtcp(&ctx, buf, &n), -1);
}

NGX_RTC_TEST(srtp_rtp_protect_unprotect_round_trip)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         plain[PKT_BUF];
    uint8_t         work[PKT_BUF];
    uint8_t         key[KEY_MAT_LEN];
    int             n;
    int             plain_len;

    fill_key(key, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, key), 0);

    plain_len = build_rtp(plain, 0x0A0B0C0Du);
    memcpy(work, plain, sizeof(work));

    n = plain_len;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&ctx, work, &n), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, plain_len + SRTP_TAG_LEN);

    /* The header stays in the clear (SRTP encrypts payload only)... */
    NGX_RTC_TEST_ASSERT_I64_EQ(memcmp(work, plain, 12), 0);
    /* ...and the payload really was encrypted. */
    NGX_RTC_TEST_ASSERT(memcmp(work + 12, plain + 12, 20) != 0);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, work, &n), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, plain_len);
    NGX_RTC_TEST_ASSERT_I64_EQ(memcmp(work, plain, (size_t)plain_len), 0);

    ngx_rtc_srtp_destroy(&ctx);
}

NGX_RTC_TEST(srtp_rtcp_protect_unprotect_round_trip)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         plain[PKT_BUF];
    uint8_t         work[PKT_BUF];
    uint8_t         key[KEY_MAT_LEN];
    int             n;
    int             plain_len;

    fill_key(key, 4);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, key), 0);

    plain_len = build_rtcp(plain, 0x0A0B0C0Du);
    memcpy(work, plain, sizeof(work));

    n = plain_len;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtcp(&ctx, work, &n), 0);
    /* RTCP grows by 14, not 10: SRTCP prepends a 31-bit index + E bit. */
    NGX_RTC_TEST_ASSERT_I64_EQ(n, plain_len + SRTCP_TAG_LEN);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtcp(&ctx, work, &n), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, plain_len);
    NGX_RTC_TEST_ASSERT_I64_EQ(memcmp(work, plain, (size_t)plain_len), 0);

    ngx_rtc_srtp_destroy(&ctx);
}

/* A packet protected under one key must not unprotect under another: this is
 * what proves the auth tag is verified rather than stripped. */
NGX_RTC_TEST(srtp_unprotect_rejects_wrong_key)
{
    ngx_rtc_srtp_t  tx;
    ngx_rtc_srtp_t  rx;
    uint8_t         plain[PKT_BUF];
    uint8_t         work[PKT_BUF];
    uint8_t         key_a[KEY_MAT_LEN];
    uint8_t         key_b[KEY_MAT_LEN];
    int             n;
    int             plain_len;

    fill_key(key_a, 5);
    fill_key(key_b, 6);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&tx, key_a, key_a), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&rx, key_b, key_b), 0);

    plain_len = build_rtp(plain, 0x0A0B0C0Du);
    memcpy(work, plain, sizeof(work));

    n = plain_len;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&tx, work, &n), 0);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&rx, work, &n), -1);

    ngx_rtc_srtp_destroy(&rx);
    ngx_rtc_srtp_destroy(&tx);
}

/* Tampering with the ciphertext must fail the auth check. */
NGX_RTC_TEST(srtp_unprotect_rejects_tampered_payload)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         plain[PKT_BUF];
    uint8_t         work[PKT_BUF];
    uint8_t         key[KEY_MAT_LEN];
    int             n;
    int             plain_len;

    fill_key(key, 7);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, key), 0);

    plain_len = build_rtp(plain, 0x0A0B0C0Du);
    memcpy(work, plain, sizeof(work));

    n = plain_len;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_protect_rtp(&ctx, work, &n), 0);

    work[16] ^= 0xFF;   /* flip a payload byte, tag stays as-is */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, work, &n), -1);

    ngx_rtc_srtp_destroy(&ctx);
}

/* Repeating destroy must not double-free; libsrtp2 contexts are torn down
 * exactly once because destroy clears the field. */
NGX_RTC_TEST(srtp_destroy_is_idempotent)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         key[KEY_MAT_LEN];

    fill_key(key, 8);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, key), 0);

    ngx_rtc_srtp_destroy(&ctx);
    NGX_RTC_TEST_ASSERT(NULL == ctx.recv_ctx);
    NGX_RTC_TEST_ASSERT(NULL == ctx.send_ctx);

    ngx_rtc_srtp_destroy(&ctx);   /* second call must be a no-op */
}

/* A truncated datagram (shorter than the tag) must be rejected, not read
 * past. libsrtp2 itself is not instrumented, so this also documents where the
 * sanitizer's reach ends. */
NGX_RTC_TEST(srtp_unprotect_rejects_short_packet)
{
    ngx_rtc_srtp_t  ctx;
    uint8_t         buf[PKT_BUF];
    uint8_t         key[KEY_MAT_LEN];
    int             n;

    fill_key(key, 9);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_global_init(), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_create(&ctx, key, key), 0);

    memset(buf, 0, sizeof(buf));
    buf[0] = 0x80;
    buf[1] = 96;

    n = 4;   /* shorter than the 10-byte tag */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_srtp_unprotect_rtp(&ctx, buf, &n), -1);

    ngx_rtc_srtp_destroy(&ctx);
}
