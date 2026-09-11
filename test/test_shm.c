/*
 * test_shm.c - host unit tests for ngx_rtc_shm.c.
 *
 * This unit was deliberately left out of the host build until now: it locks a
 * real shm mutex and allocates from a slab pool, neither of which exists here.
 * The stubs in test/include/ngx_core.h map the mutex onto a pthread mutex and
 * the slab onto malloc/free, which is exactly what the sanitizer build needs --
 * a double free or a leak in this file is then caught by ASan instead of only
 * by reading the code. That gap is not hypothetical: the eventfd leak fixed in
 * ngx_rtc_core_init_module() lives in this file and was found by review, not by
 * a test.
 *
 * Scope: the source/session registry lifecycle, which is where the slab
 * allocations and the ownership transitions live. The nginx-facing entry points
 * (zone init, worker notify fds) still need a real cycle and stay out.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_shm.h"
/* The `state` byte in the skeleton is a ngx_rtc_session_state_t. shm.h keeps
 * that dependency out (it forward-declares ngx_rtc_source_t rather than pulling
 * in core.h), so a reader of the field has to name the enum itself -- which is
 * exactly what the production callers do, via core.h. */
#include "ngx_rtc_session_fsm.h"

static ngx_rtc_shm_ctx_t  g_ctx;
static ngx_slab_pool_t    g_pool;
static ngx_shmtx_sh_t     g_pool_lock;

/* nginx's logging macros dereference the handle before deciding whether to emit
 * anything, so a NULL log is a crash rather than silence. Level 0 is below every
 * level, which is what "emit nothing" actually looks like. */
static ngx_log_t          shm_test_log;

/* Build a registry context backed by the malloc-based slab stub. Mirrors what
 * ngx_rtc_core_init_zone() does after ngx_slab_init(). */
static void
shm_test_setup(void)
{
    ngx_memzero(&g_ctx, sizeof(g_ctx));
    ngx_memzero(&g_pool, sizeof(g_pool));
    ngx_memzero(&g_pool_lock, sizeof(g_pool_lock));

    /* The pool's mutex has to be created, not merely zeroed. Under nginx's
     * struct that is a hard requirement: ngx_shmtx_lock() spins on mtx->lock,
     * and a zeroed ngx_shmtx_t leaves that pointer NULL. The stub header's
     * ngx_shmtx_lock() was a pthread call on a zeroed pthread_mutex_t, which
     * happens to be PTHREAD_MUTEX_INITIALIZER -- so the omission was invisible
     * until the real layout arrived. */
    (void) ngx_shmtx_create(&g_pool.mutex, &g_pool_lock, NULL);

    g_ctx.pool = &g_pool;
    g_ctx.nworkers = 1;
    g_ctx.ring_slots = 512;

    ngx_rbtree_init(&g_ctx.source_tree, &g_ctx.source_sentinel,
                    ngx_str_rbtree_insert_value);
    ngx_queue_init(&g_ctx.source_list);
    ngx_queue_init(&g_ctx.session_list);
}

/* Drop a source the test created. The slab is malloc here, so a source that is
 * never removed is a real leak as far as LeakSanitizer is concerned -- in
 * production the reaper's expire_locked() would reclaim it, but nothing drives
 * that on the host. Every test that calls get() must call this. */
static void
shm_test_teardown(const char *name)
{
    (void) ngx_rtc_shm_source_remove(&g_ctx, (u_char *) name,
                                     ngx_strlen(name));
}

/* A miss is a NULL return only for an unusable argument. get() is
 * get-or-create: a well-formed name that is not registered yet is allocated. */
NGX_RTC_TEST(shm_source_get_rejects_unusable_arguments)
{
    shm_test_setup();

    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_shm_source_get(NULL,
        (u_char *) "live/x", ngx_strlen("live/x")));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_shm_source_get(&g_ctx, NULL, 4));
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_shm_source_get(&g_ctx,
        (u_char *) "live/x", 0));

    /* A name at or over the field width would not terminate in src->name. */
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_shm_source_get(&g_ctx,
        (u_char *) "live/x", NGX_RTC_SHM_SOURCE_NAME_MAX));
}

/* get() creates on first use and returns the identical object afterwards, so
 * two publishers of the same name converge on one registry entry. */
NGX_RTC_TEST(shm_source_get_is_stable_across_calls)
{
    ngx_rtc_shm_source_t *first;
    ngx_rtc_shm_source_t *second;
    const char           *name = "live/stable";

    shm_test_setup();

    first = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name,
                                   ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(NULL != first);

    second = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name,
                                    ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(first == second);

    /* Its identity fields are the name it was looked up by. */
    NGX_RTC_TEST_ASSERT(NULL != first->name);

    shm_test_teardown(name);
}

