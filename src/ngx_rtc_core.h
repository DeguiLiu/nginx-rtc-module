/*
 * ngx_rtc_core.h - shared RTC source / session registry.
 *
 * A source holds one live stream's RTP state (SSRC/PT/seq/timestamp, SPS/PPS)
 * and its list of player sessions. A session binds one WebRTC player (ICE +
 * DTLS + SRTP) to a source. The HTTP signaling module and the stream UDP module
 * share these structures.
 *
 * The registries themselves are core-private (ngx_rtc_core.c). When compiled
 * into nginx (NGX_PTR_SIZE is defined by ngx_core.h/ngx_auto_config.h) the
 * source registry is an ngx_rbtree keyed by the "app/stream" name and the
 * session / subscriber lists are ngx_queue; the link fields below exist only
 * in that build so this header stays unit-testable on the host without nginx
 * headers. Single-worker MVP: no locks are needed.
 */

#ifndef NGX_RTC_CORE_H
#define NGX_RTC_CORE_H

#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include "ngx_rtc_rtp.h"
#include "ngx_rtc_dtls.h"
#include "ngx_rtc_srtp.h"
#include "ngx_rtc_session_fsm.h"

/* ============================================================================
 * GOP ring (plaintext RTP cache) constants (docs/zero-copy-optimization.md 4.1)
 * ============================================================================ */

/* Largest plaintext RTP packet the bridge can build (12 RTP + FU-A header +
 * NGX_RTC_H264_MTU payload). */
#define NGX_RTC_MAX_RTP_PKT   (NGX_RTC_RTP_HEADER_SIZE + 2u + NGX_RTC_H264_MTU)

/* libsrtp2 AES-CM-128-HMAC-SHA1-80 auth tag. */
#define NGX_RTC_SRTP_TAG_LEN  10u

/* Per-session cipher buffer: plaintext RTP + auth tag + sendto headroom. */
#define NGX_RTC_CIPHER_CAP    (NGX_RTC_MAX_RTP_PKT + NGX_RTC_SRTP_TAG_LEN + 32u)

/* NACK retransmission budget: at most N packets per window per session. */
#define NGX_RTC_NACK_WINDOW_MS  100u
#define NGX_RTC_NACK_BUDGET     128u

/* Return code for the token-bucket pacer: the packet must be deferred or
 * dropped until tokens are replenished (nginx's NGX_AGAIN occupies -2, which
 * is NGX_RTC_ERR_TOO_SMALL here, so the pacer uses -6). */
#ifndef NGX_RTC_AGAIN
#define NGX_RTC_AGAIN (-6)
#endif

/* ============================================================================
 * Send pacing (token bucket) + TWCC loss-driven bitrate adaptation (GCC-lite).
 * ============================================================================ */

/* Target send-rate clamp. 64 kbps is the floor where H264 video is still
 * barely watchable; 8 Mbps is the ceiling for a typical 1080p WebRTC uplink. */
#define NGX_RTC_PACER_MIN_BPS   64000u
#define NGX_RTC_PACER_MAX_BPS   8000000u

/* The bucket holds this many milliseconds of the target rate: one burst window.
 * A 100 ms burst lets a keyframe flush immediately but still caps an
 * uncongested sender from flooding the socket for longer than ~100 ms. */
#define NGX_RTC_PACER_BURST_MS  100u

/* Upper bound on the ready-subscriber snapshot a source caches in-process so
 * broadcast_rtp can read it lock-free (avoids the slab pool mutex per packet).
 * Matches NGX_RTC_BRIDGE_MAX_SNAPSHOT; a stream beyond this many viewers is
 * truncated and the rest are dropped from the fast path (they still count as
 * cross-worker sends only up to this bound). */
#define NGX_RTC_SOURCE_MAX_SNAPSHOT 256u

/* TWCC loss window: AIMD re-evaluates once per 500 ms of feedback, or earlier
 * when a minimum number of packets has arrived (whichever comes first), so
 * sparse streams still adapt through the time bound. */
#define NGX_RTC_TWCC_WINDOW_MS  500u
#define NGX_RTC_TWCC_MIN_PKTS   20u

/* Loss thresholds in per-mille (integer math, no float on the data path):
 *   > 50 permille (5%)  -> multiplicative decrease x0.85
 *   < 20 permille (2%)  -> additive increase +8% of current rate
 * Mirrors WebRTC GCC's loss-based AIMD bands (SRS 6.0 leaves on_rtcp_feedback_twcc
 * a no-op, so the thresholds follow the task spec / Google GCC defaults). */
