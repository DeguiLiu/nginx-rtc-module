/*
 * ngx_rtc_hsm.c - Hierarchical State Machine (HSM) engine.
 *
 * Pure C11 implementation; depends only on ngx_rtc_hsm.h and the C standard
 * library. Ported from the lightweight HSM framework by Andreas Misje.
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

#include "ngx_rtc_hsm.h"

/* --- Private helper declarations --- */

static uint8_t hsm_get_state_depth(const ngx_rtc_hsm_state_t *state);
static const ngx_rtc_hsm_state_t *hsm_find_lca(const ngx_rtc_hsm_state_t *s1,
                                               const ngx_rtc_hsm_state_t *s2);
static bool hsm_perform_transition(ngx_rtc_hsm_t *sm,
                                   const ngx_rtc_hsm_state_t *target_state,
                                   const ngx_rtc_hsm_event_t *event);
static const ngx_rtc_hsm_transition_t *hsm_find_matching_transition(
                                   const ngx_rtc_hsm_state_t *state,
                                   const ngx_rtc_hsm_event_t *event,
                                   bool *guard_passed,
                                   ngx_rtc_hsm_t *sm);
static bool hsm_execute_transition(ngx_rtc_hsm_t *sm,
                                   const ngx_rtc_hsm_transition_t *transition,
                                   const ngx_rtc_hsm_event_t *event);
static bool hsm_process_state_transitions(ngx_rtc_hsm_t *sm,
                                          const ngx_rtc_hsm_state_t *state,
                                          const ngx_rtc_hsm_event_t *event);
static void hsm_execute_exit_actions(ngx_rtc_hsm_t *sm,
                                     const ngx_rtc_hsm_state_t *source_state,
                                     const ngx_rtc_hsm_state_t *lca,
                                     const ngx_rtc_hsm_event_t *event);
static bool hsm_build_entry_path(ngx_rtc_hsm_t *sm,
                                 const ngx_rtc_hsm_state_t *target_state,
                                 const ngx_rtc_hsm_state_t *lca);
static void hsm_execute_entry_actions(ngx_rtc_hsm_t *sm,
                                      uint8_t path_length,
                                      const ngx_rtc_hsm_event_t *event);

/* --- Public API implementation --- */

void ngx_rtc_hsm_init(ngx_rtc_hsm_t *sm,
                      const ngx_rtc_hsm_state_t *initial_state,
                      const ngx_rtc_hsm_state_t **entry_path_buffer,
                      uint8_t buffer_size,
                      void *user_data,
                      ngx_rtc_hsm_action_fn unhandled_hook)
{
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != initial_state) &&
        (NULL != entry_path_buffer) && (0U < buffer_size))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        /* Assign user data and hooks first: they may be used by entry actions. */
        sm->user_data = user_data;
        sm->unhandled_event_hook = unhandled_hook;
        sm->initial_state = initial_state;
        sm->entry_path_buffer = entry_path_buffer;
        sm->buffer_size = buffer_size;
        sm->current_state = NULL;

        /* The initial transition has no source, so nothing can fail here: the
         * LCA is the initial state itself and the entry path is empty (matching
         * the reference implementation, the initial state's entry action does
         * not run). Keep the result ignored but explicit. */
        (void) hsm_perform_transition(sm, initial_state, NULL);
    }
}

void ngx_rtc_hsm_deinit(ngx_rtc_hsm_t *sm)
{
    if (NULL != sm)
    {
        sm->current_state = NULL;
        sm->initial_state = NULL;
        sm->user_data = NULL;
        sm->unhandled_event_hook = NULL;
        sm->entry_path_buffer = NULL;
        sm->buffer_size = 0U;
    }
}

void ngx_rtc_hsm_reset(ngx_rtc_hsm_t *sm)
{
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != sm->initial_state))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        (void) hsm_perform_transition(sm, sm->initial_state, NULL);
    }
}

