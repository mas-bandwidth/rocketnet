/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_frame_sync.h"

#include <string.h>
#include <errno.h>
#include <math.h>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

#include <sched.h>
#include <time.h>

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

#define ROCKETNET_NANOS_PER_SECOND 1000000000ULL

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif // #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

// -------------------------------------------------------------------
// the platform clock and the absolute sleep it pairs with
// -------------------------------------------------------------------

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

uint64_t rocketnet_frame_sync_now_ns()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );

    return (uint64_t) ts.tv_sec * ROCKETNET_NANOS_PER_SECOND + (uint64_t) ts.tv_nsec;
}

static void rocketnet_frame_sync_platform_wait_until( struct rocketnet_frame_sync_t * frame_sync, uint64_t deadline_ns )
{
    (void) frame_sync;

    struct timespec ts;
#if defined( __OpenBSD__ )
    // openbsd has no clock_nanosleep. sleep the remaining interval relative
    // to a fresh read of the same clock; the spin guard above this call
    // already absorbs an early wake, and a late one is the scheduler's to
    // give on every platform.

    uint64_t now_ns = rocketnet_frame_sync_now_ns();
    if ( now_ns >= deadline_ns )
        return;

    uint64_t remaining_ns = deadline_ns - now_ns;
    ts.tv_sec = (time_t) ( remaining_ns / ROCKETNET_NANOS_PER_SECOND );
    ts.tv_nsec = (long) ( remaining_ns % ROCKETNET_NANOS_PER_SECOND );

    nanosleep( &ts, NULL );
#else
    ts.tv_sec = (time_t) ( deadline_ns / ROCKETNET_NANOS_PER_SECOND );
    ts.tv_nsec = (long) ( deadline_ns % ROCKETNET_NANOS_PER_SECOND );

    clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL );
#endif // #if defined( __OpenBSD__ )
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

static const mach_timebase_info_data_t & rocketnet_frame_sync_timebase()
{
    // the timebase is read once, on first use by any caller (a function-local
    // static, initialized once even under concurrent first calls), so
    // rocketnet_frame_sync_now_ns called before any create never divides by an
    // unread zero denominator.

    static const mach_timebase_info_data_t timebase = []
    {
        mach_timebase_info_data_t value;
        mach_timebase_info( &value );

        return value;
    }();

    return timebase;
}

#if ROCKETNET_DEV
int rocketnet_frame_sync_timebase_ready_for_testing()
{
    // this metadata-only probe verifies the scale is available before frame-sync
    // creation, without reading the live clock.

    const mach_timebase_info_data_t & timebase = rocketnet_frame_sync_timebase();
    return timebase.numer != 0 && timebase.denom != 0;
}
#endif // #if ROCKETNET_DEV

static void rocketnet_frame_sync_platform_init()
{
    rocketnet_frame_sync_timebase();
}

