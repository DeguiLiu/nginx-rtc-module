/**
 * @file    ngx_rtc_shm.h
 * @brief   Shared-memory registry for cross-worker RTC metadata.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * A `rtc_zone` directive plus a slab-backed source / session registry, the
 * per-worker media rings and the per-source GOP retransmit rings. Only value
 * types and pointers to other shm allocations live in this zone; DTLS/SRTP/
 * connection/audio handles stay in per-worker process memory and are attached
 * when a worker first touches a session. See docs/ARCHITECTURE.md.
 *
 * The registry half of ngx_rtc_shm.c is plain data-structure code and does run
 * in the host unit tests (test/test_shm.c, over slab/shmtx stubs); what needs
 * the nginx config system, and so stays out of them, is
 * ngx_rtc_core_module.c. This header itself is nginx-only.
 *
 * The pure-C protocol core (rtp/sdp/stun/dtls/srtp) remains nginx-free.
 */

#ifndef NGX_RTC_SHM_H
#define NGX_RTC_SHM_H

#include <ngx_config.h>
#include <ngx_core.h>

#define NGX_RTC_SHM_SOURCE_NAME_MAX  128u
#define NGX_RTC_SHM_UFRAG_MAX         64u
#define NGX_RTC_SHM_PWD_MAX          256u
#define NGX_RTC_SHM_SPS_MAX          256u
#define NGX_RTC_SHM_PPS_MAX          256u
#define NGX_RTC_SHM_ASC_MAX           64u
#define NGX_RTC_SHM_SESSION_EXPIRE_MS 30000u /* half-open session reap */
#define NGX_RTC_SHM_SOURCE_EXPIRE_MS  10000u /* empty non-publishing source reap */

/* How long a publishing source may go without a heartbeat before the reaper
 * treats its publisher as gone. The publishing worker refreshes
 * publisher_seen_ms on every packet (ngx_rtmp_rtc_shm_stats), so a live stream
 * keeps it current; a worker that dies without releasing leaves it frozen, and
 * without this the source plus its retransmit ring would be pinned for the
 * life of the zone. Deliberately far larger than any plausible packet gap on a
 * live stream, so a stalled-but-alive publisher is not mistaken for a dead
 * one -- a false positive costs a re-claim, not correctness. */
#define NGX_RTC_SHM_PUBLISH_GRACE_MS  10000u

/* Publisher ownership tag for a source: which ingest protocol currently owns
 * the "publishing" flag. A source may have exactly one publisher at a time; a
 * second publisher of a different protocol is rejected rather than sharing the
 * source (which would let one protocol's close free the other's cached shm_src). */
#define NGX_RTC_PUBLISHER_NONE 0u
#define NGX_RTC_PUBLISHER_RTMP 1u
#define NGX_RTC_PUBLISHER_WHIP 2u

typedef struct ngx_rtc_shm_source_s  ngx_rtc_shm_source_t;
typedef struct ngx_rtc_shm_session_s ngx_rtc_shm_session_t;

/* Process-local mirror of the publish ownership (defined in ngx_rtc_core.h,
 * which this header must not include: it is compiled into the host tests). */
typedef struct ngx_rtc_source_s ngx_rtc_source_t;

/* Cross-worker retransmit ring: a per-source fixed-window cache of the recent
 * plaintext RTP video packets, indexed by RTP sequence number. The publisher
 * appends every video packet while a cross-worker viewer is subscribed; any
 * worker answers NACK/PLI from it. All slots are read/written under the slab
 * pool mutex, so a ring is never read after its source is freed. */
#define NGX_RTC_SHM_RETX_RING_CAP 1024u
#define NGX_RTC_RING_MAX_SESSIONS 64u
#define NGX_RTC_RING_RTP_MAX      1500u
#define NGX_RTC_RING_DEFAULT_SLOTS 512u

typedef struct {
    uint16_t    len;          /* valid bytes in data */
    uint8_t     is_gop_start; /* first packet of an IDR access unit */
    u_char      data[NGX_RTC_RING_RTP_MAX]; /* plaintext RTP packet */
} ngx_rtc_shm_retransmit_slot_t;