/* remove() drops the registry entry, so a later get() builds a fresh one
 * instead of handing back a stale object. The address may well be reused
 * (malloc is free to), so the assertion is on state, not identity: a rebuilt
 * source has its expiry re-armed, while the old one was stamped by the test. */
NGX_RTC_TEST(shm_source_remove_then_get_returns_fresh_object)
{
    ngx_rtc_shm_source_t *first;
    ngx_rtc_shm_source_t *second;
    const char           *name = "live/reborn";

    shm_test_setup();

    first = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name,
                                   ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(NULL != first);

    /* Stamp it so a recycled allocation is distinguishable from a live entry. */
    first->expires = 0x5EED5EEDu;

    (void) ngx_rtc_shm_source_remove(&g_ctx, (u_char *) name,
                                     ngx_strlen(name));

    second = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name,
                                    ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(NULL != second);
    NGX_RTC_TEST_ASSERT(0x5EED5EEDu != second->expires);

    shm_test_teardown(name);
}

/* Removing a name that was never registered is a no-op, not a wild free. */
NGX_RTC_TEST(shm_source_remove_missing_is_safe)
{
    ngx_rtc_shm_source_t *src;

    shm_test_setup();

    (void) ngx_rtc_shm_source_remove(&g_ctx, (u_char *) "live/never",
                                     ngx_strlen("live/never"));

    /* Still createable afterwards: the no-op must not have corrupted the tree
     * or the source list. */
    src = ngx_rtc_shm_source_get(&g_ctx, (u_char *) "live/never",
                                 ngx_strlen("live/never"));
    NGX_RTC_TEST_ASSERT(NULL != src);

    shm_test_teardown("live/never");
}

/* Create a skeleton under `name` and hand back its ufrag for the state tests. */
static ngx_rtc_shm_session_t *
shm_test_add_session(const char *name, const char *ufrag)
{
    ngx_rtc_shm_session_t *sess;

    (void) ngx_rtc_shm_source_get(&g_ctx, (u_char *) name, ngx_strlen(name));

    sess = ngx_rtc_shm_session_add(&g_ctx, (u_char *) ufrag, ngx_strlen(ufrag),
                                   (u_char *) "pwd", 3,
                                   (u_char *) name, ngx_strlen(name),
                                   96, 111, 3, 3, 0);
    return sess;
}

/*
 * The lifecycle state is published for Lua, and the only thing that makes it
 * trustworthy is that a worker which does not own the session cannot write it.
 * The signaling worker holds its own copy of this session whose FSM stays in
 * NEW for the session's whole life; if it could write, every live session would
 * read as NEW and the feature would be worse than nothing.
 */
NGX_RTC_TEST(shm_session_state_tracks_owner_and_rejects_foreign_writes)
{
    const char            *name = "live/state";
    const char            *ufrag = "ufragstate";
    ngx_rtc_shm_session_t *sess;

    shm_test_setup();

    sess = shm_test_add_session(name, ufrag);
    NGX_RTC_TEST_ASSERT(NULL != sess);

    /* The creator is the signaling worker and no worker owns the media path
     * yet, so NEW is the only honest value here. */
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_NEW);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->owner_slot, -1);

    /* owner_slot is still -1: in the single-worker case the STUN binding lands
     * in the process that created the session and bind() never runs, so the
     * whole ICE_BOUND window has no owner recorded. The write must be accepted
     * -- a strict owner_slot == slot test would silently drop it, and with it
     * the only states worth diagnosing. */
    ngx_rtc_shm_session_set_state(&g_ctx, (u_char *) ufrag, ngx_strlen(ufrag),
                                  0, NGX_RTC_SESSION_STATE_ICE_BOUND);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_ICE_BOUND);

    /* ... and that an unowned skeleton is not thereby claimed: owner_slot keeps
     * its existing meaning, so the cross_worker stat is unaffected. */
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->owner_slot, -1);

    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_session_activate(
        &g_ctx, (u_char *) ufrag, ngx_strlen(ufrag), 0));
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->owner_slot, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_SRTP_READY);

    /* A different worker is now a foreign writer and must be ignored. */
    ngx_rtc_shm_session_set_state(&g_ctx, (u_char *) ufrag, ngx_strlen(ufrag),
                                  1, NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_SRTP_READY);

    /* The owner still writes. */
    ngx_rtc_shm_session_set_state(&g_ctx, (u_char *) ufrag, ngx_strlen(ufrag),
                                  0, NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);

    /* An unknown ufrag is a no-op rather than a lookup miss that returns early
     * while holding the lock. */
    ngx_rtc_shm_session_set_state(&g_ctx, (u_char *) "nosuch", 6, 0,
                                  NGX_RTC_SESSION_STATE_NEW);
    NGX_RTC_TEST_ASSERT_I64_EQ((int64_t) sess->state,
                               NGX_RTC_SESSION_STATE_DTLS_HANDSHAKE);

    ngx_rtc_shm_session_remove_if_owner(&g_ctx, (u_char *) ufrag,
                                        ngx_strlen(ufrag), 0);
    shm_test_teardown(name);
}

