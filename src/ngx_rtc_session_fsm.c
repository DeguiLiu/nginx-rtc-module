/*
 * ngx_rtc_session_fsm.c - session lifecycle state machine (pure state tracking).
 *
 * Tracks the session lifecycle NEW -> ICE_BOUND -> DTLS_HANDSHAKE ->
 * SRTP_READY -> CLOSED. CLOSE/TIMEOUT/RTCP_BYE are handled at the root so the
 * session closes from any state.
 *
 * The states carry no entry/exit actions: this file only advances the machine
 * and answers state queries. The real lifecycle actions (DTLS create, SRTP key
 * export, subscribe, unsubscribe, teardown) stay in
 * ngx_rtc_stream_module.c, which owns the nginx connection and the DTLS/SRTP
 * contexts. Keeping this HSM action-free makes it the single source of truth
 * for "what state are we in" without a drift-prone copy of "what to do".
 *
 * Guards are the one exception to that rule: they are pure predicates over the
 * caller-supplied event context and never mutate anything.
 */

#include "ngx_rtc_session_fsm.h"

/* ============================================================================
 * State object forward declarations (transition tables reference them before
 * their definitions below).
 * ============================================================================ */

static const ngx_rtc_hsm_state_t s_session_top;
static const ngx_rtc_hsm_state_t s_session_new;
static const ngx_rtc_hsm_state_t s_session_ice_bound;
static const ngx_rtc_hsm_state_t s_session_dtls_handshake;
static const ngx_rtc_hsm_state_t s_session_srtp_ready;
static const ngx_rtc_hsm_state_t s_session_closed;

/* ============================================================================
 * Transition guards
 *
 * Both guards only inspect the per-event context; the opaque session pointer in
 * sm->user_data is deliberately not dereferenced, so this unit stays libc-only
 * and host-testable. A NULL context means the caller already enforced the
 * precondition (see the header) and the guard abstains.
 * ============================================================================ */

/*
 * STUN binding: the session is about to be pinned to a connection, so the ICE
 * request must have arrived on one. A *different* 5-tuple is deliberately not
 * rejected: ICE migration re-nominates a pair mid-session and the stream module
 * re-points sess->conn, which it owns. This unit cannot see sess->conn (the
 * session is opaque here), so the identity check stays with the caller.
 */
static bool
s_session_binding_guard(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    const ngx_rtc_session_fsm_ctx_t *ctx = event->context;

    (void) sm;

    if (NULL == ctx) {
        return true;
    }

    return (NULL != ctx->conn);
}

/*
 * DTLS handshake start: only a well-formed record may create a DTLS context.
 * The UDP dispatch has already looked at byte 0, so this is the length and
 * range check it could not do cheaply for a short datagram.
 */
static bool
s_session_dtls_guard(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    const ngx_rtc_session_fsm_ctx_t *ctx = event->context;

    (void) sm;

    if (NULL == ctx) {
        return true;
    }

    if (NULL == ctx->data || ctx->len < NGX_RTC_SESSION_DTLS_RECORD_MIN) {
        return false;
    }

    return (ctx->data[0] >= NGX_RTC_SESSION_DTLS_TYPE_MIN
            && ctx->data[0] <= NGX_RTC_SESSION_DTLS_TYPE_MAX);
}

/* ============================================================================
 * Session transition tables
 * ============================================================================ */

/* Root fallback: any state that does not handle CLOSE/TIMEOUT/RTCP_BYE bubbles
 * up to the root and closes the session. */
static const ngx_rtc_hsm_transition_t s_session_top_transitions[] =
{
    /* event_id,                        target,             guard,  action, type */
    { NGX_RTC_SESSION_EVT_CLOSE,        &s_session_closed,   NULL,   NULL,   NGX_RTC_HSM_TRANSITION_EXTERNAL },
    { NGX_RTC_SESSION_EVT_TIMEOUT,      &s_session_closed,   NULL,   NULL,   NGX_RTC_HSM_TRANSITION_EXTERNAL },
    { NGX_RTC_SESSION_EVT_RTCP_BYE,     &s_session_closed,   NULL,   NULL,   NGX_RTC_HSM_TRANSITION_EXTERNAL },
};

/* --- NEW transitions --- */
static const ngx_rtc_hsm_transition_t s_session_new_transitions[] =
{
    { NGX_RTC_SESSION_EVT_STUN_BINDING, &s_session_ice_bound,
      s_session_binding_guard, NULL, NGX_RTC_HSM_TRANSITION_EXTERNAL },
};

/* --- ICE_BOUND transitions ---
 * Bindings repeat for the whole session (ICE retransmit, then consent checks),
 * so a repeat is swallowed in place instead of being reported unhandled. */