typedef struct {
    ngx_uint_t  head;      /* next absolute write index (guarded by pool mutex) */
    ngx_uint_t  count;     /* valid entries in [head-count, head) */
    ngx_uint_t  gop_start; /* absolute index of the latest GOP start */
    ngx_uint_t  cap;       /* power of two slot count */
    ngx_rtc_shm_retransmit_slot_t slots[1]; /* allocated as cap slots */
} ngx_rtc_shm_retransmit_t;

/* Cross-worker source metadata. Scratch buffers (video/audio tag bodies), the
 * GOP ring and the AAC transcoder are per-worker and deliberately absent here. */
struct ngx_rtc_shm_source_s {
    ngx_str_node_t         sn;        /* rbtree node; key = crc32(name) */
    ngx_queue_t            queue;      /* source_list link (GC / stats) */
    u_char                 name[NGX_RTC_SHM_SOURCE_NAME_MAX];
    ngx_uint_t             publishing;
    ngx_uint_t             publisher_kind; /* NGX_RTC_PUBLISHER_* ownership tag */
    ngx_atomic_t           expires;    /* msec absolute, 0 = no expire (empty source reap) */
    /* Last heartbeat from the publishing worker, msec absolute (0 = never
     * published). Written lock-free per packet by ngx_rtmp_rtc_shm_stats() and
     * read by the reaper: it is the only evidence that distinguishes a dead
     * publisher (a worker crash never runs ngx_rtc_publish_release) from one
     * that is simply idle. */
    ngx_atomic_t           publisher_seen_ms;
    ngx_int_t              publisher_slot;   /* RTMP ingest worker (-1 unknown);
                                              * stats uses it to flag cross-worker
                                              * viewers (owner_slot != this) */
    uint32_t               video_ssrc;
    uint8_t                video_pt;
    uint32_t               audio_ssrc;
    uint8_t                audio_pt;
    uint16_t               video_seq;
    uint16_t               audio_seq;
    uint32_t               video_ts;
    uint32_t               audio_ts;
    ngx_uint_t             audio_ts_valid;
    ngx_uint_t             have_ts;
    ngx_uint_t             video_pkts;
    ngx_uint_t             video_octets;
    ngx_uint_t             audio_pkts;
    ngx_uint_t             audio_octets;
    ngx_uint_t             send_failed;   /* RTP dgrams dropped: send NGX_ERROR / short write */
    ngx_uint_t             send_eagain;   /* RTP dgrams dropped: send NGX_AGAIN (UDP buf full) */
    u_char                 sps[NGX_RTC_SHM_SPS_MAX];
    ngx_uint_t             sps_len;
    u_char                 pps[NGX_RTC_SHM_PPS_MAX];
    ngx_uint_t             pps_len;
    u_char                 sps_profile_level_id[3];
    ngx_uint_t             sps_profile_level_id_valid;
    u_char                 audio_asc[NGX_RTC_SHM_ASC_MAX];
    ngx_uint_t             audio_asc_len;
    ngx_atomic_t           subscribers_version; /* bumped on subscriber add/remove */
    ngx_queue_t            subscribers; /* subscriber skeleton list head */
    ngx_atomic_t           remote_subscribers; /* viewers on a worker != publisher_slot */

    /* Cross-worker retransmit ring (video): lazily allocated on first video
     * packet; any worker answers NACK/PLI from it under the pool mutex. */
    ngx_rtc_shm_retransmit_t   *retransmit;
    ngx_uint_t                  retransmit_alloc_failed; /* lazy ring alloc failures */

    /* Lock-cost instrumentation for that ring. These exist to answer one
     * question: is the shared slab pool mutex worth splitting? So the *_us
     * fields measure from just before the lock to just before the unlock --
     * lock wait plus the work underneath, not the copy alone, because waiting
     * is the cost being argued about.
     *
     * Written only while holding that mutex and read under it, so plain fields
     * and no atomics. Counts are exact; the microsecond sums are wall clock.
     * Surfaced by /rtc/v1/stats as retx_lock / total_retx_lock. */
    ngx_uint_t                  retx_append_locked;  /* appends that took the mutex */
    ngx_uint_t                  retx_append_us;      /* us lost to it, append path */
    ngx_uint_t                  retx_replay_count;   /* GOP replays served */
    ngx_uint_t                  retx_replay_slots;   /* slots copied, all replays */
    ngx_uint_t                  retx_replay_us;      /* us lost to it, replay path */
};

