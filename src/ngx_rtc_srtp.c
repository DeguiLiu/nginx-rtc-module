/**
 * @file    ngx_rtc_srtp.c
 * @brief   SRTP (RFC 3711) protect/unprotect via libsrtp2, pure C11.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Translated from SRS: src/app/srs_app_rtc_dtls.cpp (SrsSRTP).
 */

#include "ngx_rtc_srtp.h"

#include <string.h>

#include "ngx_rtc_dtls.h"

static int ngx_rtc_srtp_inited = 0;

int
ngx_rtc_srtp_global_init(void)
{
    if (0 != ngx_rtc_srtp_inited) {
        return 0;
    }

    if (srtp_init() != srtp_err_status_ok) {
        return -1;
    }

    ngx_rtc_srtp_inited = 1;
    return 0;
}

int
ngx_rtc_srtp_create(ngx_rtc_srtp_t *srtp, const uint8_t *recv_key, const uint8_t *send_key)
{
    srtp_policy_t    policy;
    srtp_err_status_t r0;

    if (NULL == srtp || NULL == recv_key || NULL == send_key) {
        return -1;
    }

    memset(srtp, 0, sizeof(*srtp));
    memset(&policy, 0, sizeof(policy));

    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
    policy.ssrc.value = 0;
    policy.window_size = 8192;
    policy.allow_repeat_tx = 1;
    policy.next = NULL;

    /* Recv context: any inbound SSRC. */
    policy.ssrc.type = ssrc_any_inbound;
    policy.key = (uint8_t *)recv_key;
    r0 = srtp_create(&srtp->recv_ctx, &policy);
    if (r0 != srtp_err_status_ok) {
        return -1;
    }

    /* Send context: any outbound SSRC. */
    policy.ssrc.type = ssrc_any_outbound;
    policy.key = (uint8_t *)send_key;
    r0 = srtp_create(&srtp->send_ctx, &policy);
    if (r0 != srtp_err_status_ok) {
        srtp_dealloc(srtp->recv_ctx);
        srtp->recv_ctx = NULL;
        return -1;
    }

    return 0;
}

int
ngx_rtc_srtp_protect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    if (NULL == srtp || NULL == srtp->send_ctx || NULL == packet || NULL == nb) {
        return -1;
    }

    if (srtp_protect(srtp->send_ctx, packet, nb) != srtp_err_status_ok) {
        return -1;
    }

    return 0;
}

int
ngx_rtc_srtp_unprotect_rtp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    if (NULL == srtp || NULL == srtp->recv_ctx || NULL == packet || NULL == nb) {
        return -1;
    }

    if (srtp_unprotect(srtp->recv_ctx, packet, nb) != srtp_err_status_ok) {
        return -1;
    }

    return 0;
}

int
ngx_rtc_srtp_protect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    if (NULL == srtp || NULL == srtp->send_ctx || NULL == packet || NULL == nb) {
        return -1;
    }

    if (srtp_protect_rtcp(srtp->send_ctx, packet, nb) != srtp_err_status_ok) {
        return -1;
    }

    return 0;
}

int
ngx_rtc_srtp_unprotect_rtcp(ngx_rtc_srtp_t *srtp, uint8_t *packet, int *nb)
{
    if (NULL == srtp || NULL == srtp->recv_ctx || NULL == packet || NULL == nb) {
        return -1;
    }

    if (srtp_unprotect_rtcp(srtp->recv_ctx, packet, nb) != srtp_err_status_ok) {
        return -1;
    }

    return 0;
}

void
ngx_rtc_srtp_destroy(ngx_rtc_srtp_t *srtp)
{
    if (NULL == srtp) {
        return;
    }

    if (NULL != srtp->recv_ctx) {
        srtp_dealloc(srtp->recv_ctx);
        srtp->recv_ctx = NULL;
    }

    if (NULL != srtp->send_ctx) {
        srtp_dealloc(srtp->send_ctx);
        srtp->send_ctx = NULL;
    }
}