static const ngx_rtc_hsm_transition_t s_session_ice_bound_transitions[] =
{
    { NGX_RTC_SESSION_EVT_STUN_BINDING, NULL, s_session_binding_guard, NULL,
      NGX_RTC_HSM_TRANSITION_INTERNAL },
    { NGX_RTC_SESSION_EVT_DTLS_PACKET,  &s_session_dtls_handshake,
      s_session_dtls_guard, NULL, NGX_RTC_HSM_TRANSITION_EXTERNAL },
};

/* --- DTLS_HANDSHAKE transitions ---
 * DTLS_HANDSHAKE_DONE is the normal path; SRTP_READY is an alternate path for
 * callers that already finished SRTP setup before dispatching. */
static const ngx_rtc_hsm_transition_t s_session_dtls_handshake_transitions[] =
{
    { NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE, &s_session_srtp_ready, NULL, NULL, NGX_RTC_HSM_TRANSITION_EXTERNAL },
    { NGX_RTC_SESSION_EVT_SRTP_READY,          &s_session_srtp_ready, NULL, NULL, NGX_RTC_HSM_TRANSITION_EXTERNAL },
    { NGX_RTC_SESSION_EVT_STUN_BINDING,        NULL, s_session_binding_guard,
      NULL, NGX_RTC_HSM_TRANSITION_INTERNAL },
};

/* --- SRTP_READY transitions --- */
static const ngx_rtc_hsm_transition_t s_session_srtp_ready_transitions[] =
{
    /* Idempotent re-announcement; already ready. */
    { NGX_RTC_SESSION_EVT_SRTP_READY,   NULL,                 NULL,  NULL,   NGX_RTC_HSM_TRANSITION_INTERNAL },
    /* ICE consent keepalives arrive here for the life of the session. */
    { NGX_RTC_SESSION_EVT_STUN_BINDING, NULL, s_session_binding_guard, NULL,
      NGX_RTC_HSM_TRANSITION_INTERNAL },
};

/* --- CLOSED transitions ---
 * Swallow close-class events so the root fallback never re-runs once the
 * session is already closed. */
static const ngx_rtc_hsm_transition_t s_session_closed_transitions[] =
{
    { NGX_RTC_SESSION_EVT_CLOSE,        NULL,                 NULL,  NULL,   NGX_RTC_HSM_TRANSITION_INTERNAL },
    { NGX_RTC_SESSION_EVT_TIMEOUT,      NULL,                 NULL,  NULL,   NGX_RTC_HSM_TRANSITION_INTERNAL },
    { NGX_RTC_SESSION_EVT_RTCP_BYE,     NULL,                 NULL,  NULL,   NGX_RTC_HSM_TRANSITION_INTERNAL },
};

/* ============================================================================
 * Session state definitions
 *
 * Hierarchy:
 *   SESSION (root) -> NEW, ICE_BOUND, DTLS_HANDSHAKE, SRTP_READY, CLOSED
 * ============================================================================ */

static const ngx_rtc_hsm_state_t s_session_top =
{
    .parent          = NULL,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_top_transitions,
    .num_transitions = sizeof(s_session_top_transitions) /
                       sizeof(s_session_top_transitions[0]),
    .name            = "SESSION",
};

static const ngx_rtc_hsm_state_t s_session_new =
{
    .parent          = &s_session_top,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_new_transitions,
    .num_transitions = sizeof(s_session_new_transitions) /
                       sizeof(s_session_new_transitions[0]),
    .name            = "NEW",
};

static const ngx_rtc_hsm_state_t s_session_ice_bound =
{
    .parent          = &s_session_top,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_ice_bound_transitions,
    .num_transitions = sizeof(s_session_ice_bound_transitions) /
                       sizeof(s_session_ice_bound_transitions[0]),
    .name            = "ICE_BOUND",
};

static const ngx_rtc_hsm_state_t s_session_dtls_handshake =
{
    .parent          = &s_session_top,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_dtls_handshake_transitions,
    .num_transitions = sizeof(s_session_dtls_handshake_transitions) /
                       sizeof(s_session_dtls_handshake_transitions[0]),
    .name            = "DTLS_HANDSHAKE",
};

static const ngx_rtc_hsm_state_t s_session_srtp_ready =
{
    .parent          = &s_session_top,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_srtp_ready_transitions,
    .num_transitions = sizeof(s_session_srtp_ready_transitions) /
                       sizeof(s_session_srtp_ready_transitions[0]),
    .name            = "SRTP_READY",
};

static const ngx_rtc_hsm_state_t s_session_closed =
{
    .parent          = &s_session_top,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = s_session_closed_transitions,
    .num_transitions = sizeof(s_session_closed_transitions) /
                       sizeof(s_session_closed_transitions[0]),
    .name            = "CLOSED",
};

/* ============================================================================
 * Session wrapper API
 * ============================================================================ */

void ngx_rtc_session_fsm_init(ngx_rtc_hsm_t *sm,
                              const ngx_rtc_hsm_state_t **path_buf,
                              uint8_t path_buf_size,
                              ngx_rtc_session_t *sess)
{
    ngx_rtc_hsm_init(sm, &s_session_new, path_buf, path_buf_size, sess, NULL);
}

