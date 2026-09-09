/*
 * ngx_rtc_session_fsm.h - session lifecycle state machine.
 *
 * Single-responsibility state machine: this HSM tracks the session lifecycle
 * (NEW -> ICE_BOUND -> DTLS_HANDSHAKE -> SRTP_READY -> CLOSED) and answers
 * state queries only. It performs no side effects. The real lifecycle actions
 * (DTLS create, SRTP key export, source subscribe/unsubscribe, teardown) stay
 * in ngx_rtc_stream_module.c, which owns the nginx connection and the DTLS/SRTP
 * contexts. The stream module drives the machine with
 * ngx_rtc_session_fsm_dispatch() and polls it with
 * ngx_rtc_session_fsm_is_ready().
 *
 * There is intentionally no source state machine here: the bridge module's
 * ctx->publishing flag already describes the RTMP publish lifecycle, and a
 * second machine would only duplicate that state.
 */

#ifndef NGX_RTC_SESSION_FSM_H
#define NGX_RTC_SESSION_FSM_H

#include "ngx_rtc_hsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration (struct definition lives in ngx_rtc_core.h). */
typedef struct ngx_rtc_session_s ngx_rtc_session_t;

/* ============================================================================
 * Session state machine
 * ============================================================================ */

/*
 * Hierarchy:
 *   SESSION (root)
 *     |- NEW             HTTP signaling created, waiting for STUN binding
 *     |- ICE_BOUND       STUN bound conn, waiting for DTLS      (conn != NULL)
 *     |- DTLS_HANDSHAKE  DTLS handshake in progress             (dtls_created)
 *     |- SRTP_READY      keys ready, subscribed to source       (srtp_ready)
 *     |- CLOSED          timeout / disconnect / RTCP BYE
 */

/*
 * Events accepted by the session machine.
 */
typedef enum
{
    NGX_RTC_SESSION_EVT_STUN_BINDING       = 0x01U,
    NGX_RTC_SESSION_EVT_DTLS_PACKET        = 0x02U,
    NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE = 0x03U,
    NGX_RTC_SESSION_EVT_SRTP_READY         = 0x04U,
    NGX_RTC_SESSION_EVT_TIMEOUT            = 0x10U,
    NGX_RTC_SESSION_EVT_CLOSE              = 0x20U,
    NGX_RTC_SESSION_EVT_RTCP_BYE           = 0x30U
} ngx_rtc_session_event_t;

/*
 * Queryable state id (the state objects are file-static).
 */
typedef enum
{
    NGX_RTC_SESSION_STATE_UNKNOWN = 0,
    NGX_RTC_SESSION_STATE_NEW,
    NGX_RTC_SESSION_STATE_ICE_BOUND,
    NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE,
    NGX_RTC_SESSION_STATE_SRTP_READY,
    NGX_RTC_SESSION_STATE_CLOSED
} ngx_rtc_session_state_t;

/* Hierarchy depth is 2 (root -> leaf); size the path buffer accordingly. */
#define NGX_RTC_SESSION_FSM_MAX_DEPTH 2U

void ngx_rtc_session_fsm_init(ngx_rtc_hsm_t *sm,
                              const ngx_rtc_hsm_state_t **path_buf,
                              uint8_t path_buf_size,
                              ngx_rtc_session_t *sess);
void ngx_rtc_session_fsm_deinit(ngx_rtc_hsm_t *sm);
bool ngx_rtc_session_fsm_dispatch(ngx_rtc_hsm_t *sm,
                                  ngx_rtc_session_event_t event);
ngx_rtc_session_state_t ngx_rtc_session_fsm_get_state(const ngx_rtc_hsm_t *sm);
const char *ngx_rtc_session_fsm_get_state_name(const ngx_rtc_hsm_t *sm);
bool ngx_rtc_session_fsm_is_ready(const ngx_rtc_hsm_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* NGX_RTC_SESSION_FSM_H */
