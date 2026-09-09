/*
 * test_hsm.c - host unit tests for the ngx_rtc_hsm.c hierarchical state
 * machine engine: guard gating, INTERNAL transitions, external entry/exit
 * ordering, self-transition, event bubbling and is_in_state().
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_hsm.h"

enum
{
    EV_GO = 1,
    EV_INT = 2,
    EV_SELF = 3
};

static int  g_guard_allow;
static char g_log[64];
static int  g_log_len;
static int  g_unhandled_count;

static void log_clear(void)
{
    g_log_len = 0;
    g_log[0] = '\0';
    g_unhandled_count = 0;
}

static void log_append(char c)
{
    if (g_log_len < (int)sizeof(g_log) - 1)
    {
        g_log[g_log_len] = c;
        g_log_len++;
        g_log[g_log_len] = '\0';
    }
}

static void enter_a(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    log_append('a');
}

static void exit_a(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    log_append('A');
}

static void enter_b(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    log_append('b');
}

static void exit_b(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    log_append('B');
}

static void act_internal(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    log_append('i');
}

static void on_unhandled(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    g_unhandled_count++;
    log_append('u');
}

static bool guard_go(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    (void)sm;
    (void)event;
    return (0 != g_guard_allow);
}

static const ngx_rtc_hsm_state_t s_top;
static const ngx_rtc_hsm_state_t s_a;
static const ngx_rtc_hsm_state_t s_b;

static const ngx_rtc_hsm_transition_t s_a_trans[] =
{
    { EV_GO,  &s_b,   guard_go, NULL,        NGX_RTC_HSM_TRANSITION_EXTERNAL },
};

static const ngx_rtc_hsm_transition_t s_b_trans[] =
{
    { EV_INT,  NULL,  NULL,     act_internal, NGX_RTC_HSM_TRANSITION_INTERNAL },
    { EV_SELF, &s_b,  NULL,     NULL,         NGX_RTC_HSM_TRANSITION_EXTERNAL },
    { EV_GO,   &s_a,  NULL,     NULL,         NGX_RTC_HSM_TRANSITION_EXTERNAL },
};

static const ngx_rtc_hsm_state_t s_top =
{
    .parent          = NULL,
    .entry_action    = NULL,
    .exit_action     = NULL,
    .transitions     = NULL,
    .num_transitions = 0,
    .name            = "TOP",
};

static const ngx_rtc_hsm_state_t s_a =
{
    .parent          = &s_top,
    .entry_action    = enter_a,
    .exit_action     = exit_a,
    .transitions     = s_a_trans,
    .num_transitions = sizeof(s_a_trans) / sizeof(s_a_trans[0]),
    .name            = "A",
};

static const ngx_rtc_hsm_state_t s_b =
{
    .parent          = &s_top,
    .entry_action    = enter_b,
    .exit_action     = exit_b,
    .transitions     = s_b_trans,
    .num_transitions = sizeof(s_b_trans) / sizeof(s_b_trans[0]),
    .name            = "B",
};

NGX_RTC_TEST(hsm_guard_gates_transition)
{
    ngx_rtc_hsm_t sm;
    const ngx_rtc_hsm_state_t *path[4];
    ngx_rtc_hsm_event_t ev;

    log_clear();
    g_guard_allow = 0;
    ngx_rtc_hsm_init(&sm, &s_a, path, 4, NULL, on_unhandled);

    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "A");

    /* Guard false: event is not handled here, bubbles up, hook fires. */
    ev.id = EV_GO;
    ev.context = NULL;
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "A");
    NGX_RTC_TEST_ASSERT_I64_EQ(g_unhandled_count, 1);
    NGX_RTC_TEST_ASSERT_STR_EQ(g_log, "u");

    /* Guard true: A -> B with exit(A) then entry(B). */
    g_guard_allow = 1;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "B");
    NGX_RTC_TEST_ASSERT_STR_EQ(g_log, "uAb");
}

NGX_RTC_TEST(hsm_internal_self_and_back_transitions)
{
    ngx_rtc_hsm_t sm;
    const ngx_rtc_hsm_state_t *path[4];
    ngx_rtc_hsm_event_t ev;

    log_clear();
    g_guard_allow = 1;
    ngx_rtc_hsm_init(&sm, &s_a, path, 4, NULL, on_unhandled);

    ev.id = EV_GO;
    ev.context = NULL;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "B");
    log_clear();

    /* INTERNAL transition: action only, state unchanged. */
    ev.id = EV_INT;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "B");
    NGX_RTC_TEST_ASSERT_STR_EQ(g_log, "i");

    /* External self-transition: exit(B) then entry(B). */
    ev.id = EV_SELF;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "B");
    NGX_RTC_TEST_ASSERT_STR_EQ(g_log, "iBb");

    /* B -> A: exit(B) then entry(A). */
    ev.id = EV_GO;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));
    NGX_RTC_TEST_ASSERT_STR_EQ(ngx_rtc_hsm_get_current_state_name(&sm), "A");
    NGX_RTC_TEST_ASSERT_STR_EQ(g_log, "iBbBa");
}

NGX_RTC_TEST(hsm_is_in_state_descendant_check)
{
    ngx_rtc_hsm_t sm;
    const ngx_rtc_hsm_state_t *path[4];
    ngx_rtc_hsm_event_t ev;

    log_clear();
    g_guard_allow = 1;
    ngx_rtc_hsm_init(&sm, &s_a, path, 4, NULL, on_unhandled);

    /* In A: A and its ancestor TOP are active, B is not. */
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_is_in_state(&sm, &s_a));
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_is_in_state(&sm, &s_top));
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_hsm_is_in_state(&sm, &s_b));

    ev.id = EV_GO;
    ev.context = NULL;
    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_dispatch(&sm, &ev));

    NGX_RTC_TEST_ASSERT(true == ngx_rtc_hsm_is_in_state(&sm, &s_b));
    NGX_RTC_TEST_ASSERT(false == ngx_rtc_hsm_is_in_state(&sm, &s_a));
}