/* Cross-worker session skeleton. DTLS/SRTP/connection state is attached by the
 * owning worker; only identity, negotiated PT and readiness live here. */
struct ngx_rtc_shm_session_s {
    ngx_queue_t            queue;      /* session_list link */
    ngx_queue_t            sub_queue;  /* source->subscribers link */
    ngx_rtc_shm_source_t  *source;
    ngx_uint_t             id;         /* monotonic, never reused */
    u_char                 ice_ufrag[NGX_RTC_SHM_UFRAG_MAX];
    u_char                 ice_pwd[NGX_RTC_SHM_PWD_MAX];
    uint8_t                video_pt;
    uint8_t                audio_pt;
    uint8_t                twcc_video_ext; /* transport-cc ext id (0 = none) */
    uint8_t                twcc_audio_ext;
    uint8_t                publishing; /* 1 = WHIP publisher, 0 = player */
    ngx_atomic_t           srtp_ready; /* 1 once DTLS -> SRTP completed */
    ngx_int_t              owner_slot; /* owning worker slot, -1 while unbound */
    ngx_atomic_t           close_requested; /* admin kick: 1 = close on next reap */
    ngx_atomic_t           expires;    /* msec absolute, 0 = no expire (half-open reap) */
    ngx_atomic_t           twcc_lost; /* cumulative transport-cc loss */
    ngx_atomic_t           twcc_received; /* cumulative transport-cc received */

    /* Where this session's egress actually goes. A pacer drop keeps the RTP
     * sequence number the source assigned, so the viewer scores it as loss and
     * a loss-based controller can mistake its own drops for congestion; these
     * three make that visible from /rtc/v1/stats (and therefore from Lua). */
    ngx_atomic_t           pacer_bps;  /* AIMD send-rate target, bits/s */
    ngx_atomic_t           drop_pacer; /* media refused by the token bucket */
    ngx_atomic_t           drop_gop;   /* video held off until the next IDR */

    /* Lifecycle state (ngx_rtc_session_state_t), written only by the worker
     * that owns the UDP session and read by the stats renderer under the pool
     * mutex. Appended last so every field above keeps its offset. Unlike
     * srtp_ready this is exhaustive, which is the point: a session wedged in
     * ICE_BOUND or DTLS_HANDSHAKE is diagnosable from Lua, and neither state is
     * visible from any other field here. CLOSED is never written -- a closed
     * skeleton is freed outright (ngx_rtc_shm_expire_locked), so the state that
     * is observable is "gone from session_list", not a byte. */
    uint8_t                state;
};

/* Immutable session fields copied out under the pool mutex so the STUN attach
 * path can rebuild a per-process session without dereferencing a slab pointer
 * after the lock is released (the owner worker may free it concurrently). */
typedef struct {
    ngx_uint_t  id;
    u_char      ice_ufrag[NGX_RTC_SHM_UFRAG_MAX];
    u_char      ice_pwd[NGX_RTC_SHM_PWD_MAX];
    uint8_t     video_pt;
    uint8_t     audio_pt;
    uint8_t     twcc_video_ext;   /* transport-cc ext id (0 = none) */
    uint8_t     twcc_audio_ext;
    uint8_t     publishing;       /* 1 = WHIP publisher, 0 = player */
    /* Mirrors the skeleton's srtp_ready. A worker rebuilding this session
     * (attach_from_shm) never performed the DTLS handshake, so it must adopt
     * this state rather than sit in NEW -- see ngx_rtc_session_fsm_restore(). */
    uint8_t     srtp_ready;
    u_char      source_name[NGX_RTC_SHM_SOURCE_NAME_MAX];
    uint32_t    source_video_ssrc;
    uint32_t    source_audio_ssrc;
    uint8_t     source_video_pt;
    uint8_t     source_audio_pt;
} ngx_rtc_shm_session_snapshot_t;

/* Phase 2: one fixed-size media-ring slot. The RTMP producer(s) fill a whole
 * entry (plaintext RTP + the shm session ids owned by one target worker) and
 * enqueue it; the owning worker consumes it and applies per-session SRTP. */