bool ngx_rtc_hsm_dispatch(ngx_rtc_hsm_t *sm, const ngx_rtc_hsm_event_t *event)
{
    bool is_handled = false;
    const ngx_rtc_hsm_state_t *state_iter = NULL;
    bool continue_processing = true;
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != event))
    {
        valid_parameters = true;
        state_iter = sm->current_state;
    }

    if (true == valid_parameters)
    {
        /* Offer the event to the current state, then bubble up to parents. */
        while ((NULL != state_iter) && (true == continue_processing))
        {
            if (true == hsm_process_state_transitions(sm, state_iter, event))
            {
                is_handled = true;
                continue_processing = false;
            }
            else
            {
                state_iter = state_iter->parent;
            }
        }

        if ((false == is_handled) && (NULL != sm->unhandled_event_hook))
        {
            sm->unhandled_event_hook(sm, event);
        }
    }

    return is_handled;
}

bool ngx_rtc_hsm_is_in_state(const ngx_rtc_hsm_t *sm,
                             const ngx_rtc_hsm_state_t *state)
{
    bool is_in_state = false;
    const ngx_rtc_hsm_state_t *current_iter = NULL;
    bool continue_search = true;
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != state))
    {
        valid_parameters = true;
        current_iter = sm->current_state;
    }

    if (true == valid_parameters)
    {
        while ((NULL != current_iter) && (true == continue_search))
        {
            if (current_iter == state)
            {
                is_in_state = true;
                continue_search = false;
            }
            else
            {
                current_iter = current_iter->parent;
            }
        }
    }

    return is_in_state;
}

const char *ngx_rtc_hsm_get_current_state_name(const ngx_rtc_hsm_t *sm)
{
    const char *name = "Unknown";

    if ((NULL != sm) && (NULL != sm->current_state) &&
        (NULL != sm->current_state->name))
    {
        name = sm->current_state->name;
    }

    return name;
}

bool ngx_rtc_hsm_restore_state(ngx_rtc_hsm_t *sm,
                               const ngx_rtc_hsm_state_t *state)
{
    if ((NULL == sm) || (NULL == state))
    {
        return false;
    }

    /* Assign directly rather than dispatching the events that lead here. This
     * machine never saw those steps, so their entry/exit actions must not run;
     * and the table may have no path to `state` at all, which is exactly the
     * mirror-worker case (nothing leads from the initial state to SRTP_READY,
     * because only the owning worker performs a DTLS handshake). */
    sm->current_state = state;

    return true;
}

/* --- Private helper implementations --- */

/*
 * Perform the state transition logic: exit the source path up to the LCA,
 * then enter the target path from the LCA down to the target.
 *
 * The entry path is recorded BEFORE any exit action runs. That buffer is
 * caller-sized, so it can be too small for the hierarchy; computing it first
 * makes that failure side-effect free -- no action has run and current_state is
 * untouched, so the machine is still exactly where it was. Running the exits
 * first (which this did) left the machine exited-but-not-entered, with
 * current_state still naming the state it had just left.
 *
 * Returns false when the transition could not be performed. The caller is
 * expected to report that; it is a build-time geometry error (a state deeper
 * than the caller's buffer_size), not a recoverable runtime condition.
 */
static bool hsm_perform_transition(ngx_rtc_hsm_t *sm,
                                   const ngx_rtc_hsm_state_t *target_state,
                                   const ngx_rtc_hsm_event_t *event)
{
    const ngx_rtc_hsm_state_t *source_state = NULL;
    const ngx_rtc_hsm_state_t *lca = NULL;
    const ngx_rtc_hsm_state_t *entry_iter = NULL;
    uint8_t path_length = 0U;

    if ((NULL == sm) || (NULL == target_state))
    {
        return false;
    }

    source_state = sm->current_state;

    if (source_state == target_state)
    {
        /* External self-transition: exit then re-enter the same state. */
        if ((NULL != source_state) && (NULL != source_state->exit_action))
        {
            source_state->exit_action(sm, event);
        }
        if (NULL != target_state->entry_action)
        {
            target_state->entry_action(sm, event);
        }

        return true;
    }

    lca = hsm_find_lca(source_state, target_state);

    if (false == hsm_build_entry_path(sm, target_state, lca))
    {
        return false;
    }

    entry_iter = target_state;
    while ((NULL != entry_iter) && (entry_iter != lca))
    {
        path_length++;
        entry_iter = entry_iter->parent;
    }

    hsm_execute_exit_actions(sm, source_state, lca, event);

    sm->current_state = target_state;

    hsm_execute_entry_actions(sm, path_length, event);

    return true;
}

