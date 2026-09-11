/**
 * @file    ngx_rtc_srtp.h
 * @brief   SRTP (RFC 3711) protect/unprotect via libsrtp2, pure C11.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Translated from SRS: src/app/srs_app_rtc_dtls.cpp (SrsSRTP).
 */

#ifndef NGX_RTC_SRTP_H
#define NGX_RTC_SRTP_H

#include <stdint.h>
#include <stddef.h>

#include <srtp2/srtp.h>

typedef struct {
    srtp_t recv_ctx;
    srtp_t send_ctx;
} ngx_rtc_srtp_t;

/*
 * Initialize libsrtp2 (global, idempotent). Return 0 on success.
 */
int ngx_rtc_srtp_global_init(void);

/*
 * Create recv/send SRTP contexts.
 *   recv_key / send_key : 30 bytes each (key 16 + salt 14), from DTLS export.
 * Return 0 on success, -1 on error.
 */
int ngx_rtc_srtp_create(ngx_rtc_srtp_t *srtp, const uint8_t *recv_key, const uint8_t *send_key);

/*
 * Protect an RTP packet in place (adds auth tag). nb points at the buffer
 * length in, updated to the protected length out. Return 0 on success.
 */
int ngx_rtc_srtp_protect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb);

/*
 * Unprotect an RTP packet in place (strips auth tag). Return 0 on success.
 */
int ngx_rtc_srtp_unprotect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb);

/*
 * Protect an RTCP packet in place (adds auth tag). nb points at the buffer
 * length in, updated to the protected length out. Return 0 on success.
 */
int ngx_rtc_srtp_protect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb);

/*
 * Unprotect an RTCP packet in place (strips auth tag). Return 0 on success.
 */
int ngx_rtc_srtp_unprotect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb);

void ngx_rtc_srtp_destroy(ngx_rtc_srtp_t *srtp);

#endif /* NGX_RTC_SRTP_H */
