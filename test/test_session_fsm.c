/*
 * test_session_fsm.c - host unit tests for ngx_rtc_session_fsm.c: the session
 * lifecycle (NEW -> ICE_BOUND -> DTLS_HANDSHAKE -> SRTP_READY -> CLOSED) and
 * close-class convergence from any state.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_core.h"
#include "ngx_rtc_session_fsm.h"

static ngx_rtc_session_t          g_sess;
static ngx_rtc_hsm_t              g_session_sm;
static const ngx_rtc_hsm_state_t *g_session_path[NGX_RTC_SESSION_FSM_MAX_DEPTH];

static ngx_uint_t                 g_unhandled_count;
static uint32_t                   g_unhandled_id;

static void
session_fsm_unhandled(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void) sm;
    g_unhandled_count++;
    g_unhandled_id = event->id;
}

NGX_RTC_TEST(session_fsm_lifecycle)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_NEW);
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_session_fsm_get_state_name(&g_session_sm),
                               "NEW");
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_is_ready(&g_session_sm));

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_ICE_BOUND);

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_SRTP_READY);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_is_ready(&g_session_sm));

    /* Idempotent re-announcement stays in SRTP_READY. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_SRTP_READY));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_SRTP_READY);

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_CLOSE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_is_ready(&g_session_sm));

    /* Already closed: close-class events are swallowed, state is terminal. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_CLOSE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

NGX_RTC_TEST(session_fsm_close_converges_from_any_state)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));

    /* NEW -> CLOSE. */
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_CLOSE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    /* ICE_BOUND -> TIMEOUT. */
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_TIMEOUT));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    /* DTLS_HANDSHAKE -> RTCP_BYE. */
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_RTCP_BYE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    /* SRTP_READY -> CLOSE. */
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&g_session_sm, NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_CLOSE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

NGX_RTC_TEST(fsm_null_guards)
{
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dispatch(
        NULL, NGX_RTC_SESSION_EVT_CLOSE));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(NULL),
                               NGX_RTC_SESSION_STATE_UNKNOWN);
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_session_fsm_get_state_name(NULL),
                               "Unknown");
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_is_ready(NULL));
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dtls_pending(NULL));
}

/*
 * The binding and DTLS transitions carry guards that validate the evidence the
 * caller supplies. A rejected binding must not claim ICE_BOUND, and a datagram
 * that is too short (or not a DTLS content type) must not start a handshake.
 */
NGX_RTC_TEST(session_fsm_guards_gate_binding_and_dtls)
{
    ngx_rtc_session_fsm_ctx_t ctx;
    u_char                    record[16];

    (void)memset(&g_sess, 0, sizeof(g_sess));
    (void)memset(&ctx, 0, sizeof(ctx));
    (void)memset(record, 0, sizeof(record));
    record[0] = 22; /* DTLS handshake */

    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);

    /* No connection in the context: ICE_BOUND promises one, so refuse. */
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_NEW);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dtls_pending(&g_session_sm));

    ctx.conn = record; /* opaque identity; the guard only tests for NULL */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_ICE_BOUND);

    /* Retransmitted binding / later consent check: handled, state unchanged. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_ICE_BOUND);

    /* Too short to be a DTLS record: no context may be created from it. */
    ctx.data = record;
    ctx.len = 5;
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_ICE_BOUND);

    /* Out-of-range content type: same verdict. */
    record[0] = 0x80;
    ctx.len = sizeof(record);
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_ICE_BOUND);
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dtls_pending(&g_session_sm));

    /* A well-formed record starts the handshake. */
    record[0] = 22;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dtls_pending(&g_session_sm));

    /* Ready sessions keep answering consent checks without leaving the state. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE));
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch_ctx(
        &g_session_sm, NGX_RTC_SESSION_EVT_STUN_BINDING, &ctx));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_SRTP_READY);

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

/*
 * An event no state handles used to vanish: the dispatch returned false and the
 * caller ignored it. The reporter makes the stall visible.
 */
NGX_RTC_TEST(session_fsm_reports_unhandled_events)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));
    g_unhandled_count = 0;
    g_unhandled_id = 0;

    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    ngx_rtc_session_fsm_set_reporter(&g_session_sm, session_fsm_unhandled);

    /* NEW has no DTLS transition and the root does not handle the event. */
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_DTLS_PACKET));
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) g_unhandled_count, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) g_unhandled_id,
                               NGX_RTC_SESSION_EVT_DTLS_PACKET);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_NEW);

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

/*
 * The mirror-worker case: this worker rebuilt a session from the shm skeleton
 * (attach_from_shm) and never performed the DTLS handshake -- only the owning
 * worker does. Without adopting the state the owner already reached, is_ready()
 * stays false, which (a) makes the send gate in ngx_rtc_core.c drop every
 * packet and (b) makes the reaper use handshake_timeout instead of
 * ready_timeout, so the session is torn down while it is in fact live.
 */
NGX_RTC_TEST(session_fsm_restore_adopts_shm_state)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));
    g_unhandled_count = 0;
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);
    ngx_rtc_session_fsm_set_reporter(&g_session_sm, session_fsm_unhandled);

    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_is_ready(&g_session_sm));

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_restore(
        &g_session_sm, NGX_RTC_SESSION_STATE_SRTP_READY));

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_is_ready(&g_session_sm));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_SRTP_READY);
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_get_state_name(&g_session_sm), "SRTP_READY");

    /* Adopting a state is not a transition: nothing was refused on the way. */
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) g_unhandled_count, 0);

    /* The adopted machine still converges on close like any other. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_session_fsm_dispatch(
        &g_session_sm, NGX_RTC_SESSION_EVT_TIMEOUT));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_CLOSED);

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

/* An unusable target is refused, leaving the machine where it was. */
NGX_RTC_TEST(session_fsm_restore_rejects_unknown)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));
    ngx_rtc_session_fsm_init(&g_session_sm, g_session_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_sess);

    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_restore(
        &g_session_sm, NGX_RTC_SESSION_STATE_UNKNOWN));
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_restore(NULL,
        NGX_RTC_SESSION_STATE_SRTP_READY));

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_fsm_get_state(&g_session_sm),
                               NGX_RTC_SESSION_STATE_NEW);
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_session_fsm_is_ready(&g_session_sm));

    ngx_rtc_session_fsm_deinit(&g_session_sm);
}

/*
 * The name lookup for a bare state id, with no HSM instance in hand. The shm
 * registry and the stats renderer only ever carry the id, so this is the only
 * way they can name a state -- get_state_name() needs a live machine.
 *
 * The out-of-range cases are not decoration: the id travels through shared
 * memory, so the renderer can be handed a byte written by an older binary.
 * "UNKNOWN" is what keeps that from becoming an arbitrary out-of-bounds read
 * through the file-static state table.
 */
NGX_RTC_TEST(session_fsm_state_name_maps_ids)
{
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_UNKNOWN), "UNKNOWN");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_NEW), "NEW");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_ICE_BOUND),
        "ICE_BOUND");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE),
        "DTLS_HANDSHAKE");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_SRTP_READY),
        "SRTP_READY");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name(NGX_RTC_SESSION_STATE_CLOSED), "CLOSED");

    /* Beyond the enum: a byte that outlived its writer. */
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name((ngx_rtc_session_state_t) 200),
        "UNKNOWN");
    NGX_RTC_TEST_ASSERT_STR_EQ(
        ngx_rtc_session_fsm_state_name((ngx_rtc_session_state_t) -1),
        "UNKNOWN");
}
