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
}
