/*
 * ngx_rtc_shm.h - shared-memory skeleton for cross-worker RTC metadata.
 *
 * Phase 0 of docs/multi-worker-shm-design.md: a `rtc_zone` directive plus a
 * slab-backed source / session registry. Only value types and pointers to other
 * shm allocations live in this zone; DTLS/SRTP/connection/audio handles stay in
 * per-worker process memory and are attached by the later phases.
 *
 * The pure-C protocol core (rtp/sdp/stun/dtls/srtp) remains nginx-free; this
 * header is nginx-only and is not compiled into the host unit tests.
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

typedef struct ngx_rtc_shm_source_s  ngx_rtc_shm_source_t;
typedef struct ngx_rtc_shm_session_s ngx_rtc_shm_session_t;

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
    ngx_atomic_t           expires;    /* msec absolute, 0 = no expire (empty source reap) */
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
    ngx_uint_t             remote_subscribers; /* viewers on a worker != publisher_slot */

    /* Cross-worker retransmit ring (video): lazily allocated on first video
     * packet; any worker answers NACK/PLI from it (per-ring lock). */
    ngx_rtc_shm_retransmit_t   *retransmit;
    ngx_uint_t                  retransmit_alloc_failed; /* lazy ring alloc failures */
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
} ngx_rtc_shm_ctx_t;

/* Per-cycle configuration (process memory). One rtc_zone per cycle. */
typedef struct {
    ngx_shm_zone_t      *shm_zone;   /* NULL when rtc_zone is not configured */
    ngx_rtc_shm_ctx_t   *sh;         /* shm root == shpool->data */
    ngx_slab_pool_t     *shpool;     /* slab allocator */
    ngx_uint_t           nworkers;    /* from ccf->worker_processes */
    ngx_uint_t           ring_slots;  /* media-ring capacity per worker */
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

/* Mirror a session's running transport-cc counters into its shm skeleton. */
void ngx_rtc_shm_session_set_twcc(ngx_rtc_shm_ctx_t *ctx, u_char *ufrag,
                                  size_t len, ngx_uint_t lost,
                                  ngx_uint_t received);

/* Mark every viewer of one source for close by its "app/stream" name (admin
 * disconnect). NGX_OK when the source exists, NGX_ERROR otherwise. */
ngx_int_t ngx_rtc_shm_source_request_close(ngx_rtc_shm_ctx_t *ctx,
                                           u_char *name, size_t len);

/* Set a source's publishing flag under the pool mutex (the only way the bridge
 * may mutate it; free_locked reads it under the same mutex). */
void ngx_rtc_shm_source_set_publishing(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                       size_t len, ngx_uint_t publishing);

/* Overwrite a source's broadcast payload types under the pool mutex. A zero
 * pt leaves the current value unchanged. */
void ngx_rtc_shm_source_set_pt(ngx_rtc_shm_ctx_t *ctx, u_char *name, size_t len,
                               uint8_t video_pt, uint8_t audio_pt);

/* Overwrite a source's SSRCs (used when a WHIP publisher's RTP SSRC becomes the
 * authoritative media SSRC). */
void ngx_rtc_shm_source_set_ssrc(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                 size_t len, uint32_t video_ssrc,
                                 uint32_t audio_ssrc);

/* Mirror a source's packet/octet counters (used by the WHIP producer path,
 * which does not run the RTMP bridge's sync_shm). */
void ngx_rtc_shm_source_set_media_stats(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                        size_t len, ngx_uint_t video_pkts,
                                        ngx_uint_t video_octets,
                                        ngx_uint_t audio_pkts,
                                        ngx_uint_t audio_octets);

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
 * the latest GOP. All ring slots are read/written under the per-ring lock. */
typedef ngx_int_t (*ngx_rtc_shm_retransmit_cb)(void *opaque,
                                               const uint8_t *rtp,
                                               uint32_t len,
                                               uint8_t is_gop_start);

/* Drop the whole ring (new publish / stream restart). */
void ngx_rtc_shm_retransmit_reset(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                  size_t len);

/* Append one video packet; lazily allocates the ring on first use. */
void ngx_rtc_shm_retransmit_append(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                   size_t len, const uint8_t *rtp,
                                   uint32_t rtp_len, uint8_t is_gop_start);

/* Copy one cached packet by its 16-bit RTP sequence into out (<= out_cap).
 * NGX_OK on hit, NGX_DECLINED on miss. */
ngx_int_t ngx_rtc_shm_retransmit_get(ngx_rtc_shm_ctx_t *ctx, u_char *name,
                                     size_t len, uint16_t rtp_seq,
                                     u_char *out, size_t out_cap,
                                     uint16_t *out_len);

/* Replay the latest GOP (cb returns NGX_OK to continue, anything else to stop). */
ngx_int_t ngx_rtc_shm_retransmit_replay_gop(ngx_rtc_shm_ctx_t *ctx,
                                            u_char *name, size_t len,
                                            ngx_rtc_shm_retransmit_cb cb,
                                            void *opaque);

/* Phase 2 media-ring primitives. init allocates one ring from the slab pool;
 * enqueue is MPSC, dequeue is single-consumer (both take ring->mtx). */
ngx_rtc_shm_ring_t *ngx_rtc_shm_ring_init(ngx_slab_pool_t *pool,
                                          ngx_uint_t slots);
ngx_int_t ngx_rtc_shm_ring_enqueue(ngx_rtc_shm_ring_t *ring,
                                   const ngx_rtc_ring_entry_t *entry);
ngx_int_t ngx_rtc_shm_ring_dequeue(ngx_rtc_shm_ring_t *ring,
                                   ngx_rtc_ring_entry_t *entry);
ngx_int_t ngx_rtc_shm_ring_full(const ngx_rtc_shm_ring_t *ring);
ngx_int_t ngx_rtc_shm_ring_empty(const ngx_rtc_shm_ring_t *ring);

extern ngx_module_t ngx_rtc_core_module;

#endif /* NGX_RTC_SHM_H */