#define NGX_RTC_TWCC_LOSS_HIGH_PM  50u
#define NGX_RTC_TWCC_LOSS_LOW_PM   20u
#define NGX_RTC_TWCC_DECREASE_NUM  85u
#define NGX_RTC_TWCC_DECREASE_DEN  100u
#define NGX_RTC_TWCC_INCREASE_NUM   8u
#define NGX_RTC_TWCC_INCREASE_DEN  100u

/* GOP ring capacity; power of two so slot = head & (capacity - 1). 2048 slots
 * x ~1224 B/slot ~= 2.4 MB per source, enough for a 1-2 s GOP at 1080p. */
#define NGX_RTC_GOP_RING_CAP  2048u

/* Source name ("app/stream") capacity, including the NUL terminator. */
#define NGX_RTC_SOURCE_NAME_MAX  128u

/* Max H264 video tag body the bridge copies out of the RTMP chain before NALU
 * parsing. nginx-rtmp delivers a large message as an ngx_chain_t of chunk-sized
 * buffers; NALU parsing needs a contiguous view, so the bridge concatenates
 * into this per-source buffer. 256 KiB covers a 1080p IDR. */
#define NGX_RTC_MAX_VIDEO_BODY   (256u * 1024u)

/* Max FLV audio tag body the bridge copies out of the RTMP chain before AAC
 * transcoding. One AAC-LC frame at 48 kHz is well under 1 KiB even at 320
 * kbps; 16 KiB leaves room for publishers that batch several frames in one
 * audio tag. */
#define NGX_RTC_MAX_AUDIO_BODY   (16u * 1024u)

/* One ring slot: a cached plaintext RTP packet shared by late subscribers. */
typedef struct
{
    uint8_t  data[NGX_RTC_MAX_RTP_PKT]; /* plaintext RTP packet */
    uint16_t len;                        /* valid bytes in data */
    uint8_t  is_gop_start;               /* first packet of an IDR access unit */
} ngx_rtc_rtp_cache_slot_t;

typedef struct
{
    ngx_rtc_rtp_cache_slot_t *slots;    /* fixed array, lazy allocated once */
    uint32_t capacity;                  /* power of two, slots[head & (cap-1)] */
    uint32_t head;                      /* next absolute write index */
    uint32_t count;                     /* valid entries in [head-count, head) */
    uint32_t gop_start;                 /* absolute index of the latest GOP */
} ngx_rtc_rtp_ring_t;

/* The source struct is defined below; session references it before the
 * definition, so the typedef must be visible first. */
typedef struct ngx_rtc_source_s ngx_rtc_source_t;

struct ngx_rtc_session_s {
    ngx_rtc_source_t *source;

    /* RTP payload types negotiated with this session's offer (RFC 3264). The
     * source broadcasts the plaintext stream with the first viewer's PT; every
     * send rewrites the RTP header PT byte to these per-session values so the
     * answer PT is always present in that session's offer. */
    uint8_t video_pt;
    uint8_t audio_pt;

    /* transport-wide-cc RTP header-extension id per medium as negotiated with
     * this session (0 = the offer did not enable it for that medium). twcc_seq
     * is a monotonic 16-bit transport sequence stamped on every real send,
     * including RTX replays; the peer's transport-cc feedback keys on it. */
    uint8_t  twcc_video_ext;
    uint8_t  twcc_audio_ext;
    uint16_t twcc_seq;
    uint32_t twcc_lost;      /* cumulative lost via transport-cc feedback */
    uint32_t twcc_received;  /* cumulative received via transport-cc feedback */
    uint8_t  publishing;     /* 1 = WHIP publisher (receives media), 0 = player */

    /* ICE credentials from the SDP offer; STUN ufrag matches on these. */
    ngx_uint_t id;       /* shm session id (monotonic, never reused) */
    char ice_ufrag[64];
    char ice_pwd[256];

    /* DTLS/SRTP state. The session state machine (NEW -> ICE_BOUND ->
     * DTLS_HANDSHAKE -> SRTP_READY -> CLOSED) drives the lifecycle; there are
     * no separate boolean flags for dtls/srtp readiness. */
    ngx_rtc_dtls_t  dtls;
    ngx_rtc_srtp_t  srtp;
    ngx_rtc_hsm_t   fsm;
    const ngx_rtc_hsm_state_t *fsm_path[NGX_RTC_SESSION_FSM_MAX_DEPTH];

    /* Peer UDP address + connection (set by stream UDP module). */
    void            *conn;      /* ngx_connection_t * */
    struct sockaddr *peer_addr;
    socklen_t        peer_len;

    /* Per-session SRTP output buffer (reused on every send, no stack buffer). */
    uint8_t         cipher[NGX_RTC_CIPHER_CAP];

