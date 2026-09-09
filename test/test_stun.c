/*
 * test_stun.c - host unit tests for ngx_rtc_stun.c.
 *
 * Covers: ufrag parsing (including over-long USERNAME), USE_CANDIDATE /
 * ICE_CONTROLLED decoding, binding-response encode bounds (the fixed P0
 * overflow), and the wire values of XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY and
 * FINGERPRINT.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_stun.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

/* FINGERPRINT XOR constant (RFC 5389 15.5), local to the test oracle. */
#define TEST_STUN_FINGERPRINT_XOR 0x5354554Eu

static void stun_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xffu);
}

static void stun_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static uint16_t stun_rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t stun_rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Independent CRC-32 IEEE 802.3 oracle for FINGERPRINT. */
static uint32_t test_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        uint32_t j;

        crc ^= data[i];
        for (j = 0; j < 8u; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Build a BindingRequest with a USERNAME and optional ICE attributes. */
static uint32_t build_binding_request(uint8_t *buf, const char *username,
                                      int with_ice_flags)
{
    uint32_t ulen = (uint32_t)strlen(username);
    uint32_t upad = (4u - (ulen % 4u)) % 4u;
    uint32_t body_len = 4u + ulen + upad;
    uint32_t pos = 20u;
    uint32_t i;

    if (with_ice_flags)
    {
        body_len += 4u;                    /* USE-CANDIDATE (empty) */
        body_len += 4u + 8u;               /* ICE-CONTROLLED (8-byte value) */
    }

    stun_wr_u16(buf, NGX_RTC_STUN_BINDING_REQUEST);
    stun_wr_u16(buf + 2, (uint16_t)body_len);
    stun_wr_u32(buf + 4, NGX_RTC_STUN_MAGIC_COOKIE);
    for (i = 0; i < 12u; i++)
    {
        buf[8u + i] = (uint8_t)i;
    }

    stun_wr_u16(buf + pos, NGX_RTC_STUN_ATTR_USERNAME);
    stun_wr_u16(buf + pos + 2, (uint16_t)ulen);
    pos += 4u;
    (void)memcpy(buf + pos, username, ulen);
    pos += ulen;
    for (i = 0; i < upad; i++)
    {
        buf[pos + i] = 0;
    }
    pos += upad;

    if (with_ice_flags)
    {
        stun_wr_u16(buf + pos, NGX_RTC_STUN_ATTR_USE_CANDIDATE);
        stun_wr_u16(buf + pos + 2, 0);
        pos += 4u;

        stun_wr_u16(buf + pos, NGX_RTC_STUN_ATTR_ICE_CONTROLLED);
        stun_wr_u16(buf + pos + 2, 8);
        pos += 4u;
        for (i = 0; i < 8u; i++)
        {
            buf[pos + i] = 0;
        }
        pos += 8u;
    }

    return pos;
}

/* Locate an attribute; on success set *val_off and *val_len. */
static int find_attr(const uint8_t *pkt, uint32_t len, uint16_t type,
                     uint32_t *hdr_off, uint32_t *val_off, uint16_t *val_len)
{
    uint32_t pos = 20u;

    while ((pos + 4u) <= len)
    {
        uint16_t t = stun_rd_u16(pkt + pos);
        uint16_t l = stun_rd_u16(pkt + pos + 2);

        if ((pos + 4u + (uint32_t)l) > len)
        {
            return -1;
        }
        if (t == type)
        {
            *hdr_off = pos;
            *val_off = pos + 4u;
            *val_len = l;
            return 0;
        }
        pos += 4u + (uint32_t)l;
        if (0u != (l % 4u))
        {
            pos += 4u - (l % 4u);
        }
    }
    return -1;
}

NGX_RTC_TEST(stun_decode_username_and_flags)
{
    uint8_t req[256];
    uint32_t req_len;
    ngx_rtc_stun_t stun;

    req_len = build_binding_request(req, "svr1:cli1", 1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.type, NGX_RTC_STUN_BINDING_REQUEST);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_username, 1);
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.local_ufrag, "svr1");
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.remote_ufrag, "cli1");
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.use_candidate, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.ice_controlled, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.ice_controlling, 0);
}

NGX_RTC_TEST(stun_decode_overlong_ufrags_capped)
{
    uint8_t req[512];
    uint32_t req_len;
    ngx_rtc_stun_t stun;
    char long_username[96];

    /* "LO:" + 64 'x' characters: attr is far longer than 2*UFRAG_MAX. */
    (void)memset(long_username, 'x', sizeof(long_username));
    long_username[0] = 'L';
    long_username[1] = 'O';
    long_username[2] = ':';
    long_username[67] = '\0'; /* "LO:" + 64 'x' = 67 chars, NUL-terminated */

    req_len = build_binding_request(req, long_username, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_username, 1);
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.local_ufrag, "LO");
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.remote_ufrag, "xxxxxxxxxxxxxxxx");

    /* A local ufrag longer than 16 is capped too. */
    req_len = build_binding_request(req, "0123456789ABCDEFGH:abcdefgh", 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.local_ufrag, "0123456789ABCDEF");
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.remote_ufrag, "abcdefgh");
}

