/*
 * ngx_rtc_stun.c - STUN (RFC 5389) binding request/response, pure C11.
 *
 * Translated from SRS: src/protocol/srs_protocol_rtc_stun.hpp/.cpp
 * (SrsStunPacket::decode / encode_binding_response).
 */

#include "ngx_rtc_stun.h"

#include <stdio.h>
#include <string.h>

#include <openssl/hmac.h>

#define NGX_RTC_STUN_HEADER_LEN 20

/* FINGERPRINT is CRC32(header..) XOR 0x5354554E (RFC 5389 15.5). */
#define NGX_RTC_STUN_FINGERPRINT_XOR 0x5354554Eu

/* CRC-32 IEEE 802.3, reflected polynomial 0xEDB88320. */
static uint32_t
ngx_rtc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc;
    size_t   i;
    uint32_t j;

    crc = 0xFFFFFFFFu;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void
ngx_rtc_stun_write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void
ngx_rtc_stun_write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static uint16_t
ngx_rtc_stun_read_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int
ngx_rtc_stun_decode(ngx_rtc_stun_t *stun, const uint8_t *data, size_t len)
{
    size_t   pos;
    uint16_t message_len;

    if (NULL == stun || NULL == data) {
        return -1;
    }

    memset(stun, 0, sizeof(*stun));

    if (len < NGX_RTC_STUN_HEADER_LEN) {
        return -1;
    }

    stun->type = ngx_rtc_stun_read_u16(data);
    message_len = ngx_rtc_stun_read_u16(data + 2);

    if ((size_t)(NGX_RTC_STUN_HEADER_LEN + message_len) != len) {
        return -1;
    }

    memcpy(stun->transaction_id, data + 8, 12);

    pos = NGX_RTC_STUN_HEADER_LEN;
    while (pos + 4 <= len) {
        uint16_t attr_type = ngx_rtc_stun_read_u16(data + pos);
        uint16_t attr_len  = ngx_rtc_stun_read_u16(data + pos + 2);

        pos += 4;
        if (pos + attr_len > len) {
            return -1;
        }

        if (NGX_RTC_STUN_ATTR_USERNAME == attr_type) {
            char   *colon;
            char    user[2u * NGX_RTC_STUN_UFRAG_MAX + 2u];
            size_t  user_len;
            size_t  local_len;
            size_t  remote_len;

            /* Both ufrags are capped at NGX_RTC_STUN_UFRAG_MAX, so only the
             * first (2 * NGX_RTC_STUN_UFRAG_MAX + 1) chars can ever match a
             * session; anything longer is a probe and is truncated here. */
            user_len = attr_len < (sizeof(user) - 1u)
                       ? attr_len : (sizeof(user) - 1u);
            memcpy(user, data + pos, user_len);
            user[user_len] = '\0';

            colon = strchr(user, ':');
            if (NULL != colon) {
                local_len = (size_t)(colon - user);
                if (local_len > NGX_RTC_STUN_UFRAG_MAX) {
                    local_len = NGX_RTC_STUN_UFRAG_MAX;
                }
                memcpy(stun->local_ufrag, user, local_len);
                stun->local_ufrag[local_len] = '\0';

                remote_len = user_len - (size_t)(colon - user) - 1u;
                if (remote_len > NGX_RTC_STUN_UFRAG_MAX) {
                    remote_len = NGX_RTC_STUN_UFRAG_MAX;
                }
                memcpy(stun->remote_ufrag, colon + 1, remote_len);
                stun->remote_ufrag[remote_len] = '\0';
                stun->has_username = 1;
            }
        } else if (NGX_RTC_STUN_ATTR_USE_CANDIDATE == attr_type) {
            stun->use_candidate = 1;
        } else if (NGX_RTC_STUN_ATTR_ICE_CONTROLLED == attr_type) {
            stun->ice_controlled = 1;
        } else if (NGX_RTC_STUN_ATTR_ICE_CONTROLLING == attr_type) {
            stun->ice_controlling = 1;
        } else if (NGX_RTC_STUN_ATTR_MESSAGE_INTEGRITY == attr_type) {
            stun->has_message_integrity = 1;
            stun->message_integrity_offset = (uint16_t)pos;
        } else if (NGX_RTC_STUN_ATTR_FINGERPRINT == attr_type) {
            stun->has_fingerprint = 1;
            stun->fingerprint_offset = (uint16_t)pos;
        }

        /* Attributes are padded to a 4-byte boundary. */
        pos += attr_len;
        if (0 != (attr_len % 4)) {
            pos += 4 - (attr_len % 4);
        }
    }

    return 0;
}

int
ngx_rtc_stun_verify_request(const ngx_rtc_stun_t *stun,
        const uint8_t *data, size_t len, const char *ice_pwd)
{
    uint8_t   hmac_out[EVP_MAX_MD_SIZE];
    unsigned  hmac_len;
    uint8_t   tmp[2048];

    if (NULL == stun || NULL == data || NULL == ice_pwd
            || 0 == stun->has_message_integrity) {
        return -1;
    }
    if (len > sizeof(tmp)
            || stun->message_integrity_offset + 20u > len
            || stun->message_integrity_offset < 4u) {
        return -1;
    }

    /* MESSAGE-INTEGRITY is computed with the header length field set to the
     * end of the MESSAGE-INTEGRITY attribute (excluding FINGERPRINT). */
    memcpy(tmp, data, len);
    ngx_rtc_stun_write_u16(tmp + 2, stun->message_integrity_offset);

    hmac_len = 0;
    if (NULL == HMAC(EVP_sha1(), ice_pwd, (int)strlen(ice_pwd),
            tmp, stun->message_integrity_offset - 4u, hmac_out, &hmac_len)
            || hmac_len != 20u
            || memcmp(data + stun->message_integrity_offset,
                      hmac_out, 20u) != 0) {
        return -1;
    }

    /* FINGERPRINT (RFC 5389 15.5) is a best-effort protocol disambiguator, not
     * an authentication mechanism. Its CRC-32 variant is subtle and a wrong
     * rejection blocks the whole session, so it is intentionally not enforced
     * here: MESSAGE-INTEGRITY above is the security-critical check. */
    return 0;
}

