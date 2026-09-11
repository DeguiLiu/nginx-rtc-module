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
#include "ngx_rtc_jitter.h"

/* ============================================================================
 * GOP ring (plaintext RTP cache) constants (docs/zero-copy-optimization.md 4.1)
 * ============================================================================ */

/* libsrtp2 AES-CM-128-HMAC-SHA1-80 auth tag. */
#define NGX_RTC_SRTP_TAG_LEN  10u

/* Per-session cipher buffer: plaintext RTP + auth tag + sendto headroom. */
#define NGX_RTC_CIPHER_CAP    (NGX_RTC_MAX_RTP_PKT + NGX_RTC_SRTP_TAG_LEN + 32u)

/* NACK retransmission budget: at most N packets per window per session. */
#define NGX_RTC_NACK_WINDOW_MS  100u
#define NGX_RTC_NACK_BUDGET     128u
/* Sender-side NACK response backoff: a saturated window doubles its width up
 * to this ceiling, so a NACK storm spreads the same budget over a longer span
 * instead of retransmitting at a fixed 100 ms cadence. A quiet window resets. */
#define NGX_RTC_NACK_WINDOW_MAX_MS  800u

/* Consecutive video EAGAINs tolerated before the session falls back to the
 * next IDR. One full UDP send buffer is transient (a single datagram), so
 * dropping a whole GOP on the first EAGAIN would cost up to one GOP interval
 * of video for a hiccup the client can still absorb through NACK/PLI. */
#define NGX_RTC_EAGAIN_STREAK_MAX   3u

/* Idle time after which a partial EAGAIN run stops counting toward
 * NGX_RTC_EAGAIN_STREAK_MAX. The run measures sustained backpressure, so a
 * gap longer than this means the burst ended and the counter restarts; without
 * it a still picture (no video packets) would freeze a stale run and let the
 * first EAGAIN after the picture resumes drop a whole GOP. */
#define NGX_RTC_EAGAIN_STREAK_IDLE_MS 1000u

/* Upper bound on the packets the pacer may let through while a keyframe access
 * unit is open. The unit normally closes on the RTP marker bit, but the source
 * does not have to set one -- the bridge computes the marker from the last NALU
 * of the RTMP message, and a trailing B-frame is skipped, so an access unit can
 * end without any packet carrying it. Without this cap that would leave the
 * exemption open, and the pacer switched off, for the rest of the session. */
#define NGX_RTC_KEYFRAME_MAX_PKTS   256u

/*
 * Runtime tunables, filled once from the rtc_* directives by
 * ngx_rtc_core_init_conf() before the workers fork, so every worker inherits
 * the same read-only snapshot.
 *
 * They live in a plain C struct rather than on the session or source because
 * the pure C units (core, jitter) must read them without pulling in the nginx
 * conf path - the host unit tests have no ngx_cycle and no conf at all.
 * ngx_rtc_core_tunables() never returns NULL: until a setter runs it yields the
 * compile-time table below, so host tests and any deployment that does not set
 * the directives keep the historical values.
 *
 * Deliberately NOT here, because they are struct array dimensions and would
 * force the fixed arrays onto the heap on a per-packet hot path:
 *   NGX_RTC_NACK_BUDGET (session nack_seen[]), NGX_RTC_JITTER_CAP (jitter slots[]).
 * Also not here: NGX_RTC_SHM_SYNC_MS / NGX_RTC_SHM_SOURCE_EXPIRE_MS, which are
 * a paired invariant (the pointer cache must expire inside the reap grace);
 * exposing them separately would let a config typo dangle a cached shm_src.
 */
typedef struct {
    uint32_t  jitter_timeout_ms;   /* gap timeout of the WHIP uplink reorder buffer */
    uint32_t  nack_window_ms;      /* base NACK retransmit window per session */
    uint32_t  nack_window_max_ms;  /* ceiling the backoff doubles up to */
    uint32_t  eagain_streak_max;   /* consecutive video EAGAINs before drop-to-IDR */
    uint32_t  gop_ring_slots;      /* per-source GOP retransmit ring capacity (power of two) */
} ngx_rtc_tunables_t;

