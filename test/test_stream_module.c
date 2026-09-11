/*
 * test_stream_module.c - host tests for the UDP media plane.
 *
 * ngx_rtc_stream_module.c keeps every interesting entry point static (the
 * receive path, the close path, the DTLS completion callback), so this file
 * includes the translation unit rather than linking it. That is the point: the
 * bugs found here by reading -- the DTLS completion callback freeing the
 * session out from under its caller, the close path having no re-entrancy
 * guard -- are all invisible to anything that can only reach the module through
 * ngx_module_t.
 *
 * The file is compiled here, not linked, so it is NOT in CORE_NAMES; the
 * Makefile gives this object the same flags that object gets (-DNGX_PTR_SIZE,
 * the -Wextra relaxations the nginx CFLAGS already imply).
 *
 * What is real in this build: the session FSM, the DTLS state machine
 * (ngx_rtc_dtls.c links against OpenSSL), the shm registry. What is stubbed is
 * listed in test/nginx_stub.c -- notably SRTP, which needs libsrtp2 and is
 * covered by the end-to-end run instead.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_stream.h>

#include "ngx_rtc_test.h"

/* Pull in the unit under test so its static functions are callable. */
#include "../src/ngx_rtc_stream_module.c"


/* A connection whose send() accepts everything and whose log suppresses every
 * message: the close path and the DTLS send path both reach for c->log, and
 * nginx's ngx_log_error() macro dereferences it before deciding whether to log.
 * A zeroed ngx_log_t has log_level 0, which is below every level. */
static ngx_log_t stream_test_log;

/* nginx allocates the per-session ctx array in ngx_stream_init_connection, from
 * the cycle pool. There is no cycle here, so the test owns it -- but it must
 * exist: the ctx macros index through the pointer, and a session whose ctx was
 * never allocated is a NULL dereference, not an empty slot. */
#define STREAM_TEST_CTX_SLOTS  8
static void *stream_test_ctx[STREAM_TEST_CTX_SLOTS];

static ssize_t
stream_test_conn_send(ngx_connection_t *c, u_char *buf, size_t size)
{
    (void) c;
    (void) buf;

    return (ssize_t) size;
}


/*
 * The DTLS completion callback runs synchronously inside
 * ngx_rtc_dtls_on_data() and ngx_rtc_dtls_handle_timeout(), and both callers
 * keep using the session pointer after it returns. When the handshake cannot be
 * finished -- SRTP key export failed, or ngx_rtc_srtp_create() failed -- the
 * callback must therefore flag the failure and let the caller close the
 * session, not close it itself.
 *
 * The version this replaces called ngx_rtc_stream_session_close() directly,
 * which ngx_free()s the session. The caller then read sess->dtls (in
 * ngx_rtc_dtls_is_done()) out of freed memory, and whichever teardown ran
 * second double-freed. Under ASan the old form dies here; the memory is
 * deliberately untouched between the callback and the assertion so a freed
 * session is still a use-after-free rather than a lucky read of intact bytes.
 */
NGX_RTC_TEST(stream_dtls_done_flags_instead_of_freeing_the_session)
{
    ngx_rtc_session_t   *sess;
    ngx_connection_t     conn;
    ngx_stream_session_t stream_sess;
    ngx_stream_session_t *s = &stream_sess;

    (void) memset(&conn, 0, sizeof(conn));
    (void) memset(&stream_sess, 0, sizeof(stream_sess));

    /* log_level 0 is below every level, so nginx's ngx_log_error() macro --
     * which tests log->log_level before calling ngx_log_error_core -- never
     * reaches the logger. The handle still has to be a real ngx_log_t, because
     * the macro dereferences it. */
    conn.log = &stream_test_log;
    conn.send = stream_test_conn_send;
    conn.data = s;
    s->connection = &conn;
    s->ctx = stream_test_ctx;

    sess = ngx_calloc(sizeof(*sess), NULL);
    NGX_RTC_TEST_ASSERT(NULL != sess);

    (void) memset(sess, 0, sizeof(*sess));
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    (void) ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                        NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void) ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                        NGX_RTC_SESSION_EVT_DTLS_PACKET);

    sess->conn = (void *) &conn;
    (void) strncpy(sess->ice_ufrag, "ufrag-no-key", sizeof(sess->ice_ufrag) - 1);
    ngx_rtc_session_add(sess);

    /* nginx assigns every module its ctx_index while it processes the config;
     * NGX_MODULE_V1 only leaves the field at NGX_MODULE_UNSET_INDEX, which is
     * (ngx_uint_t) -1. There is no config here, so the test does that step --
     * without it the ctx pointer is indexed by -1 and nginx's macro writes one
     * element BEFORE the array. The stub header seeded the field with 0, which
     * hid the requirement entirely. */
    ngx_rtc_stream_module.ctx_index = 0;

    ngx_stream_set_ctx(s, sess, ngx_rtc_stream_module);

    /* dtls is zeroed, so handshake_done == 0 and the key export fails. */
    ngx_rtc_stream_dtls_done(sess);

    /* Still ours, and flagged: nothing may have freed it. */
    NGX_RTC_TEST_ASSERT_U64_EQ(sess->close_pending, 1);

    /* The caller regains control and closes it, exactly as on_dtls() and
     * dtls_timer() do. */
    ngx_rtc_stream_session_close(sess, NGX_RTC_SESSION_EVT_CLOSE);

    /* The close path posts the UDP session's finalize so it does not run on the
     * receive path's stack; the host build has no event loop, so the test drains
     * the queue itself. */
    ngx_rtc_host_drain_posted();

    /* The session was handed back to nginx -- without this the bound 4-tuple
     * holds an ngx_connection_t slot forever. */
    NGX_RTC_TEST_ASSERT(NULL == s->connection);
    NGX_RTC_TEST_ASSERT(NULL == ngx_stream_get_module_ctx(s,
                                                          ngx_rtc_stream_module));
}