    /* Monotonic-millisecond timestamp of the last inbound packet
     * (STUN/DTLS/SRTCP); used by the idle reaper. */
    ngx_msec_t      last_active;

    /* Send-path failure accounting. A live UDP datagram that cannot be sent
     * (socket buffer full) is counted and dropped; real-time media must not
     * queue unboundedly, so the client recovers via NACK/PLI instead. */
    ngx_uint_t      send_failed;   /* c->send NGX_ERROR / short write */
    ngx_uint_t      send_eagain;   /* c->send NGX_AGAIN (UDP buffer full) */

    /* NACK retransmission budget window. Limits how many GOP-ring packets one
     * session may retransmit per interval, so a NACK storm cannot burst the
     * whole ring into the socket; the client re-NACKs what was skipped. */
    ngx_msec_t      nack_window_start;
    ngx_uint_t      nack_retransmitted;

    /* NACK dedup for the shared retransmit cache (source GOP ring / shm ring).
     * A seq already retransmitted in the current NACK window is skipped so a
     * re-NACK of the same packet does not burst duplicates; cleared on each
     * window roll. Bounded by NGX_RTC_NACK_BUDGET. */
    uint16_t    nack_seen[NGX_RTC_NACK_BUDGET];
    ngx_uint_t  nack_seen_count;

    /* Send pacing: token bucket filled at pacer_target_bps (bytes are tracked
     * with integer math; one token = one byte). pacer_started distinguishes
     * "never anchored" from "anchored at now_ms == 0" so the first admit only
     * anchors the clock without a refill; the bucket starts full for an initial
     * burst (fast-start GOP replay). */
    uint8_t  pacer_started;
    uint64_t pacer_target_bps;   /* AIMD-adjusted target send rate (bps) */
    uint64_t pacer_tokens;       /* token bucket level in bytes */
    uint64_t pacer_last_ms;      /* monotonic ms of the last refill */

    /* TWCC loss window statistics. twcc_lost/twcc_received above are cumulative;
     * the *_win_* counters accumulate one AIMD evaluation window and reset after
     * each rate decision. */
    uint32_t twcc_win_lost;
    uint32_t twcc_win_received;
    uint8_t  twcc_win_started;   /* 1 once the first feedback opened a window */
    uint64_t twcc_win_start_ms;  /* window anchor (monotonic ms) */

#ifdef NGX_PTR_SIZE
    /* nginx data-structure links (see header comment). The queue link is
     * initialised self-referential before the session is added to the global
     * list so ngx_queue_remove() on an unsubscribed session is a safe no-op. */
    ngx_queue_t     queue;      /* global session list link */
    ngx_queue_t     sub_queue;  /* source->subscribers list link */
    ngx_str_node_t  sn;         /* session rbtree node (key = ICE ufrag) */
    void           *dtls_timer; /* ngx_event_t *, DTLS retransmit timer */
#endif
};

struct ngx_rtc_source_s {
    char     name[NGX_RTC_SOURCE_NAME_MAX];   /* "app/stream" */
    uint8_t  publishing;   /* 1 while the RTMP publisher is connected */
    uint32_t video_ssrc;
    uint8_t  video_pt;
    uint32_t audio_ssrc;
    uint8_t  audio_pt;

    /* Shared RTP counters (the plaintext RTP stream is broadcast). */
    uint16_t video_seq;
    uint16_t audio_seq;
    uint32_t video_ts;      /* 90kHz, last video frame timestamp */
    uint32_t audio_ts;      /* 48kHz, next Opus frame timestamp (monotonic) */
    uint8_t  audio_ts_valid;/* 1 once audio_ts is initialised from the first AAC tag */
    uint8_t  have_ts;

    /* Last raw-frame wall time (RTMP tag ms) per medium. Video ts is re-mapped
     * from the RTMP clock every frame, audio ts is a free-running Opus counter
     * seeded once; the bridge observability log diffs the two to surface drift. */
    uint32_t last_video_rtmp_ms;
    uint32_t last_audio_rtmp_ms;
    uint32_t ring_drops;    /* shm media-ring enqueue failures (silent drops) */

    /* Sender-report statistics, accumulated by the bridge emit path. */
    uint32_t video_pkts;
    uint32_t video_octets;
    uint32_t audio_pkts;
    uint32_t audio_octets;

    /* Cached SPS + PPS from the AVC sequence header (STAP-A before each IDR). */
    uint8_t  sps[256];
    uint32_t sps_len;
    uint8_t  pps[256];
    uint32_t pps_len;

