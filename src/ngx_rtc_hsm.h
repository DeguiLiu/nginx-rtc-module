/*
 * ngx_rtc_hsm.h - Hierarchical State Machine (HSM) engine.
 *
 * A data-driven hierarchical state machine. States form a parent chain, and an
 * event is first offered to the current state, then bubbles up to its parents
 * until a transition consumes it. Transitions may be EXTERNAL (exit source
 * path, enter target path) or INTERNAL (run only the action, keep the state).
 *
 * This engine is a pure C11 port of the lightweight HSM framework by Andreas
 * Misje. It depends only on the C standard library (stdint.h / stdbool.h /
 * stddef.h / assert.h); it has no nginx or RT-Thread dependency so it can be
 * unit-tested on the host.
 */

/*
 * Copyright (c) 2013 Andreas Misje
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef NGX_RTC_HSM_H
#define NGX_RTC_HSM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Configurable macros --- */

/*
 * Configurable assertion macro. Override before including this header to use a
 * project-specific assert (or an empty no-op). Defaults to <assert.h>.
 */
#ifndef NGX_RTC_HSM_ASSERT
#include <assert.h>
#define NGX_RTC_HSM_ASSERT(expr) assert(expr)
#endif

/* --- Core types --- */

typedef struct ngx_rtc_hsm ngx_rtc_hsm_t;
typedef struct ngx_rtc_hsm_state ngx_rtc_hsm_state_t;
typedef struct ngx_rtc_hsm_event ngx_rtc_hsm_event_t;
typedef struct ngx_rtc_hsm_transition ngx_rtc_hsm_transition_t;

/*
 * Event passed to the state machine. `id` is an application-specific event
 * identifier (typically an enum value); `context` optionally points at
 * event-specific data.
 */
struct ngx_rtc_hsm_event
{
    uint32_t id;
    void    *context;
};

/*
 * Transition type.
 */
typedef enum
{
    /*
     * External transition: exit the source state path and enter the target
     * state path. If source and target are the same, it is a self-transition
     * that executes exit and entry actions.
     */
    NGX_RTC_HSM_TRANSITION_EXTERNAL = 0,

    /*
     * Internal transition: execute only the action, without exit or entry
     * calls. The state does not change; the target is ignored.
     */
    NGX_RTC_HSM_TRANSITION_INTERNAL
} ngx_rtc_hsm_transition_type_t;

typedef void (*ngx_rtc_hsm_action_fn)(ngx_rtc_hsm_t *sm,
                                      const ngx_rtc_hsm_event_t *event);
typedef bool (*ngx_rtc_hsm_guard_fn)(ngx_rtc_hsm_t *sm,
                                     const ngx_rtc_hsm_event_t *event);

/*
 * A single state transition rule.
 */
struct ngx_rtc_hsm_transition
{
    uint32_t                       event_id;
    const ngx_rtc_hsm_state_t     *target;
    ngx_rtc_hsm_guard_fn           guard;
    ngx_rtc_hsm_action_fn          action;
    ngx_rtc_hsm_transition_type_t  type;
};

/*
 * A state and its behavior.
 */
struct ngx_rtc_hsm_state
{
    const ngx_rtc_hsm_state_t     *parent;
    ngx_rtc_hsm_action_fn          entry_action;
    ngx_rtc_hsm_action_fn          exit_action;
    const ngx_rtc_hsm_transition_t *transitions;
    size_t                         num_transitions;
    const char                    *name;
};

/*
 * The state machine instance.
 */
struct ngx_rtc_hsm
{
    const ngx_rtc_hsm_state_t  *current_state;
    const ngx_rtc_hsm_state_t  *initial_state;
    void                       *user_data;
    ngx_rtc_hsm_action_fn       unhandled_event_hook;
    const ngx_rtc_hsm_state_t **entry_path_buffer;
    uint8_t                     buffer_size;
};

/* --- Public API --- */

/*
 * Initialize a state machine instance.
 *
 * `entry_path_buffer` is a caller-provided scratch array sized for the maximum
 * hierarchy depth; `buffer_size` is its element count.
 */
void ngx_rtc_hsm_init(ngx_rtc_hsm_t *sm,
                      const ngx_rtc_hsm_state_t *initial_state,
                      const ngx_rtc_hsm_state_t **entry_path_buffer,
                      uint8_t buffer_size,
                      void *user_data,
                      ngx_rtc_hsm_action_fn unhandled_hook);

/*
 * Deinitialize the state machine instance, clearing internal pointers.
 */
void ngx_rtc_hsm_deinit(ngx_rtc_hsm_t *sm);

/*
 * Reset the state machine to its initial state.
 */
void ngx_rtc_hsm_reset(ngx_rtc_hsm_t *sm);

/*
 * Dispatch an event. Returns true if the event was handled.
 */
bool ngx_rtc_hsm_dispatch(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event);

/*
 * Return true if the current state is `state` or one of its descendants.
 */
bool ngx_rtc_hsm_is_in_state(const ngx_rtc_hsm_t *sm,
                             const ngx_rtc_hsm_state_t *state);

/*
 * Return the name of the current state, or "Unknown" if unavailable.
 */
const char *ngx_rtc_hsm_get_current_state_name(const ngx_rtc_hsm_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* NGX_RTC_HSM_H */