static uint8_t hsm_get_state_depth(const ngx_rtc_hsm_state_t *state)
{
    uint8_t depth = 0U;
    const ngx_rtc_hsm_state_t *current_state = state;

    while (NULL != current_state)
    {
        depth++;
        current_state = current_state->parent;
    }

    return depth;
}

/*
 * Find the lowest common ancestor (LCA) of two states. If one side is NULL
 * (initial transition), the other state is returned; matching the reference
 * implementation this means the initial state's entry action is not invoked.
 */
static const ngx_rtc_hsm_state_t *hsm_find_lca(const ngx_rtc_hsm_state_t *s1,
                                               const ngx_rtc_hsm_state_t *s2)
{
    const ngx_rtc_hsm_state_t *result = NULL;
    const ngx_rtc_hsm_state_t *p1 = NULL;
    const ngx_rtc_hsm_state_t *p2 = NULL;
    uint8_t depth1 = 0U;
    uint8_t depth2 = 0U;
    bool valid_parameters = false;

    if ((NULL != s1) && (NULL != s2))
    {
        valid_parameters = true;
        p1 = s1;
        p2 = s2;
        depth1 = hsm_get_state_depth(p1);
        depth2 = hsm_get_state_depth(p2);
    }
    else if (NULL == s1)
    {
        result = s2;
    }
    else if (NULL == s2)
    {
        result = s1;
    }
    else
    {
        result = NULL;
    }

    if (true == valid_parameters)
    {
        while (depth1 > depth2)
        {
            p1 = p1->parent;
            depth1--;
        }
        while (depth2 > depth1)
        {
            p2 = p2->parent;
            depth2--;
        }

        while (p1 != p2)
        {
            p1 = p1->parent;
            p2 = p2->parent;
        }
        result = p1;
    }

    return result;
}

/*
 * Find the first transition whose event matches and whose guard passes.
 */
static const ngx_rtc_hsm_transition_t *hsm_find_matching_transition(
                                   const ngx_rtc_hsm_state_t *state,
                                   const ngx_rtc_hsm_event_t *event,
                                   bool *guard_passed,
                                   ngx_rtc_hsm_t *sm)
{
    const ngx_rtc_hsm_transition_t *result = NULL;
    size_t i = 0U;
    bool found = false;
    bool valid_parameters = false;

    *guard_passed = false;

    if ((NULL != state) && (NULL != state->transitions) &&
        (NULL != event) && (0U < state->num_transitions))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        while ((i < state->num_transitions) && (false == found))
        {
            const ngx_rtc_hsm_transition_t *current_transition =
                &state->transitions[i];

            if (current_transition->event_id == event->id)
            {
                bool current_guard_passed = false;

                if (NULL == current_transition->guard)
                {
                    current_guard_passed = true;
                }
                else
                {
                    current_guard_passed = current_transition->guard(sm, event);
                }

                if (true == current_guard_passed)
                {
                    result = current_transition;
                    *guard_passed = true;
                    found = true;
                }
            }
            i++;
        }
    }

    return result;
}

/*
 * Execute a matched transition: run its action, then change state for
 * external transitions. Returns false when an external transition could not be
 * performed (the entry path buffer is too small), so the event is reported as
 * unhandled instead of appearing to have been consumed. Note the action has
 * already run at that point: it sits between the exit and entry actions per UML
 * ordering, and the only failure mode is a fixed geometry error, not data.
 */