    /* profile-level-id extracted from the SPS (bytes 1..3 after the NAL header)
     * so the SDP answer's a=fmtp echoes the real stream profile instead of a
     * hardcoded value; a mismatch makes Chrome's HW decoder render garbage. */
    uint8_t  sps_profile_level_id[3];
    uint8_t  sps_profile_level_id_valid;

    /* Cached AAC AudioSpecificConfig (sequence header) + transcoder handle. */
    uint8_t  audio_asc[64];
    uint32_t audio_asc_len;
    void    *audio_ctx;   /* ngx_rtc_audio_t *, owned by the bridge module */

    /* GOP ring: caches recent plaintext RTP for fast-start replay + NACK. */
    ngx_rtc_rtp_ring_t gop;

    /* Cached ready-subscriber snapshot (session id + owner slot). Refreshed
     * only when the shm source's subscribers_version changes, so broadcast_rtp
     * reads this fixed array lock-free instead of taking the slab pool mutex on
     * every packet. shm_src is the ngx_rtc_shm_source_t *, valid while this
     * source is publishing (the shm source is not reaped while publishing=1). */
    void       *shm_src;
    ngx_uint_t  snap_version;
    ngx_uint_t  snap_count;
    ngx_uint_t  snap_ids[NGX_RTC_SOURCE_MAX_SNAPSHOT];
    ngx_int_t   snap_slots[NGX_RTC_SOURCE_MAX_SNAPSHOT];

    /* Contiguous scratch for one RTMP tag body. nginx-rtmp delivers a large
     * message as an ngx_chain_t of chunk-sized buffers, so the bridge
     * concatenates the whole tag here before parsing (see
     * ngx_rtmp_rtc_chain_copy). A video and an audio tag are handled one at a
     * time by the same RTMP handler, so a union reuses one buffer for both
     * (pipeline data-structure reuse; the video body is the larger bound). */
    union {
        uint8_t  video_body[NGX_RTC_MAX_VIDEO_BODY]; /* 256 KiB, H264 tag */
        uint8_t  audio_body[NGX_RTC_MAX_AUDIO_BODY]; /* 16 KiB, AAC tag */
    };

#ifdef NGX_PTR_SIZE
    /* Source-registry rbtree node (str is set to point at name[]) and the
     * subscriber queue head; see the header comment. */
    ngx_str_node_t  sn;          /* registry node (key = "app/stream" hash) */
    ngx_queue_t     subscribers; /* subscriber queue head */
#endif
};

/* Find or create a source by "app/stream". Returns NULL on alloc failure. */
ngx_rtc_source_t *ngx_rtc_source_get(const char *name);

/* Find an existing source without creating it. Returns NULL if absent. */
ngx_rtc_source_t *ngx_rtc_source_find(const char *name);

/* Remove a source that has no subscribers. The caller must already release
 * audio_ctx (owned by the bridge module) before calling this. */
void ngx_rtc_source_remove(const char *name);

/* Iterate the global source registry (the registry itself is core-private).
 * next(NULL) returns the first source. */
ngx_rtc_source_t *ngx_rtc_source_first(void);
ngx_rtc_source_t *ngx_rtc_source_next(ngx_rtc_source_t *src);

/* Find a session by its local ICE ufrag. */
ngx_rtc_session_t *ngx_rtc_session_find(const char *ufrag);

/* Iterate the global session list (the list itself is core-private).
 * next(NULL) returns the first session. */
ngx_rtc_session_t *ngx_rtc_session_first(void);
ngx_rtc_session_t *ngx_rtc_session_next(ngx_rtc_session_t *sess);
ngx_rtc_session_t *ngx_rtc_session_find_by_id(ngx_uint_t id);

/* Insert / remove a session from the global list. */
void ngx_rtc_session_add(ngx_rtc_session_t *sess);
void ngx_rtc_session_remove(ngx_rtc_session_t *sess);

/* Add / remove a session from a source's subscriber list. */
void ngx_rtc_source_subscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess);
void ngx_rtc_source_unsubscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess);

/* Iterate a source's subscriber queue. */
ngx_rtc_session_t *ngx_rtc_source_first_subscriber(ngx_rtc_source_t *src);
ngx_rtc_session_t *ngx_rtc_source_next_subscriber(ngx_rtc_source_t *src,
                                                  ngx_rtc_session_t *sess);

/*
 * Send one plaintext RTP packet to a session: copy into the session cipher
 * buffer, SRTP-protect in place, then c->send. No-op when the session is not
 * ready / has no bound connection.
 *
 * Returns 1 when the datagram was handed to the socket, 0 when it was dropped
 * (send EAGAIN / short write / not ready). The replay path uses this to stop
 * bursting into a full socket instead of wasting the remaining GOP.
 */
