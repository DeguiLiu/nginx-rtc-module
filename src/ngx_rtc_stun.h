/*
 * ngx_rtc_stun.h - STUN (RFC 5389) binding request/response, pure C11.
 *
 * Translated from SRS: src/protocol/srs_protocol_rtc_stun.hpp/.cpp
 * (SrsStunPacket::decode / encode_binding_response).
 * Only the server side (ICE-lite) is needed: decode a BindingRequest and
 * encode a BindingResponse with XOR-MAPPED-ADDRESS + MESSAGE-INTEGRITY +
 * FINGERPRINT.
 */

#ifndef NGX_RTC_STUN_H
#define NGX_RTC_STUN_H

#include <stdint.h>
#include <stddef.h>

#define NGX_RTC_STUN_MAGIC_COOKIE 0x2112A442u

/* Maximum length of one ICE ufrag carried in a STUN USERNAME. The server
 * allocates 8-byte ufrags and well-behaved peers use <= 16 bytes, so this cap
 * keeps the BindingResponse well under any caller buffer while still matching
 * every real session. */
#define NGX_RTC_STUN_UFRAG_MAX 16u

/* Message types (RFC 5389). */
#define NGX_RTC_STUN_BINDING_REQUEST   0x0001
#define NGX_RTC_STUN_BINDING_RESPONSE  0x0101

/* Attribute types (RFC 5389). */
#define NGX_RTC_STUN_ATTR_USERNAME          0x0006
#define NGX_RTC_STUN_ATTR_MESSAGE_INTEGRITY 0x0008
#define NGX_RTC_STUN_ATTR_XOR_MAPPED_ADDR   0x0020
#define NGX_RTC_STUN_ATTR_USE_CANDIDATE     0x0025
#define NGX_RTC_STUN_ATTR_FINGERPRINT       0x8028
#define NGX_RTC_STUN_ATTR_ICE_CONTROLLED    0x8029
#define NGX_RTC_STUN_ATTR_ICE_CONTROLLING   0x802A

/* Decoded STUN packet. */
typedef struct {
    uint16_t type;
    uint8_t  transaction_id[12];

    /* Parsed attributes. */
    uint8_t has_username;
    char    local_ufrag[NGX_RTC_STUN_UFRAG_MAX + 1u];   /* left of ':' in username */
    char    remote_ufrag[NGX_RTC_STUN_UFRAG_MAX + 1u];  /* right of ':' in username */
    uint8_t use_candidate;
    uint8_t ice_controlled;
    uint8_t ice_controlling;

    /* Offsets of MESSAGE-INTEGRITY / FINGERPRINT values, plus presence. */
    uint8_t  has_message_integrity;
    uint16_t message_integrity_offset;
    uint8_t  has_fingerprint;
    uint16_t fingerprint_offset;
} ngx_rtc_stun_t;

/* Decode a STUN packet. Return 0 on success, -1 on malformed packet. */
int ngx_rtc_stun_decode(ngx_rtc_stun_t *stun, const uint8_t *data, size_t len);

/*
 * Verify a BindingRequest's short-term credential MESSAGE-INTEGRITY and
 * FINGERPRINT against the local ICE password. Return 0 on success, -1 on
 * missing/invalid integrity data.
 */
int ngx_rtc_stun_verify_request(const ngx_rtc_stun_t *stun,
        const uint8_t *data, size_t len, const char *ice_pwd);

/*
 * Encode a BindingResponse for a decoded BindingRequest.
 *   ice_pwd      : local ICE password (short-term credential), HMAC-SHA1 key.
 *   mapped_addr  : host byte order IPv4 address of the peer.
 *   mapped_port  : host byte order UDP port of the peer.
 *   out / out_len: caller-provided buffer (>= 256 bytes is enough).
 * Return bytes written, or -1 on error.
 */
int ngx_rtc_stun_encode_binding_response(const ngx_rtc_stun_t *req,
        const char *ice_pwd, uint32_t mapped_addr, uint16_t mapped_port,
        uint8_t *out, size_t out_len);

#endif /* NGX_RTC_STUN_H */