typedef struct {
    ngx_atomic_t   seq;       /* monotonic slot sequence (future lock-free) */
    ngx_uint_t     media;     /* 0 = video, 1 = audio */
    ngx_uint_t     gop;       /* 1 = first packet of an IDR access unit (video) */
    ngx_uint_t     len;       /* valid bytes in rtp[] */
    ngx_uint_t     nsess;     /* number of valid sess[] entries */
    ngx_uint_t     sess[NGX_RTC_RING_MAX_SESSIONS]; /* shm session ids */
    u_char         rtp[NGX_RTC_RING_RTP_MAX];       /* plaintext RTP payload */
} ngx_rtc_ring_entry_t;

/* MPSC ring: multiple RTMP producers enqueue, the single owner worker dequeues.
 * The ring owns an ngx_shmtx_t lock so the media hot path no longer contends on
 * the slab pool's global mutex; the source/session registry keeps pool->mutex
 * and the media ring keeps ring->mtx. */
typedef struct {
    ngx_atomic_t          head;      /* next write slot (monotonic) */
    ngx_atomic_t          tail;      /* next read slot (monotonic) */
    ngx_shmtx_t           mtx;       /* per-ring lock (points at mtx_sh) */
    ngx_shmtx_sh_t        mtx_sh;    /* the lock word itself, in shm */
    ngx_uint_t            size;      /* power of two slot count */
    ngx_uint_t            mask;      /* size - 1 */
    ngx_rtc_ring_entry_t  entries[1]; /* allocated as size slots */
} ngx_rtc_shm_ring_t;

/* Bumped whenever the layout of anything reachable from ngx_rtc_shm_ctx_t
 * changes. A zone outlives both a reload and a master restart (see the reuse
 * branches of ngx_rtc_core_init_zone), and neither path can know that the
 * binary now expects different offsets -- a struct that grew would have the new
 * code read slab metadata as fields, silently. The tag turns that into a
 * refused start. */
#define NGX_RTC_SHM_LAYOUT  3u

/* Root table kept in shpool->data. source_tree is keyed by "app/stream" name;
 * sessions are a small linear list keyed by ICE ufrag. */
typedef struct {
    ngx_slab_pool_t       *pool;       /* == shm_zone->shm.addr */
    ngx_rbtree_t           source_tree;
    ngx_rbtree_node_t      source_sentinel;
    ngx_queue_t            source_list;   /* head of all sources */
    ngx_queue_t            session_list;  /* head of all session skeletons */
    ngx_rtc_shm_ring_t    *rings[NGX_MAX_PROCESSES]; /* per-worker media rings */
    ngx_fd_t               notify_fd[NGX_MAX_PROCESSES]; /* per-worker eventfd */
    ngx_uint_t             next_session_id; /* monotonic session id allocator */
    ngx_uint_t             nworkers;
    ngx_uint_t             ring_slots;
    uint32_t               layout;     /* == NGX_RTC_SHM_LAYOUT, checked on reuse */
} ngx_rtc_shm_ctx_t;

/* Per-cycle configuration (process memory). One rtc_zone per cycle. */
typedef struct {
    ngx_shm_zone_t      *shm_zone;   /* NULL when rtc_zone is not configured */
    ngx_rtc_shm_ctx_t   *sh;         /* shm root == shpool->data */
    ngx_slab_pool_t     *shpool;     /* slab allocator */
    ngx_uint_t           nworkers;    /* from ccf->worker_processes */
    ngx_uint_t           ring_slots;  /* media-ring capacity per worker */

    /* Runtime tunables. Published to the pure C units as one read-only
     * snapshot by ngx_rtc_core_init_conf (see ngx_rtc_tunables_t in
     * ngx_rtc_core.h); NGX_CONF_UNSET* until then. */
    ngx_msec_t           jitter_timeout;    /* rtc_jitter_timeout */
    ngx_msec_t           nack_window;       /* rtc_nack_window */
    ngx_msec_t           nack_window_max;   /* rtc_nack_window_max */
    ngx_uint_t           eagain_streak_max; /* rtc_eagain_streak */
    ngx_uint_t           gop_ring_slots;    /* rtc_gop_ring_slots */
} ngx_rtc_core_conf_t;

ngx_rtc_core_conf_t *ngx_rtc_core_get_conf(ngx_cycle_t *cycle);

/* Source registry (find-or-create / find / remove). All operations hold the
 * slab pool mutex; callers must not call slab APIs while holding it. */