static bool hsm_execute_transition(ngx_rtc_hsm_t *sm,
                                   const ngx_rtc_hsm_transition_t *transition,
                                   const ngx_rtc_hsm_event_t *event)
{
    bool executed = false;
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != transition))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        if (NULL != transition->action)
        {
            transition->action(sm, event);
        }

        if (NGX_RTC_HSM_TRANSITION_INTERNAL == transition->type)
        {
            /* Internal: the action is the whole transition. */
            executed = true;
        }
        else
        {
            executed = hsm_perform_transition(sm, transition->target, event);
        }
    }

    return executed;
}

static bool hsm_process_state_transitions(ngx_rtc_hsm_t *sm,
                                          const ngx_rtc_hsm_state_t *state,
                                          const ngx_rtc_hsm_event_t *event)
{
    bool handled = false;
    const ngx_rtc_hsm_transition_t *matching_transition = NULL;
    bool guard_passed = false;
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != state) && (NULL != event))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        matching_transition =
            hsm_find_matching_transition(state, event, &guard_passed, sm);

        if ((NULL != matching_transition) && (true == guard_passed))
        {
            if (true == hsm_execute_transition(sm, matching_transition, event))
            {
                handled = true;
            }
        }
    }

    return handled;
}

/*
 * Execute exit actions from the source state up to (excluding) the LCA.
 */
static void hsm_execute_exit_actions(ngx_rtc_hsm_t *sm,
                                     const ngx_rtc_hsm_state_t *source_state,
                                     const ngx_rtc_hsm_state_t *lca,
                                     const ngx_rtc_hsm_event_t *event)
{
    const ngx_rtc_hsm_state_t *exit_iter = source_state;

    while ((NULL != exit_iter) && (exit_iter != lca))
    {
        if (NULL != exit_iter->exit_action)
        {
            exit_iter->exit_action(sm, event);
        }
        exit_iter = exit_iter->parent;
    }
}

/*
 * Record the entry path from the target state up to (excluding) the LCA into
 * the caller-provided buffer. Returns false if the buffer is too small.
 */
static bool hsm_build_entry_path(ngx_rtc_hsm_t *sm,
                                 const ngx_rtc_hsm_state_t *target_state,
                                 const ngx_rtc_hsm_state_t *lca)
{
    bool success = true;
    uint8_t path_idx = 0U;
    const ngx_rtc_hsm_state_t *entry_iter = target_state;
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != target_state))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        while ((NULL != entry_iter) && (entry_iter != lca) && (true == success))
        {
            if (path_idx < sm->buffer_size)
            {
                sm->entry_path_buffer[path_idx] = entry_iter;
                path_idx++;
                entry_iter = entry_iter->parent;
            }
            else
            {
                success = false;
            }
        }
    }
    else
    {
        success = false;
    }

    return success;
}

/*
 * Execute entry actions in reverse order (parent to child).
 */
static void hsm_execute_entry_actions(ngx_rtc_hsm_t *sm,
                                      uint8_t path_length,
                                      const ngx_rtc_hsm_event_t *event)
{
    bool valid_parameters = false;

    if ((NULL != sm) && (NULL != sm->entry_path_buffer) && (0U < path_length))
    {
        valid_parameters = true;
    }

    if (true == valid_parameters)
    {
        /*
         * Count down an unsigned: the original `(int8_t) path_length - 1` went
         * negative for a hierarchy deeper than 127 and skipped every entry
         * action without a word. path_length is bounded by buffer_size (uint8_t),
         * so the only cost is walking the parents instead of being wrong.
         */
        while (0U < path_length)
        {
            path_length--;

            if ((NULL != sm->entry_path_buffer[path_length]) &&
                (NULL != sm->entry_path_buffer[path_length]->entry_action))
            {
                sm->entry_path_buffer[path_length]->entry_action(sm, event);
            }
        }
    }
}