uint64_t rocketnet_frame_sync_now_ns()
{
    const mach_timebase_info_data_t & timebase = rocketnet_frame_sync_timebase();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

static void rocketnet_frame_sync_platform_wait_until( struct rocketnet_frame_sync_t * frame_sync, uint64_t deadline_ns )
{
    (void) frame_sync;

    const mach_timebase_info_data_t & timebase = rocketnet_frame_sync_timebase();
    mach_wait_until( deadline_ns * timebase.denom / timebase.numer );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static HANDLE rocketnet_frame_sync_create_waitable_timer( unsigned flags )
{
#if ROCKETNET_DEV
    return (HANDLE) rocketnet_platform_priority_calls_current()->create_waitable_timer( flags );
#else
    return CreateWaitableTimerExW( NULL, NULL, flags, TIMER_ALL_ACCESS );
#endif // #if ROCKETNET_DEV
}

static double rocketnet_frame_sync_qpc_frequency()
{
    // the counter frequency is read once, on first use by any caller (a
    // function-local static, initialized once even under concurrent first
    // calls), so rocketnet_frame_sync_now_ns called before any create never
    // divides by an unread zero frequency.

    static const double frequency = []
    {
        LARGE_INTEGER value;
        QueryPerformanceFrequency( &value );

        return (double) value.QuadPart;
    }();

    return frequency;
}

static void rocketnet_frame_sync_platform_init()
{
    rocketnet_frame_sync_qpc_frequency();
}

uint64_t rocketnet_frame_sync_now_ns()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter( &counter );

    return (uint64_t) ( (double) counter.QuadPart * ( 1e9 / rocketnet_frame_sync_qpc_frequency() ) );
}

static void rocketnet_frame_sync_platform_wait_until( struct rocketnet_frame_sync_t * frame_sync, uint64_t deadline_ns )
{
    const uint64_t t = rocketnet_frame_sync_now_ns();
    if ( t >= deadline_ns )
        return;

    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG) ( ( deadline_ns - t ) / 100 );                       // relative, 100ns units
    SetWaitableTimer( frame_sync->waitable_timer, &due, 0, NULL, NULL, FALSE );
    WaitForSingleObject( frame_sync->waitable_timer, INFINITE );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

static void rocketnet_frame_sync_sleep_until( struct rocketnet_frame_sync_t * frame_sync, uint64_t deadline_ns )
{
    // the sleep never returns ahead of the target: signals and timer granularity
    // wake it early, and the loop simply goes back down.

    while ( rocketnet_frame_sync_now_ns() < deadline_ns )
    {
        rocketnet_frame_sync_platform_wait_until( frame_sync, deadline_ns );
    }
}

// -------------------------------------------------------------------
// priority on the waiting thread, refusal reported and degraded, never faked
// -------------------------------------------------------------------

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

static void rocketnet_frame_sync_elevate_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    // off (rocketnet_thread_priority( false )): no priority call, and
    // without priority held the wait is plain absolute sleep

    if ( !rocketnet_platform_thread_priority_enabled() )
        return;

    frame_sync->waiter_thread = pthread_self();
    pthread_getschedparam( pthread_self(), &frame_sync->waiter_previous_policy, &frame_sync->waiter_previous_param );

    struct sched_param param;
    memset( &param, 0, sizeof( param ) );
    param.sched_priority = 80;

    const int error = pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );
    if ( error != 0 )
    {
        rocketnet_info( "frame sync: SCHED_FIFO refused (%s), plain absolute sleep", strerror( error ) );
        return;
    }

    frame_sync->priority_held = 1;
    rocketnet_info( "frame sync: SCHED_FIFO 80 on the waiting thread, spin guard enabled" );
}

static void rocketnet_frame_sync_restore_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    if ( !frame_sync->priority_held || !pthread_equal( pthread_self(), frame_sync->waiter_thread ) )
        return;

    pthread_setschedparam( pthread_self(), frame_sync->waiter_previous_policy, &frame_sync->waiter_previous_param );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS

static void rocketnet_frame_sync_elevate_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    // off (rocketnet_thread_priority( false )): no priority call, and
    // without priority held the wait is plain absolute sleep

    if ( !rocketnet_platform_thread_priority_enabled() )
        return;

    // the real-time thread contract pro audio uses: period, computation and
    // constraint in mach time units. needs no privileges.

    frame_sync->waiter_thread = pthread_self();

    // a period past the uint32_t policy fields (a very low frame rate) is
    // refused rather than truncated into a wrong contract

    const mach_timebase_info_data_t & timebase = rocketnet_frame_sync_timebase();
    thread_time_constraint_policy_data_t policy;

    if ( !rocketnet_frame_sync_time_constraint( frame_sync->period_ns, timebase.numer, timebase.denom, &policy.period, &policy.computation, &policy.constraint ) )
    {
        rocketnet_info( "frame sync: time constraint policy skipped, period out of range, plain absolute sleep" );
        return;
    }

    policy.preemptible = TRUE;

    // mach_thread_self returns a new send right on each call; it is released
    // after the policy call so no port leaks per call

    const mach_port_t thread = mach_thread_self();
    kern_return_t result = thread_policy_set( thread, THREAD_TIME_CONSTRAINT_POLICY,
                                              (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT );
    mach_port_deallocate( mach_task_self(), thread );

    if ( result != KERN_SUCCESS )
    {
        rocketnet_info( "frame sync: time constraint policy refused (%d), plain absolute sleep", (int) result );
        return;
    }

    frame_sync->priority_held = 1;
    rocketnet_info( "frame sync: time constraint policy on the waiting thread, spin guard enabled" );
}

static void rocketnet_frame_sync_restore_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    if ( !frame_sync->priority_held || !pthread_equal( pthread_self(), frame_sync->waiter_thread ) )
        return;

    thread_standard_policy_data_t policy;
    const mach_port_t thread = mach_thread_self();
    thread_policy_set( thread, THREAD_STANDARD_POLICY,
                       (thread_policy_t) &policy, THREAD_STANDARD_POLICY_COUNT );

    mach_port_deallocate( mach_task_self(), thread );
}