/* Install the configuration snapshot (config time only; not thread safe). */
void ngx_rtc_core_set_tunables(const ngx_rtc_tunables_t *t);

/* Current tunables; the compile-time defaults when none were installed. */
const ngx_rtc_tunables_t *ngx_rtc_core_tunables(void);

/* How long a process-local source may trust its cached shm source pointer
 * without re-resolving it. A release on another worker (signaling close, or the
 * relayed publisher's close in an auto-push setup) can make that shm source
 * reapable while this worker still sends media, so the pointer is re-validated
 * on a slow cadence: one pool-mutex lookup per interval instead of two per
 * packet, with a bounded staleness window. */
#define NGX_RTC_SHM_SYNC_MS         1000u

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

    /* Where the offered media went, per session. Without these the only visible
     * symptom of an egress collapse is a low send rate, and no single counter
     * says which stage refused the packet: the token bucket, the GOP backpressure
     * gate, or the socket. A pacer drop spends no transport sequence (the gate
     * runs before the stamp), so it is invisible to transport-cc; a packet
     * dropped after the stamp is not, and the peer reports it as path loss. */
    ngx_uint_t drop_gop;     /* video held off by drop_until_gop */
    ngx_uint_t drop_pacer;   /* refused by the token bucket and dropped */
    ngx_uint_t srtp_failed;  /* abandoned after the transport seq was stamped */
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
    uint8_t         drop_until_gop; /* video-only: after a send failure, drop
                                     * video packets until the next IDR so the
                                     * client recovers to a clean keyframe;
                                     * audio is never dropped here */
    ngx_uint_t      send_eagain_streak; /* consecutive video EAGAINs since the
                                    * last successful send; only a sustained
                                    * streak triggers drop_until_gop, and the
                                    * streak restarts once it arms the flag */
    ngx_msec_t      send_eagain_ms; /* when the current streak's last EAGAIN
                                    * happened, so a run interrupted by an idle
                                    * gap (still picture) does not carry over */

    /* Keyframe access unit in flight. A GOP start opens it and the RTP marker
     * bit closes it; while it is open the pacer admits its packets whatever the
     * budget, because a keyframe the pacer tears up is one the viewer can never
     * recover from on its own. is_gop_start marks only the SPS/PPS packet that
     * opens the unit, so without this the IDR fragments that carry the picture
     * would still be paced out. */
    ngx_uint_t      keyframe_open;  /* 1 while a keyframe access unit is in
                                     * flight: opened by a GOP start, closed by
                                     * the RTP marker bit or the packet cap */
    ngx_uint_t      keyframe_pkts;  /* exempted since it opened; caps a unit
                                     * whose source never sets the marker */

    /* NACK retransmission budget window. Limits how many GOP-ring packets one
     * session may retransmit per interval, so a NACK storm cannot burst the
     * whole ring into the socket; the client re-NACKs what was skipped. */
    ngx_msec_t      nack_window_start;
    ngx_msec_t      nack_window_ms;  /* current window width, backoff-adjusted */
    ngx_uint_t      nack_retransmitted; /* attempts spent this window */

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

    /* Set by the DTLS completion callback when it cannot finish the handshake
     * (SRTP key export or ngx_rtc_srtp_create failed). That callback is invoked
     * synchronously from inside ngx_rtc_dtls_on_data() and
     * ngx_rtc_dtls_handle_timeout(), and both callers keep using their session
     * pointer after the call returns -- so closing from inside the callback
     * freed the session out from under them. The callers close it instead, the
     * moment they regain control. Only ever means "close as soon as you can";
     * it is not a general teardown latch. */
    uint8_t  close_pending;

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
    uint8_t  publishing;   /* 1 while a publisher is connected */
    uint8_t  publisher_kind; /* 0=none, 1=RTMP, 2=WHIP (ownership tag) */
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

    /* WHIP uplink reorder buffer: caches video RTP by sequence number so a
     * lossy WAN does not deliver H264 out of order to the broadcast path. */
    ngx_rtc_jitter_t jitter;

    /* Cached ready-subscriber snapshot (session id + owner slot). Refreshed
     * only when the shm source's subscribers_version changes, so broadcast_rtp
     * reads this fixed array lock-free instead of taking the slab pool mutex on
     * every packet. shm_src is the ngx_rtc_shm_source_t *; it stays valid while
     * publishing=1 keeps the reaper away, and shm_sync_ms stamps the last
     * re-resolution so a release on another worker cannot leave this worker
     * writing through a reapable pointer. */
    void       *shm_src;
    ngx_msec_t  shm_sync_ms;
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

