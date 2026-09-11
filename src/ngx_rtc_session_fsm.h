/**
 * @file    ngx_rtc_session_fsm.h
 * @brief   Session lifecycle state machine (pure state tracking).
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Single-responsibility state machine: this HSM tracks the session lifecycle
 * (NEW -> ICE_BOUND -> DTLS_HANDSHAKE -> SRTP_READY -> CLOSED) and answers
 * state queries only. It performs no side effects. The real lifecycle actions
 * (DTLS create, SRTP key export, source subscribe/unsubscribe, teardown) stay
 * in ngx_rtc_stream_module.c, which owns the nginx connection and the DTLS/SRTP
 * contexts. The stream module drives the machine with
 * ngx_rtc_session_fsm_dispatch_ctx() and polls it with
 * ngx_rtc_session_fsm_is_ready().
 *
 * Transition guards validate the evidence the caller already holds: a STUN
 * binding must arrive on a connection, and the record that starts the DTLS
 * handshake must look like a DTLS record. The guards are defense in depth, not
 * a security boundary: the UDP dispatch in ngx_rtc_stream_module.c has already
 * classified the datagram. A context-free dispatch (context == NULL) counts as
 * "the caller checked", which keeps host tests and future in-process drivers
 * working without inventing fake connections.
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

/*
 * Per-event context handed to the transition guards. The pure-C core cannot
 * name nginx types, so the caller passes its connection opaquely and the guard
 * only tests it for NULL; data/len describe one received DTLS record.
 */
typedef struct
{
    void          *conn;   /* STUN: connection the binding request came in on */
    const uint8_t *data;   /* DTLS: first record in the datagram              */
    size_t         len;    /* DTLS: bytes available at data                   */
} ngx_rtc_session_fsm_ctx_t;

/* Smallest well-formed DTLS record: type + version + epoch + sequence +
 * length. The type range mirrors the UDP dispatch heuristic in
 * ngx_rtc_stream_module.c. */
#define NGX_RTC_SESSION_DTLS_RECORD_MIN  13u
#define NGX_RTC_SESSION_DTLS_TYPE_MIN    20u
#define NGX_RTC_SESSION_DTLS_TYPE_MAX    63u

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

/* Dispatch an event; true when a transition consumed it. */
bool ngx_rtc_session_fsm_dispatch(ngx_rtc_hsm_t *sm,
                                  ngx_rtc_session_event_t event);

/* Dispatch with the per-event evidence the transition guards inspect. */
bool ngx_rtc_session_fsm_dispatch_ctx(ngx_rtc_hsm_t *sm,
                                      ngx_rtc_session_event_t event,
                                      ngx_rtc_session_fsm_ctx_t *ctx);

/* True while the DTLS context still has to be created (NEW or ICE_BOUND). An
 * explicit predicate so callers do not depend on the state enum order. */
bool ngx_rtc_session_fsm_dtls_pending(const ngx_rtc_hsm_t *sm);

ngx_rtc_session_state_t ngx_rtc_session_fsm_get_state(const ngx_rtc_hsm_t *sm);
const char *ngx_rtc_session_fsm_get_state_name(const ngx_rtc_hsm_t *sm);

/* Name a bare state id, with no machine in hand. The shm session skeleton and
 * the stats renderer only ever carry the id -- get_state_name() needs a live
 * HSM, so this is the only lookup they can use. Out-of-range ids (a byte that
 * outlived the binary that wrote it into shared memory) return "UNKNOWN"
 * rather than indexing the file-static state table. */
const char *ngx_rtc_session_fsm_state_name(ngx_rtc_session_state_t state);

bool ngx_rtc_session_fsm_is_ready(const ngx_rtc_hsm_t *sm);

/*
 * Adopt a state learned from outside -- the shm session skeleton's srtp_ready
 * flag -- without running a transition. For a worker that rebuilt the session
 * via attach_from_shm: it never performed the DTLS handshake, so no sequence of
 * events leads it to SRTP_READY, yet is_ready() must answer true there or the
 * send gate and the reaper both treat a live session as a stalled one.
 *
 * Returns false for an unknown state or a NULL machine, leaving it untouched.
 */
bool ngx_rtc_session_fsm_restore(ngx_rtc_hsm_t *sm,
                                 ngx_rtc_session_state_t state);

/* Install the reporter called for events no state handled. The pure-C core has
 * no logger, so the nginx side supplies one; NULL disables reporting. */
void ngx_rtc_session_fsm_set_reporter(ngx_rtc_hsm_t *sm,
                                      ngx_rtc_hsm_action_fn reporter);

#ifdef __cplusplus
}
#endif

#endif /* NGX_RTC_SESSION_FSM_H */