int ngx_rtc_session_send_rtp(ngx_rtc_session_t *sess, const uint8_t *rtp,
                             uint32_t len);

/* Cache one plaintext RTP packet in the source GOP ring. is_gop_start marks
 * the first packet of an IDR access unit (STAP-A SPS/PPS). */
void ngx_rtc_rtp_ring_push(ngx_rtc_rtp_ring_t *r, const uint8_t *rtp,
                           uint32_t len, uint8_t is_gop_start);

/* Replay the latest GOP to a freshly subscribed session (fast startup). */
void ngx_rtc_rtp_ring_replay(ngx_rtc_rtp_ring_t *r, ngx_rtc_session_t *sess);

/*
 * Locate a cached packet by its 16-bit RTP sequence number (handles 16-bit
 * wrap within the retained window). Returns NGX_RTC_OK and sets *out and
 * *out_len on a hit; NGX_RTC_ERR_INVALID / NGX_RTC_ERR_PARSE otherwise.
 * Used by NACK.
 */
int32_t ngx_rtc_rtp_ring_get(ngx_rtc_rtp_ring_t *r, uint16_t rtp_seq,
                             const uint8_t **out, uint16_t *out_len);

/* Lazily allocate a ring's slot array. 1 on success (already-allocated or
 * freshly calloc'd), 0 on allocation failure. Kept out of ngx_rtc_rtp_ring_push
 * so the per-session RTX ring can use its own capacity. */
int ngx_rtc_rtp_ring_reserve(ngx_rtc_rtp_ring_t *r, uint32_t capacity);

/* Per-session retransmit: resolve one video packet from the source GOP ring
 * (same-worker path) and send it with per-window NACK dedup. NGX_RTC_OK on
 * send; NGX_RTC_ERR_PARSE on miss/dedup/send-fail; NGX_RTC_ERR_INVALID on a
 * NULL session. */
int32_t ngx_rtc_session_retransmit(ngx_rtc_session_t *sess, uint16_t seq);

/* Dedup + send for a retransmit candidate whose data/len were already resolved
 * (shared by the source-ring and shm-ring paths). Same return codes. */
int32_t ngx_rtc_session_retransmit_send(ngx_rtc_session_t *sess, uint16_t seq,
                                        const uint8_t *data, uint32_t len);

/* Clear the per-window NACK dedup set (called on NACK window roll). */
void ngx_rtc_session_nack_reset(ngx_rtc_session_t *sess);

/* 1 when the local source GOP ring holds data (publisher worker). A session
 * whose source passes this check retransmits from the ring; otherwise it must
 * use the shm retransmit ring. */
int ngx_rtc_source_gop_ready(const ngx_rtc_source_t *src);

/*
 * Initialise the per-session pacer token bucket. start_bps is clamped to
 * [NGX_RTC_PACER_MIN_BPS, NGX_RTC_PACER_MAX_BPS]; the bucket starts full (one
 * NGX_RTC_PACER_BURST_MS burst) and the clock is anchored by the first admit.
 */
void ngx_rtc_session_pacer_init(ngx_rtc_session_t *sess, uint64_t start_bps);

/*
 * Try to admit one packet into the paced stream. Tokens are replenished at
 * pacer_target_bps for every now_ms - pacer_last_ms elapsed, capped at one burst
 * window. Returns NGX_RTC_OK when the packet is charged, NGX_RTC_AGAIN when
 * there are not enough tokens yet (caller defers or drops), NGX_RTC_ERR_INVALID
 * for a NULL session. now_ms is the monotonic clock in milliseconds.
 */
int32_t ngx_rtc_session_pacer_admit(ngx_rtc_session_t *sess, uint32_t pkt_bytes,
                                    uint64_t now_ms);

/*
 * Feed one transport-cc feedback window into the loss-based rate controller.
 * lost/received are this feedback's packet counts; they accumulate into the
 * TWCC window and, once the window has enough packets or elapsed time, the
 * target rate is AIMD-adjusted and the window resets.
 */
void ngx_rtc_session_on_twcc(ngx_rtc_session_t *sess, uint32_t lost,
                             uint32_t received, uint64_t now_ms);

/*
 * Set the pacer target rate from an absolute bitrate hint (e.g. REMB). bps is
 * clamped to [NGX_RTC_PACER_MIN_BPS, NGX_RTC_PACER_MAX_BPS]; the token bucket
 * level is left untouched so a sudden cap does not drop an already-admitted
 * burst.
 */
void ngx_rtc_session_pacer_set_target(ngx_rtc_session_t *sess, uint64_t bps);

#endif /* NGX_RTC_CORE_H */
