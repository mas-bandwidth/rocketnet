/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The frame sync prototype: measures the candidate mechanisms for pegging
    a headless server loop to real time, per issue #56 -- linux and macos.
    An experiment harness serving that investigation; it contracts when
    rocketnet_frame_sync lands in the library.

    ./bin/frame_sync_prototype              quiet measurement
    ./bin/frame_sync_prototype load         with busy threads contending

    Per mechanism, at 60Hz: wakeup error vs the absolute deadline (avg, p50,
    p99, p999, max), dropped frames, and thread CPU as a percent of wall --
    the cost of any spinning. Mechanisms needing privileges report the
    refusal and are skipped, never faked.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <unistd.h>
#endif

#define FRAME_RATE_HZ           60.0
#define WARMUP_FRAMES           120
#define MEASURE_FRAMES          1800
#define NANOS_PER_SECOND        1000000000ULL

// ---- the platform clock and sleep-to-absolute-deadline ----

#if defined(__linux__)

#define _GNU_SOURCE_WAS_HERE 1
#include <sched.h>
#include <sys/syscall.h>

static uint64_t now_ns( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t) ts.tv_sec * NANOS_PER_SECOND + (uint64_t) ts.tv_nsec;
}

static void sleep_until_ns( uint64_t deadline )
{
    struct timespec ts = { (time_t) ( deadline / NANOS_PER_SECOND ), (long) ( deadline % NANOS_PER_SECOND ) };
    clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL );
}

#define ABSOLUTE_SLEEP_NAME "clock_nanosleep ABSTIME"

#elif defined(__APPLE__)

#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>

static mach_timebase_info_data_t timebase;

static uint64_t now_ns( void )
{
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

static void sleep_until_ns( uint64_t deadline )
{
    mach_wait_until( deadline * timebase.denom / timebase.numer );
}

#define ABSOLUTE_SLEEP_NAME "mach_wait_until"

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER qpc_frequency;
static HANDLE waitable_timer;

static uint64_t now_ns( void )
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter( &counter );
    return (uint64_t) ( (double) counter.QuadPart * ( 1e9 / (double) qpc_frequency.QuadPart ) );
}

static void sleep_until_ns( uint64_t deadline )
{
    const uint64_t t = now_ns();
    if ( t >= deadline )
        return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG) ( ( deadline - t ) / 100 );      // relative, 100ns units
    SetWaitableTimer( waitable_timer, &due, 0, NULL, NULL, FALSE );
    WaitForSingleObject( waitable_timer, INFINITE );
}

#define ABSOLUTE_SLEEP_NAME "waitable timer highres"

#else

static uint64_t now_ns( void ) { return 0; }
static void sleep_until_ns( uint64_t deadline ) { (void) deadline; }
#define ABSOLUTE_SLEEP_NAME "unsupported"

#endif

static uint64_t thread_cpu_ns( void )
{
#if defined(_WIN32)
    FILETIME creation, exit_time, kernel, user;
    GetThreadTimes( GetCurrentThread(), &creation, &exit_time, &kernel, &user );
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;  k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;    u.HighPart = user.dwHighDateTime;
    return ( k.QuadPart + u.QuadPart ) * 100;
#else
    struct timespec ts;
    clock_gettime( CLOCK_THREAD_CPUTIME_ID, &ts );
    return (uint64_t) ts.tv_sec * NANOS_PER_SECOND + (uint64_t) ts.tv_nsec;
#endif
}

// ---- results ----

struct results_t
{
    const char * name;
    int measured;
    int drops;
    uint64_t error_ns[MEASURE_FRAMES];
    int num_errors;
    double cpu_percent;
    uint64_t converged_guard_ns;
    char note[256];
};

static int compare_uint64( const void * a, const void * b )
{
    const uint64_t va = *(const uint64_t*) a;
    const uint64_t vb = *(const uint64_t*) b;
    return va < vb ? -1 : va > vb ? 1 : 0;
}