NGX_RTC_TEST(stun_decode_malformed)
{
    uint8_t req[256];
    uint32_t req_len;
    ngx_rtc_stun_t stun;

    req_len = build_binding_request(req, "a:b", 0);

    /* Header too short. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 19u), -1);

    /* Declared length does not match the buffer. */
    stun_wr_u16(req + 2, 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), -1);
    stun_wr_u16(req + 2, (uint16_t)(req_len - 20u));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);

    /* Attribute value runs past the end of the packet. */
    stun_wr_u16(req + 20u + 2u, 0xFFu); /* USERNAME length far too large */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), -1);
}

static uint32_t build_integrity_request(uint8_t *buf, const char *username,
                                        const char *pwd)
{
    uint32_t pos;
    uint32_t mi_hdr_off;
    uint32_t mi_value_off;
    uint8_t  hmac[EVP_MAX_MD_SIZE];
    unsigned hmac_len = 0;
    uint32_t fp_value_off;
    uint32_t crc;

    pos = build_binding_request(buf, username, 0);
    mi_hdr_off = pos;

    stun_wr_u16(buf + pos, NGX_RTC_STUN_ATTR_MESSAGE_INTEGRITY);
    stun_wr_u16(buf + pos + 2, 20u);
    pos += 4u;
    mi_value_off = pos;

    /* The HMAC input uses the message length through MESSAGE-INTEGRITY, not
     * the final FINGERPRINT-inclusive length. */
    stun_wr_u16(buf + 2, (uint16_t)(mi_value_off - 20u + 20u));

    if (NULL == HMAC(EVP_sha1(), pwd, (int)strlen(pwd), buf, mi_hdr_off,
                     hmac, &hmac_len) || hmac_len != 20u)
    {
        return 0;
    }
    (void)memcpy(buf + pos, hmac, 20u);
    pos += 20u;

    stun_wr_u16(buf + pos, NGX_RTC_STUN_ATTR_FINGERPRINT);
    stun_wr_u16(buf + pos + 2, 4u);
    pos += 4u;
    fp_value_off = pos;
    stun_wr_u32(buf + pos, 0u);
    pos += 4u;

    stun_wr_u16(buf + 2, (uint16_t)(pos - 20u));
    crc = test_crc32(buf, pos) ^ TEST_STUN_FINGERPRINT_XOR;
    stun_wr_u32(buf + fp_value_off, crc);

    return pos;
}

NGX_RTC_TEST(stun_verify_request_integrity_and_fingerprint)
{
    uint8_t req[512];
    uint32_t req_len;
    ngx_rtc_stun_t stun;

    req_len = build_integrity_request(req, "svr1:cli1", "pwd123");
    NGX_RTC_TEST_ASSERT(req_len > 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_message_integrity, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_fingerprint, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_verify_request(&stun, req, req_len,
                                                           "pwd123"), 0);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_verify_request(&stun, req, req_len,
                                                           "wrong"), -1);
    req[10] ^= 0x01u; /* corrupt transaction id, fingerprint fails */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_verify_request(&stun, req, req_len,
                                                           "pwd123"), -1);
}

NGX_RTC_TEST(stun_encode_overlong_does_not_overflow_128)
{
    uint8_t arena[8u + 128u + 8u];
    uint8_t *out = arena + 8u;
    uint8_t *pre = arena;
    uint8_t *post = arena + 8u + 128u;
    uint8_t req[512];
    uint32_t req_len;
    ngx_rtc_stun_t stun;
    char long_username[96];
    int r;

    (void)memset(arena, 0xAA, sizeof(arena));
    (void)memset(long_username, 'x', sizeof(long_username));
    long_username[0] = 'L';
    long_username[1] = 'O';
    long_username[2] = ':';
    long_username[67] = '\0';

    req_len = build_binding_request(req, long_username, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);

    /* Exactly the minimum accepted buffer size. */
    r = ngx_rtc_stun_encode_binding_response(&stun, "pwd",
                                             0x0A000001u, 12345u,
                                             out, 128u);
    NGX_RTC_TEST_ASSERT(r > 0);
    NGX_RTC_TEST_ASSERT(r <= 128);

    /* Guard regions must be untouched (no out-of-bounds write). */
    NGX_RTC_TEST_ASSERT_I64_EQ(pre[0], 0xAA);
    NGX_RTC_TEST_ASSERT_I64_EQ(pre[7], 0xAA);
    NGX_RTC_TEST_ASSERT_I64_EQ(post[0], 0xAA);
    NGX_RTC_TEST_ASSERT_I64_EQ(post[7], 0xAA);

    /* A 127-byte buffer is rejected up front and must not be written. */
    (void)memset(arena, 0xAA, sizeof(arena));
    r = ngx_rtc_stun_encode_binding_response(&stun, "pwd",
                                             0x0A000001u, 12345u,
                                             out, 127u);
    NGX_RTC_TEST_ASSERT_I64_EQ(r, -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(pre[0], 0xAA);
    NGX_RTC_TEST_ASSERT_I64_EQ(post[0], 0xAA);
}

NGX_RTC_TEST(stun_encode_requires_username)
{
    ngx_rtc_stun_t stun;
    uint8_t out[256];

    (void)memset(&stun, 0, sizeof(stun));
    stun.has_username = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_stun_encode_binding_response(&stun, "pwd",
                                             0x0A000001u, 12345u,
                                             out, sizeof(out)),
        -1);
}

