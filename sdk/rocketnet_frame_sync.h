/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_FRAME_SYNC_H
#define ROCKETNET_FRAME_SYNC_H

#include <stdint.h>
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

// The frame sync primitive, built to the issue #56 measurements: an absolute
// deadline per frame, the platform sleep that honors it, priority taken on
// the waiting thread, and a capped adaptive spin guard covering the sleeper's
// oversleep. The guard runs only while priority is held: an unprivileged
// spinner gets preempted out of its own spin window and the adaptation then
// chases the loss -- measured to 37ms of guard and thousands of dropped
// frames -- so without priority the primitive is plain absolute sleep, and
// the stats say so.

#define ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE                     1000.0

#define ROCKETNET_FRAME_SYNC_GUARD_EWMA_START_NS                200000      // 200us, near the regime most measured boxes converge to
#define ROCKETNET_FRAME_SYNC_GUARD_MARGIN_NS                     50000      // added over the ewma so the spin usually wins the race
#define ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR                       4      // the guard never exceeds period / this

struct rocketnet_frame_sync_t
{
    void * context;
    double frame_rate;
    uint64_t period_ns;
    uint64_t guard_cap_ns;

    // the loop state, anchored at the first wait

    int first_wait_done;
    uint64_t deadline_ns;
    uint64_t frame;
    uint64_t dropped_frames;
    uint64_t wakeup_error_ns;
    double guard_ewma_ns;
    int priority_held;
    int spinning;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    HANDLE waitable_timer;
#if defined(ROCKETNET_WINDOWS_LIFECYCLE_TRACK)
    uint64_t lifecycle_timer_generation;
#endif
    DWORD waiter_thread_id;
    int waiter_previous_priority;
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    pthread_t waiter_thread;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
    int waiter_previous_policy;
    struct sched_param waiter_previous_param;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD
};

// The pure parts, tested directly.

// The guard actually applied ahead of the deadline: the oversleep estimate
// plus the margin, never past the cap.

uint64_t rocketnet_frame_sync_guard_ns( double guard_ewma_ns, uint64_t guard_cap_ns );

// Folds one observed oversleep into the estimate. The cap binds the estimate
// itself, not just the applied guard, so one pathological wake cannot wind
// it up into a long recovery.

void rocketnet_frame_sync_adapt_guard( double * guard_ewma_ns, uint64_t oversleep_ns, uint64_t guard_cap_ns );

// Advances the deadline past a wake at woke_ns, detecting dropped frames: a
// wake more than one whole period late drops error/period frames and the
// deadline resynchronizes to the next boundary ahead of the wake rather than
// chasing the backlog. Returns the drops; writes the wake's lateness.
// The drop count is uint64_t, the type of the dropped_frames it feeds, so a
// wake 2^31 or more periods late never wraps.

uint64_t rocketnet_frame_sync_advance( uint64_t * deadline_ns, uint64_t period_ns, uint64_t woke_ns, uint64_t * error_ns );

// The time constraint policy values in mach time units for a period: period,
// period / 4 of computation and period / 2 of constraint. Range rule: returns
// 0 and writes nothing when period_ns * denom would overflow uint64_t or the
// period in mach units exceeds UINT32_MAX (the policy fields are uint32_t);
// else writes the three values and returns 1. Pure, compiled everywhere.

int rocketnet_frame_sync_time_constraint( uint64_t period_ns, uint32_t numer, uint32_t denom, uint32_t * period, uint32_t * computation, uint32_t * constraint );

// The monotonic clock the waits run against, nanoseconds.

uint64_t rocketnet_frame_sync_now_ns();

#endif // #ifndef ROCKETNET_FRAME_SYNC_H