static void report( struct results_t * r )
{
    if ( !r->measured )
    {
        printf( "%-34s SKIPPED: %s\n", r->name, r->note );
        fflush( stdout );
        return;
    }
    qsort( r->error_ns, (size_t) r->num_errors, sizeof(uint64_t), compare_uint64 );
    uint64_t sum = 0;
    for ( int i = 0; i < r->num_errors; i++ )
        sum += r->error_ns[i];
    const uint64_t avg = r->num_errors ? sum / (uint64_t) r->num_errors : 0;
    const uint64_t p50 = r->error_ns[ r->num_errors / 2 ];
    const uint64_t p99 = r->error_ns[ (int) ( r->num_errors * 0.99 ) ];
    const uint64_t p999 = r->error_ns[ (int) ( r->num_errors * 0.999 ) ];
    const uint64_t max = r->error_ns[ r->num_errors - 1 ];
    printf( "%-34s avg %8.1f  p50 %8.1f  p99 %8.1f  p999 %8.1f  max %9.1f  drops %3d  cpu %5.1f%%",
            r->name, avg / 1000.0, p50 / 1000.0, p99 / 1000.0, p999 / 1000.0, max / 1000.0, r->drops, r->cpu_percent );
    if ( r->converged_guard_ns )
        printf( "  guard %6.1fus", r->converged_guard_ns / 1000.0 );
    if ( r->note[0] )
        printf( "  (%s)", r->note );
    printf( "\n" );
    fflush( stdout );
}

// ---- a fixed workload standing in for the frame's work ----

static volatile uint64_t work_sink;

static void frame_work( void )
{
    uint64_t x = work_sink + 1;
    for ( int i = 0; i < 20000; i++ )
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    work_sink = x;
}

// ---- the common measurement loop ----
// mode 0: relative nanosleep       mode 1: absolute sleep
// mode 2: absolute sleep to (deadline - guard) then spin, guard adaptive

static void measure( struct results_t * r, int mode )
{
    const uint64_t period = (uint64_t) ( NANOS_PER_SECOND / FRAME_RATE_HZ );

    r->measured = 1;
    r->num_errors = 0;
    r->drops = 0;

    double guard_ewma = 200000.0;

    uint64_t deadline = now_ns() + period;
    const uint64_t cpu_start = thread_cpu_ns();
    const uint64_t wall_start = now_ns();

    for ( int frame = 0; frame < WARMUP_FRAMES + MEASURE_FRAMES; frame++ )
    {
        frame_work();

        if ( mode == 0 )
        {
            const uint64_t t = now_ns();
            if ( t < deadline )
            {
                const uint64_t remaining = deadline - t;
#if defined(_WIN32)
                Sleep( (DWORD) ( remaining / 1000000 ) );       // the space game's shipped path
#else
                struct timespec ts = { (time_t) ( remaining / NANOS_PER_SECOND ), (long) ( remaining % NANOS_PER_SECOND ) };
                nanosleep( &ts, NULL );
#endif
            }
        }
        else
        {
            uint64_t sleep_until = deadline;
            if ( mode == 2 )
            {
                const uint64_t guard = (uint64_t) guard_ewma + 50000;
                sleep_until = deadline > guard ? deadline - guard : 0;
            }
            sleep_until_ns( sleep_until );
            if ( mode == 2 )
            {
                const uint64_t woke = now_ns();
                if ( woke > sleep_until )
                    guard_ewma = 0.9 * guard_ewma + 0.1 * (double) ( woke - sleep_until );
                while ( now_ns() < deadline ) { }
            }
        }

        const uint64_t woke = now_ns();
        const uint64_t error = woke > deadline ? woke - deadline : 0;

        if ( frame >= WARMUP_FRAMES && r->num_errors < MEASURE_FRAMES )
            r->error_ns[r->num_errors++] = error;

        if ( error > period )
        {
            if ( frame >= WARMUP_FRAMES )
                r->drops += (int) ( error / period );
            deadline += ( ( error / period ) + 1 ) * period;
        }
        else
        {
            deadline += period;
        }
    }

    const uint64_t cpu = thread_cpu_ns() - cpu_start;
    const uint64_t wall = now_ns() - wall_start;
    r->cpu_percent = 100.0 * (double) cpu / (double) wall;
    if ( mode == 2 )
        r->converged_guard_ns = (uint64_t) guard_ewma + 50000;
}

// ---- priority elevation per platform, refusal reported never faked ----

#if defined(__linux__)