ngx_rtc_shm_source_t *ngx_rtc_shm_source_get(ngx_rtc_shm_ctx_t *ctx,
                                              u_char *name, size_t len);
void ngx_rtc_shm_source_remove(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len);

/* Accumulate per-source send-drop counters (called by the per-worker stats
 * mirror timer; no-op when the source no longer exists). */
void ngx_rtc_shm_source_add_send_stats(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                       size_t len, ngx_uint_t failed,
                                       ngx_uint_t eagain);

/* Session registry. add allocates a skeleton and links it into session_list;
 * remove unlinks it from both lists and frees it. */
ngx_rtc_shm_session_t *ngx_rtc_shm_session_add(ngx_rtc_shm_ctx_t *ctx,
        u_char *ufrag, size_t ufrag_len, u_char *pwd, size_t pwd_len,
        u_char *name, size_t name_len, uint8_t video_pt, uint8_t audio_pt,
        uint8_t twcc_video_ext, uint8_t twcc_audio_ext, uint8_t publishing);

/* Bind a skeleton to `slot` and copy its immutable fields into `out` (attach
 * path). NGX_OK on success or when already owned by `slot`; NGX_DECLINED when
 * another worker owns it. All reads/writes happen under the pool mutex. */
ngx_int_t ngx_rtc_shm_session_bind(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                   size_t len, ngx_uint_t slot,
                                   ngx_rtc_shm_session_snapshot_t *out);

/* Bind, mark srtp_ready and subscribe to its source (DTLS-done path).
 * Idempotent for the owning worker. NGX_OK / NGX_DECLINED. */
ngx_int_t ngx_rtc_shm_session_activate(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                       size_t len, ngx_uint_t slot);

/* Remove the skeleton iff it is still unbound (-1) or owned by `slot`; this is
 * the only way a half-open (never-DTLS-bound) skeleton is reclaimed. */
void ngx_rtc_shm_session_remove_if_owner(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                         size_t len, ngx_uint_t slot);

/* Mark one skeleton for close by its monotonic session id (admin kick). The
 * owner worker reaps it on the next timer tick. NGX_OK / NGX_ERROR. */
ngx_int_t ngx_rtc_shm_session_request_close(ngx_rtc_shm_ctx_t *ctx,
                                            ngx_uint_t id);

/* Read the close_requested flag for a skeleton (no slab pointer escapes the
 * mutex). Returns 1 when requested, 0 otherwise. */
ngx_uint_t ngx_rtc_shm_session_is_close_requested(ngx_rtc_shm_ctx_t *ctx,
                                                  u_char *ufrag, size_t len);

/* Mirror a session's running egress counters into its shm skeleton: the
 * transport-cc tallies, the AIMD pacer target, and the two reasons the pacer
 * and the IDR gate held media back. Published for /rtc/v1/stats, which Lua
 * reads out of lua_shared_dict rtc_stats. */
void ngx_rtc_shm_session_set_stats(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                   size_t len, ngx_uint_t lost,
                                   ngx_uint_t received, ngx_uint_t pacer_bps,
                                   ngx_uint_t drop_pacer, ngx_uint_t drop_gop);

/* Publish a session's lifecycle state for the stats renderer to read.
 *
 * Owner-only by construction, and the slot guard enforces it: `slot` is the
 * caller's worker slot, and a skeleton already owned by a different worker is
 * left alone. The guard deliberately accepts owner_slot == -1 as well as an
 * exact match -- in the single-worker case the OS routes the STUN binding to
 * the same process that created the session, so ngx_rtc_shm_session_bind()
 * never runs and owner_slot stays -1 until activate(). A strict comparison
 * would therefore drop exactly the ICE_BOUND and DTLS_HANDSHAKE writes, which
 * are the ones worth having.
 *
 * Note it only writes the state: owner_slot keeps its existing meaning, set by
 * bind()/activate() alone, so the cross_worker stat is unaffected. */
void ngx_rtc_shm_session_set_state(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                   size_t len, ngx_uint_t slot, uint8_t state);

/* Verify that a reused zone root was built by a binary with this layout. A zone
 * outlives both a reload and a master restart, and neither reuse path can know
 * that the new binary expects different offsets -- a struct that grew would have
 * the new code read slab metadata as fields, silently. NGX_OK when the tag
 * matches, NGX_ERROR after logging NGX_LOG_EMERG otherwise, so the caller can
 * return it directly. */