/*
 * A zone outlives a reload and a master restart, and both reuse paths take the
 * old root table on faith. Without the tag, a struct that grew since would have
 * the new binary read slab metadata as fields -- silently, at 1 Hz, forever.
 * This is what makes the upgrade fail loudly instead.
 */
NGX_RTC_TEST(shm_layout_check_refuses_a_foreign_zone)
{
    shm_test_setup();

    /* No root at all is not a matching layout. */
    NGX_RTC_TEST_ASSERT(NGX_ERROR == ngx_rtc_shm_layout_check(NULL, &shm_test_log));

    g_ctx.layout = NGX_RTC_SHM_LAYOUT;
    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_layout_check(&g_ctx, &shm_test_log));

    /* Zeroed -- what a memzero'd root that never reached the stamp looks like. */
    g_ctx.layout = 0;
    NGX_RTC_TEST_ASSERT(NGX_ERROR == ngx_rtc_shm_layout_check(&g_ctx, &shm_test_log));

    /* A future bump must not be mistaken for this one. */
    g_ctx.layout = NGX_RTC_SHM_LAYOUT + 1u;
    NGX_RTC_TEST_ASSERT(NGX_ERROR == ngx_rtc_shm_layout_check(&g_ctx, &shm_test_log));
}

/*
 * A source must outlive every session that points at it, even when its grace
 * has elapsed and nothing has subscribed.
 *
 * The reaper used to decide on `subscribers` alone, and a session that has not
 * finished its handshake is not a subscriber -- a WHIP publisher never becomes
 * one. So the source could be freed while sess->source still pointed at it, and
 * session_activate() then inserts into src->subscribers: a write into freed
 * slab. The dangling pointer is invisible to the NULL check activate() does.
 *
 * The second half is what keeps this from becoming "never reap anything": once
 * the last session is gone the source must still be collected.
 */
NGX_RTC_TEST(shm_source_outlives_a_session_that_points_at_it)
{
    const char            *name = "live/pending";
    const char            *ufrag = "ufragpend";
    ngx_rtc_shm_session_t *sess;
    ngx_rtc_shm_source_t  *src;

    shm_test_setup();

    sess = shm_test_add_session(name, ufrag);
    NGX_RTC_TEST_ASSERT(NULL != sess);
    src = sess->source;
    NGX_RTC_TEST_ASSERT(NULL != src);

    /* Handshake not finished, nothing subscribed: empty from the reaper's
     * point of view, grace long past. */
    NGX_RTC_TEST_ASSERT(true == ngx_queue_empty(&src->subscribers));
    src->expires = 1;

    /* forced = 0 on purpose. A forced pass (the allocation-pressure path)
     * discards `expires` and would reap the session too, which leaves the
     * source unreferenced and hides what this is testing. Only the session's
     * OWN grace keeps it alive here, which is the real window: the session is
     * well inside its lifetime while its source's grace has already run out. */
    ngx_rtc_shm_expire(&g_ctx, 0);

    /* Still registered. A freed source is removed from source_list first, so
     * membership is the assertion -- not the address, which malloc may reuse. */
    NGX_RTC_TEST_ASSERT(false == ngx_queue_empty(&g_ctx.source_list));
    NGX_RTC_TEST_ASSERT(src == ngx_queue_data(ngx_queue_head(&g_ctx.source_list),
                                              ngx_rtc_shm_source_t, queue));

    /* The session can now become a subscriber without touching freed memory. */
    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_session_activate(
        &g_ctx, (u_char *) ufrag, ngx_strlen(ufrag), 0));
    NGX_RTC_TEST_ASSERT(false == ngx_queue_empty(&src->subscribers));

    /* activate() cleared expires ("has at least one viewer"). In production the
     * unsubscribe path re-arms it once the last viewer leaves; here the session
     * is simply removed, so re-arm it by hand to reach the same state. */
    src->expires = 1;

    /* Once nothing points at it, the source is collected as before. */
    ngx_rtc_shm_session_remove_if_owner(&g_ctx, (u_char *) ufrag,
                                        ngx_strlen(ufrag), 0);
    ngx_rtc_shm_expire(&g_ctx, 0);
    NGX_RTC_TEST_ASSERT(true == ngx_queue_empty(&g_ctx.source_list));
}

/*
 * A publisher that dies without releasing -- a worker crash, a SIGKILL -- never
 * runs ngx_rtc_publish_release(), so its shm source keeps publishing=1 and the
 * reaper used to skip it forever: the flag meant "an owner exists", and nothing
 * could ever disprove that. The per-packet heartbeat that
 * ngx_rtmp_rtc_shm_stats() writes is what lets the reaper tell a dead publisher
 * from a live one, and this is the case that pins it down.
 */