static int elevate_priority( struct results_t * r )
{
    struct sched_param param;
    memset( &param, 0, sizeof(param) );
    param.sched_priority = 80;
    if ( sched_setscheduler( 0, SCHED_FIFO, &param ) != 0 )
    {
        snprintf( r->note, sizeof(r->note), "SCHED_FIFO refused: %s", strerror( errno ) );
        return 0;
    }
    snprintf( r->note, sizeof(r->note), "SCHED_FIFO 80" );
    return 1;
}

static void restore_priority( void )
{
    struct sched_param param;
    memset( &param, 0, sizeof(param) );
    sched_setscheduler( 0, SCHED_OTHER, &param );
}

struct sched_attr_local
{
    uint32_t size; uint32_t sched_policy; uint64_t sched_flags; int32_t sched_nice;
    uint32_t sched_priority; uint64_t sched_runtime; uint64_t sched_deadline; uint64_t sched_period;
};

static void measure_sched_deadline( struct results_t * r )
{
    const uint64_t period = (uint64_t) ( NANOS_PER_SECOND / FRAME_RATE_HZ );

    struct sched_attr_local attr;
    memset( &attr, 0, sizeof(attr) );
    attr.size = sizeof(attr);
    attr.sched_policy = 6;                          // SCHED_DEADLINE
    attr.sched_runtime = period / 4;
    attr.sched_deadline = period;
    attr.sched_period = period;
    if ( syscall( SYS_sched_setattr, 0, &attr, 0 ) != 0 )
    {
        snprintf( r->note, sizeof(r->note), "SCHED_DEADLINE refused: %s", strerror( errno ) );
        return;
    }

    r->measured = 1;
    r->num_errors = 0;
    r->drops = 0;

    const uint64_t cpu_start = thread_cpu_ns();
    const uint64_t wall_start = now_ns();
    uint64_t deadline = 0;

    for ( int frame = 0; frame < WARMUP_FRAMES + MEASURE_FRAMES; frame++ )
    {
        frame_work();
        sched_yield();                              // suspend to the next kernel period

        const uint64_t woke = now_ns();
        if ( deadline == 0 )
            deadline = woke;                        // phase locks to the kernel period on first wake
        const uint64_t error = woke > deadline ? woke - deadline : 0;

        if ( frame >= WARMUP_FRAMES && r->num_errors < MEASURE_FRAMES )
            r->error_ns[r->num_errors++] = error;

        if ( error > period )
        {
            if ( frame >= WARMUP_FRAMES )
                r->drops += (int) ( error / period );
            deadline += ( ( error / period ) + 1 ) * period;
        }
        else
        {
            deadline += period;
        }
    }

    const uint64_t cpu = thread_cpu_ns() - cpu_start;
    const uint64_t wall = now_ns() - wall_start;
    r->cpu_percent = 100.0 * (double) cpu / (double) wall;

    restore_priority();
}

#elif defined(__APPLE__)

static int elevate_priority( struct results_t * r )
{
    // the real-time thread contract pro audio uses: period, computation,
    // constraint in mach time units

    const uint64_t period_ns = (uint64_t) ( NANOS_PER_SECOND / FRAME_RATE_HZ );
    const uint64_t period_mach = period_ns * timebase.denom / timebase.numer;

    thread_time_constraint_policy_data_t policy;
    policy.period = (uint32_t) period_mach;
    policy.computation = (uint32_t) ( period_mach / 4 );
    policy.constraint = (uint32_t) ( period_mach / 2 );
    policy.preemptible = TRUE;

    kern_return_t result = thread_policy_set( mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                              (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT );
    if ( result != KERN_SUCCESS )
    {
        snprintf( r->note, sizeof(r->note), "time constraint policy refused: %d", result );
        return 0;
    }
    snprintf( r->note, sizeof(r->note), "time constraint policy" );
    return 1;
}

static void restore_priority( void )
{
    thread_standard_policy_data_t policy;
    thread_policy_set( mach_thread_self(), THREAD_STANDARD_POLICY,
                       (thread_policy_t) &policy, THREAD_STANDARD_POLICY_COUNT );
}

#elif defined(_WIN32)

static int elevate_priority( struct results_t * r )
{
    if ( !SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL ) )
    {
        snprintf( r->note, sizeof(r->note), "TIME_CRITICAL refused: %lu", (unsigned long) GetLastError() );
        return 0;
    }
    snprintf( r->note, sizeof(r->note), "TIME_CRITICAL" );
    return 1;
}

static void restore_priority( void )
{
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_NORMAL );
}

#endif

