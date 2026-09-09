/*
 * ngx_rtc_glue.h - cross-module glue entry points.
 *
 * nginx only invokes module init_process for core/http/stream module types; an
 * NGX_RTMP_MODULE's init_process is never run, so a periodic timer living in a
 * rtmp module would never fire. Entry points here are called from a module
 * whose init_process nginx does run (the http module).
 */

#ifndef NGX_RTC_GLUE_H
#define NGX_RTC_GLUE_H

#include <ngx_config.h>
#include <ngx_core.h>

/* Start the per-worker RTCP sender-report / observability timer (defined in
 * ngx_rtmp_rtc_bridge_module.c). Safe to call more than once: ngx_add_timer on
 * an already-scheduled event reschedules it. */
void ngx_rtmp_rtc_rtcp_timer_start(ngx_cycle_t *cycle);

#endif /* NGX_RTC_GLUE_H */