ngx_int_t ngx_rtc_shm_layout_check(const ngx_rtc_shm_ctx_t *sh, ngx_log_t *log);

/* Mark every viewer of one source for close by its "app/stream" name (admin
 * disconnect). NGX_OK when the source exists, NGX_ERROR otherwise. */
ngx_int_t ngx_rtc_shm_source_request_close(ngx_rtc_shm_ctx_t *ctx,
                                           u_char *name, size_t len);

/* Atomically claim publish ownership of a source under the pool mutex.
 * NGX_OK when `kind` now owns it (claim, or already owned by the same kind);
 * NGX_BUSY when a different publisher kind owns it; NGX_ERROR on bad args or a
 * source that no longer exists. Only a successful claim makes the returned
 * source safe to cache (publishing == 1 keeps it alive). */
ngx_int_t ngx_rtc_shm_source_try_publish(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                         size_t len, ngx_uint_t kind);

/* Release publish ownership held by `kind`. Clears publishing only when the
 * source is still owned by that kind, so a stale close cannot steal the flag
 * from a live publisher of another protocol. */
void ngx_rtc_shm_source_release_publish(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                        size_t len, ngx_uint_t kind);

/* ============================================================================
 * Publish ownership: the only writers of a local source's ownership pair
 *
 * `publisher_kind` / `publishing` exist twice: in the shm source (the
 * authority, arbitrated above) and on the process-local ngx_rtc_source_t (a
 * mirror the per-packet media path reads without taking a lock). Two copies is
 * unavoidable; two *owners* is not.
 *
 * Before these three functions, five unrelated call sites wrote the local
 * mirror by hand while the shm half was claimed separately, and the two drifted.
 * Both known ownership defects came from exactly that: a WHIP publisher whose
 * teardown did not release the shm right (name permanently unpublishable), and
 * an RTMP takeover that claimed in shm and then bailed, leaving publishing=1
 * with nobody left to clear it.
 *
 * Nothing except these functions may write src->publisher_kind or
 * src->publishing. Reading them anywhere is fine, and a caller may gate on the
 * tag to decide whether a release is worth attempting.
 *
 * src->shm_src is a cache of the shm source pointer, not ownership. All three
 * functions clear it when the ownership moves, because a release can make the
 * shm source reapable and a stale cached pointer would then dangle. Populating
 * it is also allowed from the one place that adopts another worker's claim:
 * ngx_rtc_stream_attach_from_shm(). Never populate it without holding the
 * ownership that keeps the source alive.
 * ============================================================================ */

/* Take the publish right for src->name on behalf of this process.
 *
 * NGX_OK   this process now owns the name; the mirror reads `kind`. shm_src may
 *          still be NULL -- no rtc_zone (single-worker), or the shm source
 *          vanished before the claim -- which degrades to "no cross-worker
 *          mirror", never to a dangling pointer.
 * NGX_BUSY another kind already owns it. The local mirror is cleared, because a
 *          stale tag is precisely what made an RTMP takeover of a dead WHIP
 *          name impossible until the process restarted.
 * NGX_ERROR bad argument. */
ngx_int_t ngx_rtc_publish_claim(ngx_rtc_source_t *src, ngx_uint_t kind);

/* Give up the publish right this process holds for `src`, if any. Idempotent,
 * so every teardown path (deleteStream, disconnect, session close, reap) may
 * call it unconditionally; a worker that does not own the name is a no-op. */
void ngx_rtc_publish_release(ngx_rtc_source_t *src);

/* Adopt an ownership another worker already holds, without claiming anything in
 * shm. For the media worker's local source built by attach_from_shm: it answers
 * DTLS/SRTP for a WHIP published on the signaling worker, so it must report the
 * same ownership without racing that worker's claim. */
void ngx_rtc_publish_mirror(ngx_rtc_source_t *src, ngx_uint_t kind);

/* Overwrite a source's broadcast payload types under the pool mutex. A zero
 * pt leaves the current value unchanged. */
void ngx_rtc_shm_source_set_pt(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                               uint8_t video_pt, uint8_t audio_pt);