// ---- optional contention ----

static volatile int load_running;

#if defined(_WIN32)

static DWORD WINAPI load_thread( LPVOID arg )
{
    (void) arg;
    volatile uint64_t x = 0;
    while ( load_running )
        x++;
    return 0;
}

#else

static void * load_thread( void * arg )
{
    (void) arg;
    volatile uint64_t x = 0;
    while ( load_running )
        x++;
    return NULL;
}

#endif

int main( int argc, char ** argv )
{
#if defined(__APPLE__)
    mach_timebase_info( &timebase );
#endif
#if defined(_WIN32)
    QueryPerformanceFrequency( &qpc_frequency );
    waitable_timer = CreateWaitableTimerExW( NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS );
    if ( !waitable_timer )
    {
        printf( "high resolution waitable timer unavailable, using standard\n" );
        waitable_timer = CreateWaitableTimerExW( NULL, NULL, 0, TIMER_ALL_ACCESS );
    }
#endif

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)
    printf( "frame_sync_prototype supports linux, macos and windows\n" );
    return 0;
#else

    const int with_load = argc > 1 && strcmp( argv[1], "load" ) == 0;

    printf( "frame sync prototype: %d frames at %.0fHz per mechanism%s (errors in microseconds)\n\n",
            MEASURE_FRAMES, FRAME_RATE_HZ, with_load ? ", with busy threads contending" : ", quiet" );

#if defined(_WIN32)
    HANDLE threads[64];
    int num_threads = 0;
    if ( with_load )
    {
        load_running = 1;
        SYSTEM_INFO info;
        GetSystemInfo( &info );
        num_threads = (int) info.dwNumberOfProcessors;
        if ( num_threads > 64 ) num_threads = 64;
        for ( int i = 0; i < num_threads; i++ )
            threads[i] = CreateThread( NULL, 0, load_thread, NULL, 0, NULL );
        printf( "started %d busy threads\n\n", num_threads );
    }
#else
    pthread_t threads[64];
    int num_threads = 0;
    if ( with_load )
    {
        load_running = 1;
        num_threads = (int) sysconf( _SC_NPROCESSORS_ONLN );
        if ( num_threads > 64 ) num_threads = 64;
        for ( int i = 0; i < num_threads; i++ )
            pthread_create( &threads[i], NULL, load_thread, NULL );
        printf( "started %d busy threads\n\n", num_threads );
    }
#endif

    static struct results_t r;

#if defined(_WIN32)
    memset( &r, 0, sizeof(r) ); r.name = "Sleep(ms)";
    measure( &r, 0 ); report( &r );

    timeBeginPeriod( 1 );
    memset( &r, 0, sizeof(r) ); r.name = "Sleep(ms) + timeBeginPeriod(1)";
    measure( &r, 0 ); report( &r );
#else
    memset( &r, 0, sizeof(r) ); r.name = "relative nanosleep";
    measure( &r, 0 ); report( &r );
#endif

    memset( &r, 0, sizeof(r) ); r.name = ABSOLUTE_SLEEP_NAME;
    measure( &r, 1 ); report( &r );

    memset( &r, 0, sizeof(r) ); r.name = ABSOLUTE_SLEEP_NAME " + spin guard";
    measure( &r, 2 ); report( &r );

    memset( &r, 0, sizeof(r) ); r.name = ABSOLUTE_SLEEP_NAME " + priority";
    if ( elevate_priority( &r ) ) { measure( &r, 1 ); restore_priority(); }
    report( &r );

    memset( &r, 0, sizeof(r) ); r.name = ABSOLUTE_SLEEP_NAME " + prio + guard";
    if ( elevate_priority( &r ) ) { measure( &r, 2 ); restore_priority(); }
    report( &r );

#if defined(__linux__)
    memset( &r, 0, sizeof(r) ); r.name = "SCHED_DEADLINE + yield";
    measure_sched_deadline( &r ); report( &r );
#endif

    if ( with_load )
    {
        load_running = 0;
#if defined(_WIN32)
        WaitForMultipleObjects( (DWORD) num_threads, threads, TRUE, INFINITE );
#else
        for ( int i = 0; i < num_threads; i++ )
            pthread_join( threads[i], NULL );
#endif
    }

#if defined(_WIN32)
    timeEndPeriod( 1 );
#endif

    return 0;

#endif
}