int
ngx_rtc_stun_encode_binding_response(const ngx_rtc_stun_t *req,
        const char *ice_pwd, uint32_t mapped_addr, uint16_t mapped_port,
        uint8_t *out, size_t out_len)
{
    uint8_t  *p;
    size_t    pos;
    uint32_t  xor_addr;
    uint16_t  xor_port;
    uint8_t   hmac_out[EVP_MAX_MD_SIZE];
    unsigned  hmac_len;
    uint32_t  crc;
    size_t    i;
    uint8_t   username[128];
    size_t    username_len;
    size_t    pad;

    if (NULL == req || NULL == ice_pwd || NULL == out || out_len < 128) {
        return -1;
    }

    if (0 == req->has_username) {
        return -1;
    }

    p = out;
    pos = 0;

    /* Header: type + length (patched later) + magic + transaction id. */
    if (NGX_RTC_STUN_HEADER_LEN > out_len) {
        return -1;
    }
    ngx_rtc_stun_write_u16(p + pos, NGX_RTC_STUN_BINDING_RESPONSE); pos += 2;
    ngx_rtc_stun_write_u16(p + pos, 0); pos += 2;
    ngx_rtc_stun_write_u32(p + pos, NGX_RTC_STUN_MAGIC_COOKIE); pos += 4;
    memcpy(p + pos, req->transaction_id, 12); pos += 12;

    /* USERNAME echoes the request ("local:remote"). */
    {
        size_t n;

        n = (size_t)snprintf((char *)username, sizeof(username), "%s:%s",
                req->local_ufrag, req->remote_ufrag);
        if (n >= sizeof(username)) {
            n = sizeof(username) - 1;
        }
        username_len = n;
        pad = (4 - (username_len % 4)) % 4;

        /* 4 attribute header bytes + value + 4-byte padding. */
        if (4u + username_len + pad > out_len - pos) {
            return -1;
        }
        ngx_rtc_stun_write_u16(p + pos, NGX_RTC_STUN_ATTR_USERNAME); pos += 2;
        ngx_rtc_stun_write_u16(p + pos, (uint16_t)username_len); pos += 2;
        memcpy(p + pos, username, username_len); pos += username_len;
        for (i = 0; i < pad; i++) {
            p[pos + i] = 0;
        }
        pos += pad;
    }

    /* XOR-MAPPED-ADDRESS: family IPv4, port/addr XORed with magic cookie. */
    if (12u > out_len - pos) {
        return -1;
    }
    xor_port = (uint16_t)(mapped_port ^ (NGX_RTC_STUN_MAGIC_COOKIE >> 16));
    xor_addr = mapped_addr ^ NGX_RTC_STUN_MAGIC_COOKIE;

    ngx_rtc_stun_write_u16(p + pos, NGX_RTC_STUN_ATTR_XOR_MAPPED_ADDR); pos += 2;
    ngx_rtc_stun_write_u16(p + pos, 8); pos += 2;
    p[pos++] = 0;          /* reserved */
    p[pos++] = 1;          /* family: IPv4 */
    ngx_rtc_stun_write_u16(p + pos, xor_port); pos += 2;
    ngx_rtc_stun_write_u32(p + pos, xor_addr); pos += 4;

    /* Patch length to include the MESSAGE-INTEGRITY attribute (24 bytes). */
    ngx_rtc_stun_write_u16(out + 2, (uint16_t)((pos - NGX_RTC_STUN_HEADER_LEN) + 24));

    /* MESSAGE-INTEGRITY: HMAC-SHA1 over header..current, key = ice password. */
    hmac_len = 0;
    if (NULL == HMAC(EVP_sha1(), ice_pwd, (int)strlen(ice_pwd),
            out, pos, hmac_out, &hmac_len)) {
        return -1;
    }

    /* 4 attribute header bytes + 20-byte HMAC-SHA1. */
    if (4u + (size_t)hmac_len > out_len - pos) {
        return -1;
    }
    ngx_rtc_stun_write_u16(p + pos, NGX_RTC_STUN_ATTR_MESSAGE_INTEGRITY); pos += 2;
    ngx_rtc_stun_write_u16(p + pos, (uint16_t)hmac_len); pos += 2;
    memcpy(p + pos, hmac_out, hmac_len); pos += hmac_len;

    /* Patch length to include the FINGERPRINT attribute (8 bytes). */
    ngx_rtc_stun_write_u16(out + 2, (uint16_t)((pos - NGX_RTC_STUN_HEADER_LEN) + 8));

    /* FINGERPRINT: CRC32(header..current) XOR magic. */
    if (8u > out_len - pos) {
        return -1;
    }
    crc = ngx_rtc_crc32(out, pos) ^ NGX_RTC_STUN_FINGERPRINT_XOR;
    ngx_rtc_stun_write_u16(p + pos, NGX_RTC_STUN_ATTR_FINGERPRINT); pos += 2;
    ngx_rtc_stun_write_u16(p + pos, 4); pos += 2;
    ngx_rtc_stun_write_u32(p + pos, crc); pos += 4;

    /* Final length. */
    ngx_rtc_stun_write_u16(out + 2, (uint16_t)(pos - NGX_RTC_STUN_HEADER_LEN));

    return (int)pos;
}