/* Overwrite a source's SSRCs (used when a WHIP publisher's RTP SSRC becomes the
 * authoritative media SSRC). */
void ngx_rtc_shm_source_set_ssrc(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len, uint32_t video_ssrc,
                                 uint32_t audio_ssrc);

/* Expire sweep: reap half-open sessions and empty non-publishing sources whose
 * expires has elapsed. forced=1 also sweeps entries not yet due (allocation
 * pressure fallback). Safe to call on every worker. */
void ngx_rtc_shm_expire(ngx_rtc_shm_ctx_t *ctx, ngx_uint_t forced);

/* Snapshot a source's ready subscribers under the pool mutex: fills ids/slots
 * with at most max entries and returns the count (0 when the source is absent).
 * No slab pointer escapes the mutex. */
ngx_uint_t ngx_rtc_shm_source_snapshot(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                       size_t len, ngx_uint_t *ids,
                                       ngx_int_t *slots, ngx_uint_t max);

/* Cross-worker retransmit ring. The publisher appends every video packet (in
 * RTP sequence order); any worker reads one packet by RTP sequence or replays
 * the latest GOP. All ring slots are read/written under the slab pool mutex. */
typedef ngx_int_t (*ngx_rtc_shm_retransmit_cb)(void *opaque,
                                               const uint8_t *rtp,
                                               uint32_t len,
                                               uint8_t is_gop_start);

/* Drop the whole ring (new publish / stream restart). */
void ngx_rtc_shm_retransmit_reset(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                  size_t len);

/* Append one video packet; lazily allocates the ring on first use. The caller
 * passes the publisher's cached shm source pointer (valid while publishing == 1)
 * so the no-cross-worker-viewer fast path can skip the slab pool mutex and the
 * rbtree lookup entirely. */
void ngx_rtc_shm_retransmit_append(ngx_rtc_shm_ctx_t *ctx,
                                   ngx_rtc_shm_source_t *src,
                                   const uint8_t *rtp, uint32_t rtp_len,
                                   uint8_t is_gop_start);

/* Copy one cached packet by its 16-bit RTP sequence into out (<= out_cap).
 * NGX_OK on hit, NGX_DECLINED on miss. */
ngx_int_t ngx_rtc_shm_retransmit_get(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                     size_t len, uint16_t rtp_seq,
                                     u_char *out, size_t out_cap,
                                     uint16_t *out_len);

/* Replay the latest GOP (cb returns NGX_OK to continue, anything else to stop).
 * Returns the number of cached packets handed to cb -- 0 means the ring held no
 * keyframe to replay, which is what tells the caller to ask the publisher for a
 * fresh IDR instead. -1 on error (bad arguments, no such source, no memory). */
ngx_int_t ngx_rtc_shm_retransmit_replay_gop(ngx_rtc_shm_ctx_t *ctx,
                                            u_char *name, size_t len,
                                            ngx_rtc_shm_retransmit_cb cb,
                                            void *opaque);

/* Phase 2 media-ring primitives. init allocates one ring from the slab pool;
 * enqueue is MPSC, dequeue is single-consumer (both take ring->mtx). */
ngx_rtc_shm_ring_t *ngx_rtc_shm_ring_init(ngx_slab_pool_t *pool,
                                          ngx_uint_t slots);
/* Enqueue one plaintext RTP datagram plus its target session ids, writing the
 * ring slot in a single pass (no stack staging copy of the caller's buffer).
 * media = 0 video / 1 audio, gop = first packet of an IDR access unit. */
ngx_int_t ngx_rtc_shm_ring_enqueue(ngx_rtc_shm_ring_t *ring,
                                   uint8_t media, uint8_t gop,
                                   const uint8_t *rtp, uint16_t rtp_len,
                                   const ngx_uint_t *sess_ids, ngx_uint_t nsess);
ngx_int_t ngx_rtc_shm_ring_dequeue(ngx_rtc_shm_ring_t *ring,
                                   ngx_rtc_ring_entry_t *entry);
ngx_int_t ngx_rtc_shm_ring_full(const ngx_rtc_shm_ring_t *ring);
ngx_int_t ngx_rtc_shm_ring_empty(const ngx_rtc_shm_ring_t *ring);

extern ngx_module_t ngx_rtc_core_module;

#endif /* NGX_RTC_SHM_H */
