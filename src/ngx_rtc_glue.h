/**
 * @file    ngx_rtc_glue.h
 * @brief   Cross-module glue entry points.
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * The periodic RTCP sender-report timer lives in the rtmp bridge module, but its
 * starter is reached through here so the http module can call it from its own
 * init_process. Both handlers do run - nginx walks cycle->modules[] with no
 * module-type filter - and the duplicate call only re-arms the same event.
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