#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static void rocketnet_frame_sync_elevate_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    // off (rocketnet_thread_priority( false )): no priority call, and
    // without priority held the wait is plain absolute sleep

    if ( !rocketnet_platform_thread_priority_enabled() )
        return;

    frame_sync->waiter_thread_id = GetCurrentThreadId();
    frame_sync->waiter_previous_priority = GetThreadPriority( GetCurrentThread() );

    if ( !SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL ) )
    {
        rocketnet_info( "frame sync: TIME_CRITICAL refused (%lu), plain absolute sleep", (unsigned long) GetLastError() );
        return;
    }

    frame_sync->priority_held = 1;
    rocketnet_info( "frame sync: TIME_CRITICAL on the waiting thread, spin guard enabled" );
}

static void rocketnet_frame_sync_restore_priority( struct rocketnet_frame_sync_t * frame_sync )
{
    if ( !frame_sync->priority_held || GetCurrentThreadId() != frame_sync->waiter_thread_id )
        return;

    SetThreadPriority( GetCurrentThread(), frame_sync->waiter_previous_priority );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_BSD

#if ROCKETNET_DEV
int rocketnet_frame_sync_elevate_priority_for_testing( uint64_t period_ns )
{
    // for development unit tests only: the
    // priority step rocketnet_frame_sync_wait takes on its first wait, run on a
    // frame sync that is never created (no timer, no thread), then given back
    // through rocketnet_frame_sync_restore_priority. Returns priority_held.

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_BSD
    rocketnet_frame_sync_platform_init();
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_BSD

    struct rocketnet_frame_sync_t frame_sync;
    memset( &frame_sync, 0, sizeof( frame_sync ) );
    frame_sync.period_ns = period_ns;

    rocketnet_frame_sync_elevate_priority( &frame_sync );
    rocketnet_frame_sync_restore_priority( &frame_sync );

    return frame_sync.priority_held;
}
#endif // #if ROCKETNET_DEV

// -------------------------------------------------------------------
// the pure parts
// -------------------------------------------------------------------

uint64_t rocketnet_frame_sync_guard_ns( double guard_ewma_ns, uint64_t guard_cap_ns )
{
    rocketnet_assert( guard_ewma_ns >= 0.0 );

    uint64_t guard = (uint64_t) guard_ewma_ns + ROCKETNET_FRAME_SYNC_GUARD_MARGIN_NS;
    if ( guard > guard_cap_ns )
    {
        guard = guard_cap_ns;
    }

    return guard;
}

void rocketnet_frame_sync_adapt_guard( double * guard_ewma_ns, uint64_t oversleep_ns, uint64_t guard_cap_ns )
{
    rocketnet_assert( guard_ewma_ns );

    double next = 0.9 * ( *guard_ewma_ns ) + 0.1 * (double) oversleep_ns;
    if ( next > (double) guard_cap_ns )
    {
        next = (double) guard_cap_ns;
    }

    *guard_ewma_ns = next;
}

uint64_t rocketnet_frame_sync_advance( uint64_t * deadline_ns, uint64_t period_ns, uint64_t woke_ns, uint64_t * error_ns )
{
    rocketnet_assert( deadline_ns );
    rocketnet_assert( error_ns );
    rocketnet_assert( period_ns > 0 );

    const uint64_t error = woke_ns > *deadline_ns ? woke_ns - *deadline_ns : 0;
    *error_ns = error;

    uint64_t drops = 0;
    if ( error > period_ns )
    {
        drops = error / period_ns;
        *deadline_ns += ( ( error / period_ns ) + 1 ) * period_ns;
    }
    else
    {
        *deadline_ns += period_ns;
    }

    return drops;
}

int rocketnet_frame_sync_time_constraint( uint64_t period_ns, uint32_t numer, uint32_t denom, uint32_t * period, uint32_t * computation, uint32_t * constraint )
{
    rocketnet_assert( numer > 0 );
    rocketnet_assert( period );
    rocketnet_assert( computation );
    rocketnet_assert( constraint );

    // range rule: period_ns * denom must fit uint64_t and the period in mach
    // units must fit the uint32_t policy fields, else nothing is written

    if ( denom != 0 && period_ns > UINT64_MAX / denom )
        return 0;

    const uint64_t period_mach = period_ns * denom / numer;
    if ( period_mach > UINT32_MAX )
        return 0;

    *period = (uint32_t) period_mach;
    *computation = (uint32_t) ( period_mach / 4 );
    *constraint = (uint32_t) ( period_mach / 2 );

    return 1;
}

// -------------------------------------------------------------------
// the public surface
// -------------------------------------------------------------------

struct rocketnet_frame_sync_t * rocketnet_frame_sync_create( void * context, double frame_rate )
{
    return rocketnet_frame_sync_create_ex( context, frame_rate, NULL );
}

struct rocketnet_frame_sync_t * rocketnet_frame_sync_create_ex( void * context, double frame_rate, int * status )
{
    // the lifecycle state is read once here, before any allocation; a create
    // before rocketnet_init is refused, logged and counted

    if ( !rocketnet_library_initialized() )
    {
        if ( status )
            *status = ROCKETNET_STATUS_NOT_INITIALIZED;

        rocketnet_error( "rocketnet_frame_sync_create: rocketnet_init must be called first" );
        rocketnet_api_misuse_record();
        return NULL;
    }

    // docs/ERRORS-AND-LOGGING.md 1.4 and 2.1: F1 invalid frame rate is
    // INVALID_ARGUMENT, F2 the malloc refusal is OUT_OF_MEMORY, and F3 the
    // Windows timer refusal is TIMER_CREATE_FAILED.

    if ( !( frame_rate > 0.0 ) || frame_rate > ROCKETNET_FRAME_SYNC_MAX_FRAME_RATE )
    {
        if ( status )
            *status = ROCKETNET_STATUS_INVALID_ARGUMENT;

        rocketnet_error( "frame sync: invalid frame rate %.1f", frame_rate );
        return NULL;
    }

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_BSD
    rocketnet_frame_sync_platform_init();
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_LINUX && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_BSD

    struct rocketnet_frame_sync_t * frame_sync = (struct rocketnet_frame_sync_t *) rocketnet_malloc( context, sizeof( struct rocketnet_frame_sync_t ) );
    if ( !frame_sync )
    {
        if ( status )
            *status = ROCKETNET_STATUS_OUT_OF_MEMORY;

        rocketnet_error( "frame sync: could not allocate frame sync" );
        return NULL;
    }

    memset( frame_sync, 0, sizeof( struct rocketnet_frame_sync_t ) );

    frame_sync->context = context;
    frame_sync->frame_rate = frame_rate;
    frame_sync->period_ns = (uint64_t) ( 1000000000.0 / frame_rate );
    frame_sync->guard_cap_ns = frame_sync->period_ns / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR;
    frame_sync->guard_ewma_ns = (double) ROCKETNET_FRAME_SYNC_GUARD_EWMA_START_NS;
    if ( frame_sync->guard_ewma_ns > (double) frame_sync->guard_cap_ns )
    {
        frame_sync->guard_ewma_ns = (double) frame_sync->guard_cap_ns;
    }

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    // Windows frame pacing requires a high-resolution waitable timer
    // (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION); it never retries or falls back
    // to a coarse standard waitable timer (flags == 0).

    frame_sync->waitable_timer = rocketnet_frame_sync_create_waitable_timer( CREATE_WAITABLE_TIMER_HIGH_RESOLUTION );
    if ( !frame_sync->waitable_timer )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TIMER_CREATE_FAILED;

        rocketnet_error( "frame sync: could not create high resolution waitable timer (%lu)", (unsigned long) GetLastError() );
        rocketnet_free( context, frame_sync );
        return NULL;
    }

#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    frame_sync->lifecycle_timer_generation = rocketnet_lifecycle_track_open( ROCKETNET_LIFECYCLE_TIMER,
                                                                             (uintptr_t) frame_sync->waitable_timer, "frame sync waitable timer" );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#elif ROCKETNET_DEV
    if ( !rocketnet_platform_priority_calls_current()->create_waitable_timer( CREATE_WAITABLE_TIMER_HIGH_RESOLUTION ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_TIMER_CREATE_FAILED;

        rocketnet_error( "frame sync: could not create high resolution waitable timer" );
        rocketnet_free( context, frame_sync );
        return NULL;
    }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    rocketnet_info( "frame sync: %.1f hz", frame_rate );

    if ( status )
        *status = ROCKETNET_STATUS_OK;

    return frame_sync;
}

