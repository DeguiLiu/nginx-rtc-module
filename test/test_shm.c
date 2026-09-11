/*
 * test_shm.c - host unit tests for ngx_rtc_shm.c.
 *
 * This unit was deliberately left out of the host build until now: it locks a
 * real shm mutex and allocates from a slab pool, neither of which exists here.
 *
 * The slab stays malloc-backed in both header worlds -- one allocation per
 * object instead of a sub-allocation of one zone -- and that is the point of
 * not linking ngx_slab.c. A real slab hands the sanitizer a single large
 * allocation, under which a leak or an overflow inside it is invisible; with
 * malloc per object, LeakSanitizer and ASan keep per-object granularity. That
 * gap is not hypothetical: the eventfd leak fixed in ngx_rtc_core_init_module()
 * lives in this file, and the source leak the reaper covers was caught here.
 *
 * The mutex is the part that differs. Against the real headers the pool carries
 * nginx's own ngx_shmtx_t and ngx_shmtx_lock() spins on mtx->lock, so setup has
 * to call ngx_shmtx_create() -- a zeroed struct leaves that pointer NULL. Only
 * the Windows build, which uses test/include/, maps it onto a pthread mutex.
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
 * The above test forces the deadline by hand (`src->expires = now`). The real
 * case cannot: try_publish() sets expires = 0 for as long as the source
 * publishes (ngx_rtc_shm.c:653), so a crashed publisher leaves the source with
 * expires == 0 permanently. Clearing `publishing` in the reaper does not re-arm
 * it, and the reap rule below is gated on `0 != src->expires` -- so the source
 * and its ~1.5 MB retransmit ring survive every sweep, which is the very
 * outcome the heartbeat comment says it exists to prevent.
 *
 * The distinguishing condition is a publisher with NO viewer: then no session
 * ever unbinds, so nothing runs the "last session left" arm at :329 either.
 */
NGX_RTC_TEST(shm_source_reclaims_a_dead_publisher_with_no_viewer)
{
    const char           *name = "live/crashed_noviewer";
    ngx_rtc_shm_source_t *src;

    shm_test_setup();
    ngx_current_msec = 1000000;

    src = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name, ngx_strlen(name));
    NGX_RTC_TEST_ASSERT(NULL != src);
    NGX_RTC_TEST_ASSERT(NGX_OK == ngx_rtc_shm_source_try_publish(
        &g_ctx, (u_char *) name, ngx_strlen(name), NGX_RTC_PUBLISHER_RTMP));

    /* The invariant the leak turns on: publishing implies expires == 0. */
    NGX_RTC_TEST_ASSERT(0 == src->expires);

    /* Two sweeps, because the reaper both decides and collects, and the
     * decision is what arms the deadline: the first sweep sees a silent
     * heartbeat, clears `publishing` and arms expires = now + EXPIRE; only a
     * later sweep, once that deadline has passed, may collect.
     *
     * No viewer is ever subscribed, so nothing runs the "last session left"
     * arm -- this sweep is the only chance to set the deadline at all. */
    src->publisher_seen_ms = (ngx_atomic_t) ngx_current_msec;
    ngx_current_msec += NGX_RTC_SHM_PUBLISH_GRACE_MS + 1;
    ngx_rtc_shm_expire(&g_ctx, 0);

    /* Declared dead, deadline armed -- but not yet due. */
    NGX_RTC_TEST_ASSERT(false == ngx_queue_empty(&g_ctx.source_list));

    ngx_current_msec += NGX_RTC_SHM_SOURCE_EXPIRE_MS + 1;
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

/* --- GOP replay ------------------------------------------------------- */

typedef struct {
    uint16_t  seq[64];
    uint32_t  n;
} shm_replay_rec_t;

static ngx_int_t
shm_replay_record(void *opaque, const uint8_t *rtp, uint32_t len,
                  uint8_t is_gop_start)
{
    shm_replay_rec_t *rec = opaque;

    (void) is_gop_start;

    if (rec->n < 64u && len > 4u) {
        rec->seq[rec->n] = (uint16_t)(((uint16_t) rtp[2] << 8) | (uint16_t) rtp[3]);
    }
    rec->n++;
    return NGX_OK;
}

/* A 20-byte packet: real length (the replay only trusts the marker bit on a
 * packet longer than the RTP header), sequence in bytes 2..3, marker in bit 7
 * of byte 1. */
static void
shm_replay_make_rtp(uint8_t *rtp, uint16_t seq, int marker)
{
    ngx_memzero(rtp, 20);
    rtp[0] = 0x80;
    rtp[1] = (uint8_t)((0 != marker ? 0x80 : 0x00) | 96);
    rtp[2] = (uint8_t)(seq >> 8);
    rtp[3] = (uint8_t)(seq & 0xff);
}

/*
 * The replay copies the cached GOP out of shared memory under the slab pool
 * mutex -- the global lock every worker's slab allocation also takes -- and
 * then sends it outside. The send loop stops at the marker bit that ends the
 * keyframe's access unit, so everything past it is sent by nobody; copying it
 * under the global lock buys a lock hold proportional to the retained window
 * (up to 1024 * 1508 bytes) to throw all of it away.
 *
 * The count returned is what tells the caller whether the cache could serve
 * this viewer at all, so "copied" and "served" have to be the same number.
 */
NGX_RTC_TEST(shm_replay_copies_only_the_keyframe_access_unit)
{
    ngx_rtc_shm_source_t *src;
    shm_replay_rec_t      rec;
    uint8_t               rtp[20];
    uint16_t              seq;
    ngx_int_t             rc;
    const char           *name = "live/replay";
    size_t                nlen = ngx_strlen(name);

    shm_test_setup();

    src = ngx_rtc_shm_source_get(&g_ctx, (u_char *) name, nlen);
    NGX_RTC_TEST_ASSERT(NULL != src);

    /* The ring only fills while a viewer on another worker is subscribed. */
    src->remote_subscribers = 1;

    /* One IDR access unit: three packets, the marker on the last. */
    for (seq = 100u; seq < 103u; seq++) {
        shm_replay_make_rtp(rtp, seq, 102u == seq);
        ngx_rtc_shm_retransmit_append(&g_ctx, src, rtp, sizeof(rtp),
                                      100u == seq);
    }

    /* Then live packets that a GOP replay must leave alone. */
    for (seq = 103u; seq < 108u; seq++) {
        shm_replay_make_rtp(rtp, seq, 0);
        ngx_rtc_shm_retransmit_append(&g_ctx, src, rtp, sizeof(rtp), 0);
    }

    (void) ngx_memzero(&rec, sizeof(rec));
    rc = ngx_rtc_shm_retransmit_replay_gop(&g_ctx, (u_char *) name, nlen,
                                           shm_replay_record, &rec);

    NGX_RTC_TEST_ASSERT_I64_EQ(rc, 3);   /* the access unit, not the window */
    NGX_RTC_TEST_ASSERT_I64_EQ(rec.n, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(rec.seq[0], 100u);
    NGX_RTC_TEST_ASSERT_I64_EQ(rec.seq[1], 101u);
    NGX_RTC_TEST_ASSERT_I64_EQ(rec.seq[2], 102u);

    shm_test_teardown(name);
}