NGX_RTC_TEST(shm_source_reclaims_a_publisher_that_stopped_its_heartbeat)
{
    const char           *name = "live/crashed";
    ngx_rtc_shm_source_t *src;

    shm_test_setup();
    ngx_current_msec = 1000000;

    src = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name, ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(NULL != src);
    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_source_try_publish(
        &g_ctx, (u_char *) name, ngx_strlen(name), NGX_RTC_PUBLISHER_RTMP));

    /* Due for collection: only publishing=1 + the heartbeat hold it now. */
    src->expires = (ngx_atomic_t) ngx_current_msec;
    src->publisher_seen_ms = (ngx_atomic_t) ngx_current_msec;

    /* Heartbeat still fresh: the publisher is alive, so it keeps its source
     * even though the empty-source expiry has elapsed. */
    ngx_current_msec += NGX_RTC_SHM_PUBLISH_GRACE_MS - 1;
    ngx_rtc_shm_expire(&g_ctx, 0);
    NGX_RTC_TEST_ASSERT(false == ngx_queue_empty(&g_ctx.source_list));

    /* Heartbeat silent past the grace: the publisher is gone for good. */
    ngx_current_msec += 2;
    ngx_rtc_shm_expire(&g_ctx, 0);
    NGX_RTC_TEST_ASSERT(true == ngx_queue_empty(&g_ctx.source_list));
}

/*
 * The second half of ngx_rtc_publish_release() must not undo the first half.
 *
 * release_publish() arms src->expires when the last viewer leaves -- the grace
 * that keeps a shm source alive while another worker still holds a cached
 * pointer to it. publish_release() then calls source_remove() on the very next
 * line, and remove() decided on `subscribers` and `publishing` alone: it never
 * looked at `expires`, and never looked at the skeletons still pointing at the
 * source. So it freed immediately, and the caller's own close path walked into
 * it -- ngx_rtc_shm_session_remove_if_owner() -> session_free_locked() does
 * `src = sess->source` and then reads src->subscribers / src->publishing and
 * writes src->expires.
 *
 * A WHIP publisher hits this every time it stops without viewers: a publishing
 * session is deliberately kept out of its own subscriber list, so `subscribers`
 * is empty and remove() had nothing holding it back. The write lands in slab
 * memory that is already on the free list.
 *
 * ngx_rtc_shm_source_referenced_locked() is the check the reaper already uses
 * for exactly this; remove() is the path that missed it.
 */
NGX_RTC_TEST(shm_source_survives_release_while_a_session_points_at_it)
{
    const char            *name = "live/whip";
    const char            *ufrag = "ufragwhip";
    ngx_rtc_shm_session_t *sess;
    ngx_rtc_shm_source_t  *src;

    shm_test_setup();

    sess = shm_test_add_session(name, ufrag);
    NGX_RTC_TEST_ASSERT(NULL != sess);
    src = sess->source;
    NGX_RTC_TEST_ASSERT(NULL != src);

    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_source_try_publish(
        &g_ctx, (u_char *) name, ngx_strlen(name), NGX_RTC_PUBLISHER_WHIP));

    /* Exactly what ngx_rtc_publish_release() does, in order: arm the grace,
     * then hand the shm source back. */
    ngx_rtc_shm_source_release_publish(&g_ctx, (u_char *) name,
                                       ngx_strlen(name), NGX_RTC_PUBLISHER_WHIP);
    NGX_RTC_TEST_ASSERT(0 != src->expires);

    ngx_rtc_shm_source_remove(&g_ctx, (u_char *) name, ngx_strlen(name));

    /* Still referenced by the skeleton, so still registered. A freed source is
     * unlinked first, so membership is the assertion -- not the address, which
     * malloc may reuse. */
    NGX_RTC_TEST_ASSERT(false == ngx_queue_empty(&g_ctx.source_list));
    NGX_RTC_TEST_ASSERT(src == ngx_queue_data(ngx_queue_head(&g_ctx.source_list),
                                              ngx_rtc_shm_source_t, queue));
    NGX_RTC_TEST_ASSERT(src == sess->source);

    /* The other half: once the last session is gone the grace runs out and the
     * reaper collects it. The guard must not become "never free". */
    src->expires = 1;
    ngx_rtc_shm_session_remove_if_owner(&g_ctx, (u_char *) ufrag,
                                        ngx_strlen(ufrag), 0);
    ngx_rtc_shm_expire(&g_ctx, 0);
    NGX_RTC_TEST_ASSERT(true == ngx_queue_empty(&g_ctx.source_list));
}
