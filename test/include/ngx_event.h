/*
 * ngx_event.h - host-test stub for the nginx event header.
 *
 * ngx_rtc_core.c includes this header but never uses an event type. Now
 * ngx_rtc_stream_module.c compiles here too, and it arms timers and posts
 * events, so the type and the two entry points have to exist.
 *
 * The host build has no event loop: ngx_add_timer() records nothing and
 * ngx_post_event() runs the handler inline. That is the honest mapping for a
 * single-threaded test -- a posted event in production runs "later", but every
 * test here inspects state immediately after the call that posted it, and an
 * inline run is what makes that observable. Anything that depended on the
 * deferral would have to fake a loop instead.
 */

#ifndef NGX_STUB_EVENT_H
#define NGX_STUB_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include <ngx_core.h>

typedef struct ngx_event_s ngx_event_t;
typedef void (*ngx_event_handler_pt)(ngx_event_t *ev);

/* Event-flag and sentinel values from nginx. The module sets ev->index to
 * NGX_INVALID_INDEX when it registers an eventfd, so the field has to exist
 * even though nothing here maintains a timer tree. Only the read flag is
 * defined: the stream module never registers a write event. */
#define NGX_INVALID_INDEX  0x80000000u
#define NGX_READ_EVENT     0x0001

struct ngx_event_s
{
    void                 *data;      /* handler context (ngx_stream_session_t *) */
    void                 *log;       /* ngx_log_t *; unused by the stub */
    ngx_event_handler_pt  handler;

    /* Timer bookkeeping. The host build never fires anything, so these are
     * only ever read back by a test that wants to see what was armed. */
    ngx_uint_t            index;
    unsigned              timer_set:1;
    unsigned              posted:1;
    uint64_t              timer_ms;  /* absolute deadline the caller asked for */
};

/* The posted-event queue is a sentinel the stream module passes to
 * ngx_post_event; the stub runs handlers inline, so nothing ever lands here. */
extern ngx_event_t  ngx_posted_events;

void ngx_add_timer(ngx_event_t *ev, uint64_t ms);
void ngx_del_timer(ngx_event_t *ev);
void ngx_post_event(ngx_event_t *ev, ngx_event_t *queue);

/* Event registration against a descriptor. No event loop exists on the host,
 * so this records nothing and always succeeds -- enough for the module's
 * per-worker notify-fd setup to run to completion. */
ngx_int_t ngx_add_event(ngx_event_t *ev, ngx_uint_t event, ngx_uint_t flags);

#endif /* NGX_STUB_EVENT_H */