/* Report an event no session state handled (installed with
 * ngx_rtc_session_fsm_set_reporter when a session is created). Lives here
 * because the session_fsm core is libc-only and has no logger. */
void ngx_rtc_session_fsm_report_unhandled(ngx_rtc_hsm_t *sm,
                                          const ngx_rtc_hsm_event_t *event);

/* Add / remove a session from a source's subscriber list. */
void ngx_rtc_source_subscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess);
void ngx_rtc_source_unsubscribe(ngx_rtc_source_t *src, ngx_rtc_session_t *sess);

/* 1 when any session still points at src (subscribed or publisher-assigned). */
ngx_uint_t ngx_rtc_source_has_holder(const ngx_rtc_source_t *src);

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
 * (send EAGAIN / short write / not ready / video backpressure to next GOP).
 * is_gop_start marks the first packet of an IDR access unit (video only) so the
 * video backpressure can resume at a clean keyframe. The replay path uses the
 * zero return to stop bursting into a full socket instead of wasting the rest
 * of the GOP.
 */
int ngx_rtc_session_send_rtp(ngx_rtc_session_t *sess, const uint8_t *rtp,
                             uint32_t len, uint8_t is_gop_start);

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

/* Charge one retransmit attempt against the current window's budget, returning
 * 1 when the attempt may proceed. The budget bounds attempts, not deliveries:
 * when the pacer refuses every retransmit a success-counted budget never
 * advances, so the caller's loop never breaks and a single NACK feedback packet
 * can offer every one of its expanded sequences to the pacer. Counting only
 * successes would also silently disable the backoff below, which reads the same
 * counter. */
int ngx_rtc_session_nack_budget_take(ngx_rtc_session_t *sess);

/* Advance the sender-side NACK response window if `now` has passed its current
 * width, applying the backoff policy: a saturated window (retransmits reached
 * NGX_RTC_NACK_BUDGET) doubles the next width up to NGX_RTC_NACK_WINDOW_MAX_MS,
 * while a quiet window resets to NGX_RTC_NACK_WINDOW_MS. Returns 1 when the
 * window rolled, so the caller also clears the dedup set via nack_reset(). */
int ngx_rtc_session_nack_window_step(ngx_rtc_session_t *sess, ngx_msec_t now);

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
 * Apply an upper-bound hint (REMB) to the pacer: lower the target to bps when
 * bps is smaller, never raise it. pacer_target_bps is the loss controller's
 * only state, so treating the viewer's estimate as an absolute target wipes
 * that state on every report -- and the estimate itself is derived from what
 * the viewer received, which the pacer's own drops reduce. The two then drive
 * each other down to the floor. bps is clamped to
 * [NGX_RTC_PACER_MIN_BPS, NGX_RTC_PACER_MAX_BPS], and a lowered target shrinks
 * the token bucket with it so a surplus from the higher rate cannot burst
 * through.
 */
void ngx_rtc_session_pacer_cap(ngx_rtc_session_t *sess, uint64_t bps);

#endif /* NGX_RTC_CORE_H */