void ngx_rtc_session_fsm_deinit(ngx_rtc_hsm_t *sm)
{
    ngx_rtc_hsm_deinit(sm);
}

bool ngx_rtc_session_fsm_dispatch(ngx_rtc_hsm_t *sm,
                                  ngx_rtc_session_event_t event)
{
    return ngx_rtc_session_fsm_dispatch_ctx(sm, event, NULL);
}

bool ngx_rtc_session_fsm_dispatch_ctx(ngx_rtc_hsm_t *sm,
                                      ngx_rtc_session_event_t event,
                                      ngx_rtc_session_fsm_ctx_t *ctx)
{
    ngx_rtc_hsm_event_t ev;

    if (NULL == sm)
    {
        return false;
    }

    ev.id = (uint32_t)event;
    ev.context = ctx;   /* opaque to the engine; the guards cast it back */

    return ngx_rtc_hsm_dispatch(sm, &ev);
}

bool ngx_rtc_session_fsm_dtls_pending(const ngx_rtc_hsm_t *sm)
{
    if (NULL == sm)
    {
        return false;
    }

    return (ngx_rtc_hsm_is_in_state(sm, &s_session_new)
            || ngx_rtc_hsm_is_in_state(sm, &s_session_ice_bound));
}

ngx_rtc_session_state_t ngx_rtc_session_fsm_get_state(const ngx_rtc_hsm_t *sm)
{
    if (NULL == sm)
    {
        return NGX_RTC_SESSION_STATE_UNKNOWN;
    }

    if (ngx_rtc_hsm_is_in_state(sm, &s_session_srtp_ready))
    {
        return NGX_RTC_SESSION_STATE_SRTP_READY;
    }
    if (ngx_rtc_hsm_is_in_state(sm, &s_session_dtls_handshake))
    {
        return NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE;
    }
    if (ngx_rtc_hsm_is_in_state(sm, &s_session_ice_bound))
    {
        return NGX_RTC_SESSION_STATE_ICE_BOUND;
    }
    if (ngx_rtc_hsm_is_in_state(sm, &s_session_closed))
    {
        return NGX_RTC_SESSION_STATE_CLOSED;
    }
    if (ngx_rtc_hsm_is_in_state(sm, &s_session_new))
    {
        return NGX_RTC_SESSION_STATE_NEW;
    }

    return NGX_RTC_SESSION_STATE_UNKNOWN;
}

const char *ngx_rtc_session_fsm_get_state_name(const ngx_rtc_hsm_t *sm)
{
    return ngx_rtc_hsm_get_current_state_name(sm);
}

/*
 * Map a public state id to its state object. Kept next to get_state() so the
 * two enumerations stay in step: a state added there must be added here too.
 */
static const ngx_rtc_hsm_state_t *
ngx_rtc_session_fsm_state_object(ngx_rtc_session_state_t state)
{
    switch (state)
    {
    case NGX_RTC_SESSION_STATE_NEW:
        return &s_session_new;
    case NGX_RTC_SESSION_STATE_ICE_BOUND:
        return &s_session_ice_bound;
    case NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE:
        return &s_session_dtls_handshake;
    case NGX_RTC_SESSION_STATE_SRTP_READY:
        return &s_session_srtp_ready;
    case NGX_RTC_SESSION_STATE_CLOSED:
        return &s_session_closed;
    default:
        return NULL;
    }
}

/*
 * Name a bare state id, with no machine in hand. Delegates to the same state
 * objects the running HSM uses, so a name can never drift from the machine;
 * an id outside the enum (a byte that outlived the binary that wrote it into
 * shared memory) yields the state_object() NULL and reads as "UNKNOWN" instead
 * of indexing the table out of bounds.
 */
const char *
ngx_rtc_session_fsm_state_name(ngx_rtc_session_state_t state)
{
    const ngx_rtc_hsm_state_t *object;

    object = ngx_rtc_session_fsm_state_object(state);

    return (NULL != object) ? object->name : "UNKNOWN";
}

bool ngx_rtc_session_fsm_restore(ngx_rtc_hsm_t *sm,
                                 ngx_rtc_session_state_t state)
{
    /* ngx_rtc_hsm_restore_state() rejects a NULL target, so UNKNOWN and NULL
     * `sm` both fall out as false without touching the machine. */
    return ngx_rtc_hsm_restore_state(
            sm, ngx_rtc_session_fsm_state_object(state));
}

bool ngx_rtc_session_fsm_is_ready(const ngx_rtc_hsm_t *sm)
{
    if (NULL == sm)
    {
        return false;
    }
    return ngx_rtc_hsm_is_in_state(sm, &s_session_srtp_ready);
}

void ngx_rtc_session_fsm_set_reporter(ngx_rtc_hsm_t *sm,
                                      ngx_rtc_hsm_action_fn reporter)
{
    if (NULL != sm)
    {
        sm->unhandled_event_hook = reporter;
    }
}