NGX_RTC_TEST(stun_response_wire_attributes)
{
    uint8_t req[256];
    uint8_t resp[256];
    uint32_t req_len;
    ngx_rtc_stun_t stun;
    ngx_rtc_stun_t round;
    int resp_len;
    uint32_t hdr_off;
    uint32_t val_off;
    uint16_t val_len;

    req_len = build_binding_request(req, "svr1:cli1", 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, req_len), 0);

    resp_len = ngx_rtc_stun_encode_binding_response(&stun, "pwd",
                                                    0x0A000001u, 12345u,
                                                    resp, sizeof(resp));
    NGX_RTC_TEST_ASSERT(resp_len > 0);

    /* Header: response type, magic cookie, echoed transaction id, length. */
    NGX_RTC_TEST_ASSERT_I64_EQ(stun_rd_u16(resp), NGX_RTC_STUN_BINDING_RESPONSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun_rd_u16(resp + 2), (uint16_t)(resp_len - 20));
    NGX_RTC_TEST_ASSERT_U64_EQ(stun_rd_u32(resp + 4), NGX_RTC_STUN_MAGIC_COOKIE);
    NGX_RTC_TEST_ASSERT_MEM_EQ(resp + 8, req + 8, 12u);

    /* XOR-MAPPED-ADDRESS: family IPv4, port/addr XORed with magic cookie. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        find_attr(resp, (uint32_t)resp_len, NGX_RTC_STUN_ATTR_XOR_MAPPED_ADDR,
                  &hdr_off, &val_off, &val_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(val_len, 8);
    NGX_RTC_TEST_ASSERT_I64_EQ(resp[val_off + 0], 0); /* reserved */
    NGX_RTC_TEST_ASSERT_I64_EQ(resp[val_off + 1], 1); /* IPv4 */
    NGX_RTC_TEST_ASSERT_I64_EQ(stun_rd_u16(resp + val_off + 2),
                               (uint16_t)(12345u ^ (NGX_RTC_STUN_MAGIC_COOKIE >> 16)));
    NGX_RTC_TEST_ASSERT_U64_EQ(stun_rd_u32(resp + val_off + 4),
                               0x0A000001u ^ NGX_RTC_STUN_MAGIC_COOKIE);

    /* MESSAGE-INTEGRITY: HMAC-SHA1 over the header..attribute start. The
     * encoder patches the message-length field to include MI (but not
     * FINGERPRINT) before computing the MAC, so the test oracle rebuilds that
     * intermediate header state. */
    {
        uint8_t mi_input[256];
        uint8_t hmac[EVP_MAX_MD_SIZE];
        unsigned hmac_len = 0;

        NGX_RTC_TEST_ASSERT_I64_EQ(
            find_attr(resp, (uint32_t)resp_len, NGX_RTC_STUN_ATTR_MESSAGE_INTEGRITY,
                      &hdr_off, &val_off, &val_len), 0);
        NGX_RTC_TEST_ASSERT_I64_EQ(val_len, 20);
        NGX_RTC_TEST_ASSERT(hdr_off < sizeof(mi_input));

        (void)memcpy(mi_input, resp, hdr_off);
        stun_wr_u16(mi_input + 2, (uint16_t)((hdr_off - 20u) + 24u));

        NGX_RTC_TEST_ASSERT(NULL != HMAC(EVP_sha1(), "pwd", 3,
                                         mi_input, hdr_off, hmac, &hmac_len));
        NGX_RTC_TEST_ASSERT_I64_EQ(hmac_len, 20);
        NGX_RTC_TEST_ASSERT_MEM_EQ(resp + val_off, hmac, 20u);
    }

    /* FINGERPRINT: CRC32(header..attribute start) XOR 0x5354554E. */
    {
        uint32_t expect;

        NGX_RTC_TEST_ASSERT_I64_EQ(
            find_attr(resp, (uint32_t)resp_len, NGX_RTC_STUN_ATTR_FINGERPRINT,
                      &hdr_off, &val_off, &val_len), 0);
        NGX_RTC_TEST_ASSERT_I64_EQ(val_len, 4);
        expect = test_crc32(resp, hdr_off) ^ TEST_STUN_FINGERPRINT_XOR;
        NGX_RTC_TEST_ASSERT_U64_EQ(stun_rd_u32(resp + val_off), expect);
    }

    /* The encoded response decodes back with the echoed username. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&round, resp,
                                                   (size_t)resp_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(round.has_username, 1);
    NGX_RTC_TEST_ASSERT_STR_EQ(round.local_ufrag, "svr1");
    NGX_RTC_TEST_ASSERT_STR_EQ(round.remote_ufrag, "cli1");
}