void rocketnet_frame_sync_destroy( struct rocketnet_frame_sync_t * frame_sync )
{
    if ( !rocketnet_api_null_guard( frame_sync ) )
        return;
    rocketnet_assert( frame_sync );

    if ( frame_sync->first_wait_done )
    {
        rocketnet_frame_sync_restore_priority( frame_sync );
    }

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( !rocketnet_lifecycle_track_close_begin( ROCKETNET_LIFECYCLE_TIMER, (uintptr_t) frame_sync->waitable_timer,
                                                 frame_sync->lifecycle_timer_generation ) )
        return;

    const BOOL closed = CloseHandle( frame_sync->waitable_timer );
    rocketnet_lifecycle_track_close_end( frame_sync->lifecycle_timer_generation, closed != 0 );
#else
    CloseHandle( frame_sync->waitable_timer );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    rocketnet_free( frame_sync->context, frame_sync );
}

void rocketnet_frame_sync_wait( struct rocketnet_frame_sync_t * frame_sync )
{
    if ( !rocketnet_api_null_guard( frame_sync ) )
        return;
    rocketnet_assert( frame_sync );

    const uint64_t entry_ns = rocketnet_frame_sync_now_ns();

    if ( !frame_sync->first_wait_done )
    {
        // the waiting thread is by construction the loop thread, so priority
        // is taken here rather than at create, and the deadline anchors here
        // so the gap between create and the loop never reads as lateness.

        rocketnet_frame_sync_elevate_priority( frame_sync );
        frame_sync->spinning = frame_sync->priority_held;
        frame_sync->deadline_ns = entry_ns + frame_sync->period_ns;
        frame_sync->first_wait_done = 1;
    }

    if ( frame_sync->spinning )
    {
        const uint64_t guard = rocketnet_frame_sync_guard_ns( frame_sync->guard_ewma_ns, frame_sync->guard_cap_ns );
        const uint64_t sleep_until = frame_sync->deadline_ns > guard ? frame_sync->deadline_ns - guard : 0;

        rocketnet_frame_sync_sleep_until( frame_sync, sleep_until );

        const uint64_t woke = rocketnet_frame_sync_now_ns();
        if ( sleep_until > entry_ns && woke > sleep_until )
        {
            // adapt only when the sleep actually had room: a frame whose work
            // overran never slept, and its lateness is not the sleeper's.

            rocketnet_frame_sync_adapt_guard( &frame_sync->guard_ewma_ns, woke - sleep_until, frame_sync->guard_cap_ns );
        }

        while ( rocketnet_frame_sync_now_ns() < frame_sync->deadline_ns )
        {
        }
    }
    else
    {
        rocketnet_frame_sync_sleep_until( frame_sync, frame_sync->deadline_ns );
    }

    const uint64_t woke = rocketnet_frame_sync_now_ns();

    const uint64_t drops = rocketnet_frame_sync_advance( &frame_sync->deadline_ns, frame_sync->period_ns, woke, &frame_sync->wakeup_error_ns );
    if ( drops > 0 )
    {
        frame_sync->dropped_frames += drops;
        rocketnet_debug( "frame sync: dropped %" PRIu64 " frame%s (%.2fms late)", drops, drops == 1 ? "" : "s", (double) frame_sync->wakeup_error_ns / 1000000.0 );
    }

    frame_sync->frame++;
}

void rocketnet_frame_sync_stats( struct rocketnet_frame_sync_t * frame_sync, struct rocketnet_frame_sync_stats_t * stats )
{
    if ( !rocketnet_api_null_guard( frame_sync ) )
        return;
    if ( !rocketnet_api_null_guard( stats ) )
        return;
    rocketnet_assert( frame_sync );
    rocketnet_assert( stats );

    stats->frame = frame_sync->frame;
    stats->dropped_frames = frame_sync->dropped_frames;
    stats->priority_held = frame_sync->priority_held;
    stats->spinning = frame_sync->spinning;
    stats->guard_seconds = (double) rocketnet_frame_sync_guard_ns( frame_sync->guard_ewma_ns, frame_sync->guard_cap_ns ) / 1000000000.0;
    stats->wakeup_error_seconds = (double) frame_sync->wakeup_error_ns / 1000000000.0;
}
