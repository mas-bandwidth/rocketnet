/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional cases that the unit suite must not run: real threads, real
    frame pacing and wall-clock reads belong here, never in ./bin/test.
    Each test is one process, and starts a real platform primitive.
*/

#include "rocketnet_internal.h"
#include "rocketnet_ack.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_frame_sync.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_base64.h"
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <chrono>
#include <string>

#if defined( __APPLE__ ) && !defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <libproc.h>
#include <unistd.h>
#endif // #if defined( __APPLE__ ) && !defined( ROCKETNET_USE_SYSTEM_SODIUM )

static void rocketnet_platform_primitives_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_platform_primitives_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

static bool threads_work = false;

static void test_thread_function( void * )
{
    threads_work = true;
}

void rocketnet_test_platform_thread()
{
    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, test_thread_function, NULL );
    rocketnet_check( thread );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_check( threads_work );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static bool rocketnet_windows_handle_is_closed( HANDLE handle )
{
    DWORD flags = 0;
#if defined( _MSC_VER )
    __try
    {
        if ( GetHandleInformation( handle, &flags ) != 0 )
            return false;
        return GetLastError() == ERROR_INVALID_HANDLE;
    }
    __except ( GetExceptionCode() == STATUS_INVALID_HANDLE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH )
    {
        return true;
    }
#else
    return GetHandleInformation( handle, &flags ) == 0 && GetLastError() == ERROR_INVALID_HANDLE;
#endif // #if defined( _MSC_VER )
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

void rocketnet_test_frame_sync_wait()
{
    // paced for real, briefly: 25 frames at 250hz is ~100ms of wall time.
    // the assertion is pacing and accounting, never precision -- precision
    // was the measurement campaign's job, and CI runners are noisy.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV
    rocketnet_lifecycle_track_begin();

    rocketnet_lifecycle_resources_t before_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &before_resources ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV

    int create_status = ROCKETNET_STATUS_INVALID_ARGUMENT;
    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create_ex( NULL, 250.0, &create_status );
    rocketnet_check( frame_sync );
    rocketnet_check( create_status == ROCKETNET_STATUS_OK );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const HANDLE timer_handle = frame_sync->waitable_timer;
    rocketnet_check( timer_handle != NULL && timer_handle != INVALID_HANDLE_VALUE );
    rocketnet_check( !rocketnet_windows_handle_is_closed( timer_handle ) );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_resources_t live_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &live_resources ) );
    rocketnet_check( live_resources.errors == 0 );
    rocketnet_check( live_resources.created[ROCKETNET_LIFECYCLE_TIMER] == before_resources.created[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( live_resources.closed[ROCKETNET_LIFECYCLE_TIMER] == before_resources.closed[ROCKETNET_LIFECYCLE_TIMER] );
    rocketnet_check( live_resources.live_count == before_resources.live_count + 1 );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    const double start = rocketnet_platform_time();

    for ( int i = 0; i < 25; i++ )
    {
        rocketnet_frame_sync_wait( frame_sync );
    }

    const double elapsed = rocketnet_platform_time() - start;

#if ROCKETNET_DEV
    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 25 );
#endif // #if ROCKETNET_DEV
    rocketnet_check( elapsed > 0.06 );                              // it actually paced: a free run finishes in microseconds

    // the suite watchdog catches a hang; scheduler latency is not a failure

#if ROCKETNET_DEV
    rocketnet_check( !stats.spinning || stats.priority_held );      // never an unprivileged spin
    rocketnet_check( stats.guard_seconds <= ( 1.0 / 250.0 ) / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR + 1e-9 );
#endif // #if ROCKETNET_DEV

    rocketnet_frame_sync_destroy( frame_sync );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_check( rocketnet_windows_handle_is_closed( timer_handle ) );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_resources_t after_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &after_resources ) );
    rocketnet_check( after_resources.errors == 0 );
    rocketnet_check( after_resources.created[ROCKETNET_LIFECYCLE_TIMER] == before_resources.created[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.closed[ROCKETNET_LIFECYCLE_TIMER] == before_resources.closed[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.live_count == before_resources.live_count );
    rocketnet_lifecycle_track_end();
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// The successful create, first stats and destroy of a frame sync. Moved from
// the unit suite's frame sync lifecycle case: a create that succeeds creates
// the real Windows high-resolution waitable timer, and destroy closes it. The
// frame-rate refusals that return before any timer stay unit coverage
// (test_frame_sync_create_refuses_invalid_rates).
void rocketnet_test_frame_sync_lifecycle()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV
    rocketnet_lifecycle_track_begin();

    rocketnet_lifecycle_resources_t before_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &before_resources ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 60.0 );
    rocketnet_check( frame_sync );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const HANDLE timer_handle = frame_sync->waitable_timer;
    rocketnet_check( timer_handle != NULL && timer_handle != INVALID_HANDLE_VALUE );
    rocketnet_check( !rocketnet_windows_handle_is_closed( timer_handle ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV
    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 0 );
    rocketnet_check( stats.dropped_frames == 0 );
    rocketnet_check( stats.priority_held == 0 );                    // taken at the first wait, not before
    rocketnet_check( stats.spinning == 0 );
    rocketnet_check( stats.wakeup_error_seconds == 0.0 );
    rocketnet_check( stats.guard_seconds > 0.0 );
    rocketnet_check( stats.guard_seconds <= ( 1.0 / 60.0 ) / ROCKETNET_FRAME_SYNC_GUARD_CAP_DIVISOR + 1e-9 );
#endif // #if ROCKETNET_DEV

    rocketnet_frame_sync_destroy( frame_sync );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_check( rocketnet_windows_handle_is_closed( timer_handle ) );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_resources_t after_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &after_resources ) );
    rocketnet_check( after_resources.errors == 0 );
    rocketnet_check( after_resources.created[ROCKETNET_LIFECYCLE_TIMER] == before_resources.created[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.closed[ROCKETNET_LIFECYCLE_TIMER] == before_resources.closed[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.live_count == before_resources.live_count );
    rocketnet_lifecycle_track_end();
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

// The structured create's success exit. Moved from the unit lifecycle case
// test_lifecycle_ex_reports_every_status: a create that succeeds creates the
// real Windows high-resolution waitable timer. The library runs on a counting
// allocator given in context, as there, so the one frame sync allocation is
// seen to be released by destroy and nothing is left at term.

struct rocketnet_frame_sync_create_ex_allocator_t
{
    int live;
    int calls;
};

static rocketnet_frame_sync_create_ex_allocator_t rocketnet_frame_sync_create_ex_allocator;

static void * rocketnet_frame_sync_create_ex_malloc( void * context, size_t bytes )
{
    rocketnet_frame_sync_create_ex_allocator_t * state = (rocketnet_frame_sync_create_ex_allocator_t *) context;
    rocketnet_check( state == &rocketnet_frame_sync_create_ex_allocator );
    ++state->calls;

    void * p = malloc( bytes );
    if ( p )
    {
        ++state->live;
    }

    return p;
}

static void rocketnet_frame_sync_create_ex_free( void * context, void * pointer )
{
    if ( !pointer )
        return;

    rocketnet_frame_sync_create_ex_allocator_t * state = (rocketnet_frame_sync_create_ex_allocator_t *) context;
    rocketnet_check( state == &rocketnet_frame_sync_create_ex_allocator );
    rocketnet_check( state->live > 0 );
    --state->live;
    free( pointer );
}

void rocketnet_test_frame_sync_create_ex_success_status()
{
    memset( &rocketnet_frame_sync_create_ex_allocator, 0, sizeof( rocketnet_frame_sync_create_ex_allocator ) );

    rocketnet_frame_sync_create_ex_allocator_t * state = &rocketnet_frame_sync_create_ex_allocator;

    // the worker starts the process initialized; restart it on the counting
    // allocator with that allocator's state as context

    rocketnet_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

    int status = -1;
    rocketnet_check( rocketnet_init_ex( state, rocketnet_frame_sync_create_ex_malloc, rocketnet_frame_sync_create_ex_free, &status ) );
    rocketnet_check( status == ROCKETNET_STATUS_OK );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV
    rocketnet_lifecycle_track_begin();

    rocketnet_lifecycle_resources_t before_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &before_resources ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS && ROCKETNET_DEV

    const int live_before = state->live;
    const int calls_before = state->calls;

    // frame sync success

    status = -1;

    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create_ex( state, 60.0, &status );
    rocketnet_check( frame_sync != NULL );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    rocketnet_check( state->calls == calls_before + 1 );
    rocketnet_check( state->live == live_before + 1 );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    const HANDLE timer_handle = frame_sync->waitable_timer;
    rocketnet_check( timer_handle != NULL && timer_handle != INVALID_HANDLE_VALUE );
    rocketnet_check( !rocketnet_windows_handle_is_closed( timer_handle ) );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    rocketnet_frame_sync_destroy( frame_sync );
    rocketnet_check( state->live == live_before );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_check( rocketnet_windows_handle_is_closed( timer_handle ) );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_resources_t after_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &after_resources ) );
    rocketnet_check( after_resources.errors == 0 );
    rocketnet_check( after_resources.created[ROCKETNET_LIFECYCLE_TIMER] == before_resources.created[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.closed[ROCKETNET_LIFECYCLE_TIMER] == before_resources.closed[ROCKETNET_LIFECYCLE_TIMER] + 1 );
    rocketnet_check( after_resources.live_count == before_resources.live_count );
    rocketnet_lifecycle_track_end();
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    // a clean term is OK and leaves no allocation behind

    rocketnet_check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    rocketnet_check( state->live == 0 );

    // the default allocator again, for the worker's own term

    rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

#if defined( __APPLE__ ) && !defined( ROCKETNET_USE_SYSTEM_SODIUM )

// The bundled RNG's descriptor lifecycle on macOS. Moved from the unit suite
// (tests/test_unit_rng_lifecycle.cpp): it reads the live process's open
// descriptors through proc_pidinfo across real init, random, keygen and term
// cycles, which is OS state. The system-sodium case, which installs a fake RNG
// and reads nothing from the OS, stays a unit case.

static long rng_lifecycle_fd_count()
{
    const int bytes = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, NULL, 0 );
    if ( bytes <= 0 )
        return -1;

    void * entries = malloc( (size_t) bytes );
    if ( !entries )
        return -1;

    const int used = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, entries, bytes );
    free( entries );

    return used < 0 ? -1 : (long) ( used / (int) PROC_PIDLISTFD_SIZE );
}

void rocketnet_test_bundled_rng_closes_and_reopens_on_mac()
{
    // The functional worker begins initialized. Establish the uninitialized
    // baseline after the first completed term, then exercise several distinct
    // cycles.

    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );

    const long baseline = rng_lifecycle_fd_count();
    rocketnet_check( baseline >= 0 );

    // sodium_init is one-time. Subsequent empty cycles must neither reopen
    // the RNG descriptor nor retain any other descriptor at termination.

    for ( int cycle = 0; cycle < 3; ++cycle )
    {
        rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
        rocketnet_check( rng_lifecycle_fd_count() == baseline );
        rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
        rocketnet_check( rng_lifecycle_fd_count() == baseline );
    }

    for ( int cycle = 0; cycle < 3; ++cycle )
    {
        rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

        uint8_t random_data[32];
        rocketnet_random_bytes( random_data, sizeof( random_data ) );

        uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
        uint8_t secret_key[32];     // an X25519 server secret, present in production builds too
        rocketnet_check( rocketnet_generate_server_keypair( public_key, secret_key ) == ROCKETNET_STATUS_OK );

        // The old bug kept /dev/urandom open across term. If the first term
        // failed to close it, baseline already includes that descriptor and
        // this active-cycle assertion fails rather than masking the leak.

        rocketnet_check( rng_lifecycle_fd_count() == baseline + 1 );

        if ( cycle == 1 )
        {
            rocketnet_client_config_t config;
            rocketnet_check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
            config.max_fragmented_messages = 1;

            rocketnet_status_t status = ROCKETNET_STATUS_OK;
            rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &status );
            rocketnet_check( client != NULL && status == ROCKETNET_STATUS_OK );

            const long with_client = rng_lifecycle_fd_count();
            rocketnet_check( with_client >= 0 );
            rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_LIVE_ALLOCATIONS );
            rocketnet_check( rng_lifecycle_fd_count() == with_client );
            rocketnet_client_destroy( client );
        }

        rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
        rocketnet_check( rng_lifecycle_fd_count() == baseline );
    }

    // Restore the worker's initialized state for its ordinary final term

    rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

#endif // #if defined(__APPLE__) && !defined(ROCKETNET_USE_SYSTEM_SODIUM)

#if ROCKETNET_DEV

struct rocketnet_frame_sync_timer_seam_state_t
{
    int calls;
    unsigned flags[8];
    int live_allocations;
    int total_allocations;
    size_t live_bytes;
    int error_lines;
};

static rocketnet_frame_sync_timer_seam_state_t rocketnet_frame_sync_timer_seam_state;

static void * rocketnet_frame_sync_seam_malloc( void * context, size_t bytes )
{
    rocketnet_frame_sync_timer_seam_state_t * state = (rocketnet_frame_sync_timer_seam_state_t *) context;
    rocketnet_check( state == &rocketnet_frame_sync_timer_seam_state );

    void * raw = malloc( sizeof( size_t ) + bytes );
    if ( !raw )
        return NULL;
    memcpy( raw, &bytes, sizeof( size_t ) );
    ++state->total_allocations;
    ++state->live_allocations;
    state->live_bytes += bytes;

    return (uint8_t *) raw + sizeof( size_t );
}

static void rocketnet_frame_sync_seam_free( void * context, void * pointer )
{
    if ( !pointer )
        return;

    rocketnet_frame_sync_timer_seam_state_t * state = (rocketnet_frame_sync_timer_seam_state_t *) context;
    rocketnet_check( state == &rocketnet_frame_sync_timer_seam_state );

    void * raw = (uint8_t *) pointer - sizeof( size_t );
    size_t bytes = 0;
    memcpy( &bytes, raw, sizeof( size_t ) );
    rocketnet_check( state->live_allocations > 0 );
    rocketnet_check( state->live_bytes >= bytes );
    --state->live_allocations;
    state->live_bytes -= bytes;
    free( raw );
}

static void rocketnet_frame_sync_seam_log( void * context, rocketnet_log_level_t level, const char * message, size_t )
{
    rocketnet_frame_sync_timer_seam_state_t * state = (rocketnet_frame_sync_timer_seam_state_t *) context;
    if ( state && level == ROCKETNET_LOG_LEVEL_ERROR &&
         strstr( message, "frame sync: could not create high resolution waitable timer" ) != NULL )
    {
        ++state->error_lines;
    }
}

static void * rocketnet_frame_sync_reject_high_resolution_timer( unsigned flags )
{
    if ( rocketnet_frame_sync_timer_seam_state.calls < 8 )
    {
        rocketnet_frame_sync_timer_seam_state.flags[rocketnet_frame_sync_timer_seam_state.calls] = flags;
    }

    ++rocketnet_frame_sync_timer_seam_state.calls;

    if ( ( flags & 0x00000002u ) != 0 )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        SetLastError( ERROR_NOT_SUPPORTED );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        return NULL;
    }

    return rocketnet_platform_priority_calls_real()->create_waitable_timer( flags );
}

void rocketnet_test_frame_sync_high_resolution_timer_required()
{
    memset( &rocketnet_frame_sync_timer_seam_state, 0, sizeof( rocketnet_frame_sync_timer_seam_state ) );

    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_init( &rocketnet_frame_sync_timer_seam_state,
                                     rocketnet_frame_sync_seam_malloc,
                                     rocketnet_frame_sync_seam_free ) == ROCKETNET_STATUS_OK );

    const rocketnet_log_level_t saved_level = rocketnet_log_level_current();

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( rocketnet_frame_sync_seam_log, &rocketnet_frame_sync_timer_seam_state );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    DWORD handles_before = 0;
    rocketnet_check( GetProcessHandleCount( GetCurrentProcess(), &handles_before ) != 0 );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_track_begin();

    rocketnet_lifecycle_resources_t before_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &before_resources ) );
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    struct rocketnet_platform_priority_calls_t fake = *rocketnet_platform_priority_calls_real();
    fake.create_waitable_timer = rocketnet_frame_sync_reject_high_resolution_timer;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    const int allocations_before = rocketnet_frame_sync_timer_seam_state.total_allocations;

    int status = ROCKETNET_STATUS_OK;

    struct rocketnet_frame_sync_t * frame_sync =
        rocketnet_frame_sync_create_ex( &rocketnet_frame_sync_timer_seam_state, 250.0, &status );
    rocketnet_check( frame_sync == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_TIMER_CREATE_FAILED );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.calls == 1 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.flags[0] == 0x00000002u );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.total_allocations == allocations_before + 1 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_allocations == 0 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_bytes == 0 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.error_lines == 1 );

    rocketnet_check( rocketnet_frame_sync_create( &rocketnet_frame_sync_timer_seam_state, 250.0 ) == NULL );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.calls == 2 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.flags[1] == 0x00000002u );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.total_allocations == allocations_before + 2 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_allocations == 0 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_bytes == 0 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.error_lines == 2 );

    rocketnet_platform_set_priority_calls_for_testing( NULL );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    DWORD handles_after = 0;
    rocketnet_check( GetProcessHandleCount( GetCurrentProcess(), &handles_after ) != 0 );
    rocketnet_check( handles_after <= handles_before );
#if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_lifecycle_resources_t after_resources;
    rocketnet_check( rocketnet_lifecycle_track_read( &after_resources ) );
    rocketnet_check( after_resources.errors == 0 );
    rocketnet_check( after_resources.live_count == before_resources.live_count );
    rocketnet_check( after_resources.created[ROCKETNET_LIFECYCLE_TIMER] == before_resources.created[ROCKETNET_LIFECYCLE_TIMER] );
    rocketnet_check( after_resources.closed[ROCKETNET_LIFECYCLE_TIMER] == before_resources.closed[ROCKETNET_LIFECYCLE_TIMER] );
    rocketnet_lifecycle_track_end();
#endif // #if ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_allocations == 0 );
    rocketnet_check( rocketnet_frame_sync_timer_seam_state.live_bytes == 0 );
    rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

#endif // #if ROCKETNET_DEV

#if ROCKETNET_DEV
void rocketnet_test_frame_sync_dropped_stats()
{
    // The dropped-frame observability case: the real public path
    // (create/wait/stats/destroy) must make missed server frame intervals
    // visible in dropped_frames. A baseline normal cadence stays clean
    // within its documented bound, a bounded expensive frame workload
    // spanning several configured periods must raise the public count
    // inside bounds derived from independently measured elapsed time, and
    // recovery at normal cadence must leave the total cumulative. No
    // exact OS schedule is asserted anywhere: the workload is
    // predicate-driven (busy work until the target elapsed time is
    // measured on the steady clock), never a fixed sleep treated as proof.

    const double frame_rate = 250.0;
    const uint64_t period_ns = (uint64_t) ( 1000000000.0 / frame_rate );   // 4ms
    struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, frame_rate );
    rocketnet_check( frame_sync );

    // Phase A: baseline normal cadence, 12 frames (~48ms of wall time)

    for ( int i = 0; i < 12; i++ )
    {
        rocketnet_frame_sync_wait( frame_sync );
    }

    struct rocketnet_frame_sync_stats_t stats;
    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 12 );

    const uint64_t baseline_drops = stats.dropped_frames;

    // documented bound: a clean cadence on a noisy host may still lose a
    // couple of frames to scheduler latency; more than that is a defect

    rocketnet_check( baseline_drops <= 2 );

    // Phase B: bounded expensive frame work spanning several periods.
    // Busy work until at least 10 periods have elapsed, measured
    // independently with the steady clock; the work itself is the frame
    // load, and its elapsed time is the evidence the bounds derive from.

    const uint64_t target_work_ns = 10 * period_ns;
    volatile uint64_t sink = 0;
    const auto work_start = std::chrono::steady_clock::now();

    for ( ;; )
    {
        sink += 1;
        if ( std::chrono::duration<double, std::nano>(
                 std::chrono::steady_clock::now() - work_start )
                 .count() >= (double) target_work_ns )
            break;
    }

    const uint64_t work_elapsed_ns = (uint64_t) std::chrono::duration<double, std::nano>(
                                         std::chrono::steady_clock::now() - work_start )
                                         .count();
    rocketnet_check( work_elapsed_ns >= target_work_ns );

    // the wait that observes the miss, as a real frame loop would call it
    // after expensive work

    rocketnet_frame_sync_wait( frame_sync );

    rocketnet_frame_sync_stats( frame_sync, &stats );

    const uint64_t after_work_drops = stats.dropped_frames;
    const uint64_t work_drops = after_work_drops - baseline_drops;

    // Bounds from the measured elapsed time and the configured period,
    // tolerant of scheduler noise on both sides. The work spans at least
    // 10 periods, so the accounting must be multiple intervals high.

    const double periods_of_work = (double) work_elapsed_ns / (double) period_ns;

    rocketnet_check( work_drops >= 3 );                                     // nonzero, multiple missed intervals
    rocketnet_check( (double) work_drops > periods_of_work - 3.0 );         // not undercounting
    rocketnet_check( (double) work_drops < periods_of_work + 3.0 );         // not overcounting

    // Phase C: recovery at normal cadence. The total stays cumulative
    // (a reset would clear it), the frame count advances by exactly the
    // waits run, and lateness stays coherent.

    for ( int i = 0; i < 12; i++ )
    {
        rocketnet_frame_sync_wait( frame_sync );
    }

    rocketnet_frame_sync_stats( frame_sync, &stats );
    rocketnet_check( stats.frame == 25 );                                   // 12 + 1 + 12 waits
    rocketnet_check( stats.dropped_frames >= after_work_drops );             // cumulative, never reset
    rocketnet_check( stats.dropped_frames <= after_work_drops + 2 );         // recovery adds at most noise
    rocketnet_check( stats.wakeup_error_seconds >= 0.0 );                   // lateness coherent

    printf( "    frame sync dropped stats: period_ns %llu, work_elapsed_ns %llu, baseline %llu, work_drops %llu, final %llu\n",
            (unsigned long long) period_ns, (unsigned long long) work_elapsed_ns,
            (unsigned long long) baseline_drops, (unsigned long long) work_drops,
            (unsigned long long) stats.dropped_frames );
    fflush( stdout );
    (void) sink;

    rocketnet_frame_sync_destroy( frame_sync );
}
#endif // #if ROCKETNET_DEV

static void rocketnet_test_fill_vector_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                               struct rocketnet_connect_token_private_t * private_fields )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, 32 );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = ROCKETNET_TEST_CLIENT_ID;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, 32 );
}

struct rocketnet_mint_thread_result_t
{
    bool ok;
    int mints;
};

// one thread's independent run: mint into its own buffer, then verify and open
// every token against the test keys and its own private fields
static void rocketnet_test_connect_token_mint_thread( struct rocketnet_signed_connect_data_t signed_fields,
                                                      struct rocketnet_connect_token_private_t private_fields,
                                                      uint64_t base_client_id,
                                                      int iterations,
                                                      struct rocketnet_mint_thread_result_t * result )
{
    result->ok = true;
    result->mints = 0;

    for ( int i = 0; i < iterations; i++ )
    {
        struct rocketnet_connect_token_private_t fields = private_fields;
        fields.client_id = base_client_id + (uint64_t) i;

        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( !rocketnet_write_connect_token( &signed_fields, &fields, rocketnet_test_backend_secret_key, token_data ) )
        {
            result->ok = false;
            return;
        }

        struct rocketnet_connect_token_t token;
        rocketnet_read_connect_token( &token, token_data );
        if ( !rocketnet_verify_signed_connect_data( token.signed_connect_data, token.signature, rocketnet_test_backend_public_key ) )
        {
            result->ok = false;
            return;
        }

        struct rocketnet_connect_token_private_t opened;
        if ( !rocketnet_open_connect_token_private( &opened, token.signed_connect_data, rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) )
        {
            result->ok = false;
            return;
        }

        if ( opened.client_id != fields.client_id )
        {
            result->ok = false;
            return;
        }

        if ( memcmp( opened.client_to_server_key, fields.client_to_server_key, ROCKETNET_KEY_BYTES ) != 0 ||
             memcmp( opened.server_to_client_key, fields.server_to_client_key, ROCKETNET_KEY_BYTES ) != 0 )
        {
            result->ok = false;
            return;
        }

        result->mints++;
    }
}

void rocketnet_test_connect_token_mint_concurrent()
{
    // two threads mint independent tokens at once, each with its own client
    // ids and output buffer, and each opens only its own tokens: minting must
    // touch no state outside its own call

    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    rocketnet_test_fill_vector_fields( &signed_fields, &private_fields );

    const int iterations = 300;
    struct rocketnet_mint_thread_result_t results[2];

    std::thread thread_a( rocketnet_test_connect_token_mint_thread, signed_fields, private_fields,
                          ROCKETNET_TEST_CLIENT_ID + 1, iterations, &results[0] );
    std::thread thread_b( rocketnet_test_connect_token_mint_thread, signed_fields, private_fields,
                          ROCKETNET_TEST_CLIENT_ID + 0x100000, iterations, &results[1] );
    thread_a.join();
    thread_b.join();

    rocketnet_check( results[0].ok );
    rocketnet_check( results[1].ok );
    rocketnet_check( results[0].mints == iterations );
    rocketnet_check( results[1].mints == iterations );
}

// two independent ack endpoints -- one per connection, as a client and a
// server in one process each own theirs -- apply ack blocks at once on two
// threads. the ack walk must touch no state outside its own endpoint: under
// a thread sanitizer any shared write here is a reported race.

struct rocketnet_ack_thread_result_t
{
    bool ok;
    int acked;
};

static void rocketnet_test_ack_endpoint_thread( int rounds, struct rocketnet_ack_thread_result_t * result )
{
    result->ok = false;
    result->acked = 0;

    struct rocketnet_ack_endpoint_t * endpoint = (struct rocketnet_ack_endpoint_t *)
        rocketnet_malloc( NULL, sizeof( struct rocketnet_ack_endpoint_t ) );
    if ( !endpoint )
        return;
    rocketnet_ack_endpoint_reset( endpoint );

    uint64_t acked[64];
    uint64_t unknown = 0;
    uint64_t batch_full = 0;
    double time = 1000.0;
    uint64_t sequence = 100;

    bool ok = true;

    for ( int round = 0; round < rounds; round++ )
    {
        // send 16 packets, then apply one block that acknowledges all of them

        const uint64_t first = sequence;

        for ( int i = 0; i < 16; i++, sequence++ )
        {
            rocketnet_ack_endpoint_record_sent( endpoint, sequence, 100, sequence, 1, 0, time );
        }

        struct rocketnet_ack_block_t block;
        memset( &block, 0, sizeof( block ) );
        block.largest = sequence - 1;
        block.first_range_length = (uint32_t) ( sequence - first );

        time += 0.01;

        int acked_count = 0;
        const int newly_acked = rocketnet_ack_endpoint_process_block( endpoint, &block, time, acked, 64, &acked_count, &unknown, &batch_full );
        if ( newly_acked != 16 || acked_count != 16 )
        {
            ok = false;
        }

        result->acked += newly_acked;
    }

    rocketnet_free( NULL, endpoint );
    result->ok = ok && unknown == 0 && batch_full == 0;
}

void rocketnet_test_ack_endpoints_independent_across_threads()
{
    const int rounds = 2000;
    struct rocketnet_ack_thread_result_t results[2];

    std::thread thread_a( rocketnet_test_ack_endpoint_thread, rounds, &results[0] );
    std::thread thread_b( rocketnet_test_ack_endpoint_thread, rounds, &results[1] );
    thread_a.join();
    thread_b.join();

    rocketnet_check( results[0].ok );
    rocketnet_check( results[1].ok );
    rocketnet_check( results[0].acked == rounds * 16 );
    rocketnet_check( results[1].acked == rounds * 16 );
}

#if defined( ROCKETNET_PLATFORM_PRIMITIVES_STANDALONE )

static const char * const rocketnet_platform_environment_probe = "ROCKETNET_PLATFORM_ENVIRONMENT_PROBE";

static void rocketnet_platform_primitives_set_environment( const char * value )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    rocketnet_check( _putenv_s( rocketnet_platform_environment_probe, value ? value : "" ) == 0 );
#else
    rocketnet_check( value ? setenv( rocketnet_platform_environment_probe, value, 1 ) == 0 : unsetenv( rocketnet_platform_environment_probe ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

static void rocketnet_platform_primitives_environment()
{
    std::string saved;
    bool saved_present = false;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    char * original = NULL;
    size_t original_bytes = 0;
    rocketnet_check( _dupenv_s( &original, &original_bytes, rocketnet_platform_environment_probe ) == 0 );
    if ( original )
    {
        saved_present = true;
        saved = original;
        free( original );
    }
#else
    const char * original = getenv( rocketnet_platform_environment_probe );
    if ( original )
    {
        saved_present = true;
        saved = original;
    }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

    char value[32];

    rocketnet_platform_primitives_set_environment( NULL );
    rocketnet_check( rocketnet_platform_getenv( rocketnet_platform_environment_probe, value, sizeof( value ) ) == 0 );
    rocketnet_check( value[0] == '\0' );
    rocketnet_platform_primitives_set_environment( "native-platform-path" );
    rocketnet_check( rocketnet_platform_getenv( rocketnet_platform_environment_probe, value, sizeof( value ) ) == 1 );
    rocketnet_check( strcmp( value, "native-platform-path" ) == 0 );

    char short_value[4];
    rocketnet_check( rocketnet_platform_getenv( rocketnet_platform_environment_probe, short_value, sizeof( short_value ) ) == -1 );
    rocketnet_check( short_value[0] == '\0' );
    rocketnet_platform_primitives_set_environment( saved_present ? saved.c_str() : NULL );
}

// These guards change the real process environment only in this functional
// executable. They restore exact values and never enter a library artifact.
class rocketnet_platform_real_environment_t
{
public:
    explicit rocketnet_platform_real_environment_t( const char * variable ) : name( variable ), present( false )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        char * value = NULL;
        size_t bytes = 0;
        rocketnet_check( _dupenv_s( &value, &bytes, name.c_str() ) == 0 );
        if ( value )
        {
            present = true;
            saved = value;
            free( value );
        }
#else
        const char * value = getenv( name.c_str() );
        if ( value )
        {
            present = true;
            saved = value;
        }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }
    ~rocketnet_platform_real_environment_t() { set( present ? saved.c_str() : NULL ); }
    void set( const char * value )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rocketnet_check( _putenv_s( name.c_str(), value ? value : "" ) == 0 );
#else
        rocketnet_check( value ? setenv( name.c_str(), value, 1 ) == 0 : unsetenv( name.c_str() ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

private:
    std::string name;
    std::string saved;
    bool present;
};

#if !ROCKETNET_DEV
static int rocketnet_platform_primitives_setenv( const char * name, const char * value )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return _putenv_s( name, value );
#else
    return setenv( name, value, 1 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}
static int rocketnet_platform_primitives_unsetenv( const char * name )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return _putenv_s( name, "" );
#else
    return unsetenv( name );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}
#endif // #if !ROCKETNET_DEV

static const char * rocketnet_platform_primitives_capture_prefix;
static int rocketnet_platform_primitives_matching_lines;
static rocketnet_log_level_t rocketnet_platform_primitives_capture_level;
static rocketnet_log_level_t rocketnet_platform_primitives_saved_level;
static void rocketnet_platform_primitives_capture( void *, rocketnet_log_level_t level, const char * message, size_t )
{
    if ( level == rocketnet_platform_primitives_capture_level && rocketnet_platform_primitives_capture_prefix &&
         strstr( message, rocketnet_platform_primitives_capture_prefix ) )
        ++rocketnet_platform_primitives_matching_lines;
}
static void rocketnet_platform_primitives_capture_begin( const char * prefix, rocketnet_log_level_t level = ROCKETNET_LOG_LEVEL_ERROR )
{
    rocketnet_platform_primitives_capture_prefix = prefix;
    rocketnet_platform_primitives_capture_level = level;
    rocketnet_platform_primitives_matching_lines = 0;
    rocketnet_platform_primitives_saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( rocketnet_platform_primitives_capture, NULL );
}
static void rocketnet_platform_primitives_capture_end()
{
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( rocketnet_platform_primitives_saved_level );
}

static void rocketnet_platform_primitives_key_warning_environment()
{
    rocketnet_platform_real_environment_t transport( "ROCKETNET_SERVER_TRANSPORT" );
    transport.set( NULL );
    rocketnet_platform_real_environment_t budget( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );
    budget.set( "1" );

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    memset( config.backend_public_key, 0x42, 32 );
    memset( config.server_public_key, 0x43, 32 );
    memset( config.server_secret_key, 0x44, 32 );
    config.protocol_id = 1;

    // a private filter: DEV=0 refuses the published test filter before the ladder
    {
        const uint8_t private_filter[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
        memcpy( config.packet_filter, private_filter, sizeof( private_filter ) );
    }
    snprintf( config.transport, sizeof( config.transport ), "%s", "portable" );

    int status = -1;

    rocketnet_platform_primitives_capture_begin( "public and not for production", ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    rocketnet_check( rocketnet_platform_primitives_matching_lines == 0 );
    rocketnet_platform_primitives_capture_end();
}

static void rocketnet_platform_primitives_public_config_environment()
{
    rocketnet_platform_real_environment_t client_env( "ROCKETNET_CLIENT_SEND_KBPS" );
    rocketnet_platform_real_environment_t server_env( "ROCKETNET_SERVER_MAX_CLIENTS" );

    rocketnet_client_config_t client;
    rocketnet_check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;

    client_env.set( "not-a-number" );
    rocketnet_check( rocketnet_client_create( NULL, &client, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    client_env.set( NULL );

    rocketnet_server_config_t server;
    rocketnet_check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    server_env.set( "not-a-number" );
    status = ROCKETNET_STATUS_OK;
    rocketnet_check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
}

#if !ROCKETNET_DEV
static void rocketnet_platform_primitives_production_config( rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config( config );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    config->protocol_id = 1;
    {
        const uint8_t private_filter[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
        memcpy( config->packet_filter, private_filter, sizeof( private_filter ) );
    }
    config->max_clients = 1;
    snprintf( config->transport, sizeof( config->transport ), "%s", "portable" );
}

static const char * rocketnet_platform_primitives_native_transport()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    return "gso";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    return "msgx";
#elif ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    return "iocp";
#else
    return "portable";
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
}

static void rocketnet_platform_primitives_production_status_environment()
{
    rocketnet_platform_real_environment_t transport( "ROCKETNET_SERVER_TRANSPORT" );
    transport.set( NULL );
    rocketnet_platform_real_environment_t maximum( "ROCKETNET_SERVER_MAX_CLIENTS" );
    rocketnet_platform_real_environment_t budget( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );
    maximum.set( "not-a-number" );

    int status = -1;
    rocketnet_check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:12345", NULL, 1000.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    maximum.set( NULL );

    rocketnet_server_transport_config_t config;
    rocketnet_platform_primitives_production_config( &config );
    budget.set( "not-a-number" );
    status = -1;
    rocketnet_check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    budget.set( "1" );

    const char * const transports[] = { rocketnet_platform_primitives_native_transport(), "portable" };

    for ( int i = 0; i < 2; ++i )
    {
        snprintf( config.transport, sizeof( config.transport ), "%s", transports[i] );
        status = -1;
        rocketnet_check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:12345", &config, 1000.0, &status ) == NULL );
        rocketnet_check( status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
    }
}

static void rocketnet_platform_primitives_production_simulator_environment()
{
    rocketnet_platform_real_environment_t client_drop( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" );
    rocketnet_platform_real_environment_t client_reverse( "ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW" );
    rocketnet_platform_real_environment_t server_drop( "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH" );
    rocketnet_platform_real_environment_t server_reverse( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" );
    client_drop.set( NULL );
    client_reverse.set( NULL );
    server_drop.set( NULL );
    server_reverse.set( NULL );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );

    rocketnet_server_transport_config_t server_config;
    rocketnet_platform_primitives_production_config( &server_config );

    int client_fields = 0;
    const rocketnet_config_field_t * client_table = rocketnet_client_transport_config_fields( &client_fields );
    int server_fields = 0;
    const rocketnet_config_field_t * server_table = rocketnet_server_transport_config_fields( &server_fields );
    rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = 1;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = UINT64_C( 1 ) << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = 0x40;
    signed_fields.ipv4_address[5] = 0x9c;
    memset( signed_fields.server_public_key, 0x43, sizeof( signed_fields.server_public_key ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );
    rocketnet_write_signed_connect_data( &signed_fields, token );
    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH", "7" ) == 0 );

    int status = ROCKETNET_STATUS_OK;

    rocketnet_platform_primitives_capture_begin( "simulated_drop_every_nth" );
    rocketnet_check( rocketnet_client_transport_create_ex( NULL, token, &client_config, 1000.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    rocketnet_check( rocketnet_platform_primitives_matching_lines == 1 );
    rocketnet_platform_primitives_capture_end();
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" ) == 0 );

    server_config.protocol_id = 1;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );
    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW", "4" ) == 0 );
    status = ROCKETNET_STATUS_OK;
    rocketnet_platform_primitives_capture_begin( "simulated_reverse_window" );
    rocketnet_check( rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, 1000.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    rocketnet_check( rocketnet_platform_primitives_matching_lines == 1 );
    rocketnet_platform_primitives_capture_end();
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" ) == 0 );

    // the env overlay reaches both fields on both configs

    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH", "7" ) == 0 );
    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW", "4" ) == 0 );
    rocketnet_client_transport_default_config_current( &client_config );
    rocketnet_check( rocketnet_config_apply_env( "client", &client_config, client_table, client_fields ) );
    rocketnet_check( client_config.simulated_drop_every_nth == 7 );
    rocketnet_check( client_config.simulated_reverse_window == 4 );
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" ) == 0 );
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW" ) == 0 );

    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH", "5" ) == 0 );
    rocketnet_check( rocketnet_platform_primitives_setenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW", "3" ) == 0 );
    rocketnet_server_transport_default_config_current( &server_config );
    rocketnet_check( rocketnet_config_apply_env( "server", &server_config, server_table, server_fields ) );
    rocketnet_check( server_config.simulated_drop_every_nth == 5 );
    rocketnet_check( server_config.simulated_reverse_window == 3 );
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH" ) == 0 );
    rocketnet_check( rocketnet_platform_primitives_unsetenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" ) == 0 );
}

static void rocketnet_platform_primitives_stop_pair( rocketnet_client_t * client, rocketnet_server_t * server )
{
    rocketnet_client_disconnect( client );
    rocketnet_server_stop( server );

    const double deadline = rocketnet_platform_time() + 10.0;
    while ( ( !rocketnet_client_disconnected( client ) || !rocketnet_server_stopped( server ) ) && rocketnet_platform_time() < deadline )
    {
        const double now = rocketnet_platform_time();
        rocketnet_client_update( client, now );
        rocketnet_server_update( server, now );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( rocketnet_client_disconnected( client ) && rocketnet_server_stopped( server ) );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
}

static void rocketnet_platform_primitives_production_filter_environment()
{
    rocketnet_platform_real_environment_t client_filter( "ROCKETNET_CLIENT_PACKET_FILTER" );
    rocketnet_platform_real_environment_t server_filter( "ROCKETNET_SERVER_PACKET_FILTER" );
    rocketnet_platform_real_environment_t client_transport( "ROCKETNET_CLIENT_TRANSPORT" );
    rocketnet_platform_real_environment_t server_transport( "ROCKETNET_SERVER_TRANSPORT" );
    client_transport.set( "portable" );
    server_transport.set( "portable" );

    const uint8_t private_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x5A, 0x5A, 0x5A, 0x5A };
    char encoded_filter[16];
    rocketnet_check( rocketnet_base64_encode_data( private_filter, sizeof( private_filter ), encoded_filter, sizeof( encoded_filter ) ) > 0 );
    client_filter.set( encoded_filter );
    server_filter.set( encoded_filter );

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t seed[32];
    memset( seed, 0x3C, sizeof( seed ) );
    rocketnet_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = UINT64_C( 0x2E50C0FFEE0A11CE );
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( backend_public ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_INVALID_ARGUMENT;

    // a fixed port: the served address is the create address, and the token
    // writer refuses a port of 0

    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:48123", NULL, &server_config, &status );
    rocketnet_check( server && status == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    rocketnet_check( client && status == ROCKETNET_STATUS_OK );

    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    snprintf( address, sizeof( address ), "127.0.0.1:48123" );     // the create address is the served address

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * addresses[] = { address };
    const time_t wall_now = time( NULL );
    rocketnet_check( wall_now != (time_t) -1 );
    rocketnet_check( rocketnet_write_production_connect_token( backend_secret, server_public, server_config.protocol_id,
                                                               addresses, 1, 0x3000, (uint64_t) wall_now, (uint64_t) wall_now + 60, 15, NULL, token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    char token_text[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_base64_encode_data( token, sizeof( token ), token_text, sizeof( token_text ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    rocketnet_check( rocketnet_client_connect( client, token_text ) == ROCKETNET_STATUS_OK );

    const double deadline = rocketnet_platform_time() + 10.0;

    while ( ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server, 0 ) ) &&
            rocketnet_platform_time() < deadline )
    {
        const double now = rocketnet_platform_time();
        rocketnet_client_update( client, now );
        rocketnet_server_update( server, now );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_client_connected( server, 0 ) );
    rocketnet_platform_primitives_stop_pair( client, server );

    const uint8_t test_filter[ROCKETNET_PACKET_FILTER_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_check( rocketnet_base64_encode_data( test_filter, sizeof( test_filter ), encoded_filter, sizeof( encoded_filter ) ) > 0 );
    client_filter.set( encoded_filter );
    server_filter.set( encoded_filter );
    memcpy( server_config.packet_filter, private_filter, sizeof( private_filter ) );
    status = ROCKETNET_STATUS_OK;
    rocketnet_check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );
    memcpy( client_config.packet_filter, private_filter, sizeof( private_filter ) );
    status = ROCKETNET_STATUS_OK;
    rocketnet_check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION );
}
#endif // #if !ROCKETNET_DEV

struct rocketnet_platform_primitives_priority_thread_t
{
    rocketnet_platform_wake_t * wake;
    bool signaled;
};

static void rocketnet_platform_primitives_priority_thread( void * context )
{
    rocketnet_platform_primitives_priority_thread_t * state =
        (rocketnet_platform_primitives_priority_thread_t *) context;
    state->signaled = rocketnet_platform_wake_wait( state->wake, false, 30.0 );
}

static void rocketnet_platform_primitives_priority()
{
    rocketnet_platform_primitives_priority_thread_t state = { rocketnet_platform_wake_create( NULL, NULL ), false };
    rocketnet_check( state.wake );

    rocketnet_platform_thread_t * thread = rocketnet_platform_thread_create( NULL, rocketnet_platform_primitives_priority_thread, &state );
    rocketnet_check( thread );

    const uint64_t refusals = rocketnet_platform_thread_priority_refusals();

    rocketnet_platform_client_thread_priority( thread );
    rocketnet_platform_server_thread_priority( thread );

    const uint64_t after_priority = rocketnet_platform_thread_priority_refusals();
    rocketnet_check( after_priority >= refusals && after_priority - refusals <= 2 );
    rocketnet_platform_wake_set( state.wake );
    rocketnet_platform_thread_join( thread );
    rocketnet_platform_thread_destroy( thread );
    rocketnet_platform_wake_destroy( state.wake );
    rocketnet_check( state.signaled );
}

int main()
{
    const bool priority_enabled = rocketnet_platform_thread_priority_enabled();

    rocketnet_thread_priority( true );
    rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_platform_primitives_environment();
    rocketnet_platform_primitives_public_config_environment();
    rocketnet_platform_primitives_key_warning_environment();
#if !ROCKETNET_DEV
    rocketnet_platform_primitives_production_status_environment();
    rocketnet_platform_primitives_production_simulator_environment();
    rocketnet_platform_primitives_production_filter_environment();
#endif // #if !ROCKETNET_DEV

#if ROCKETNET_DEV
    rocketnet_address_t address;
    memset( &address, 0, sizeof( address ) );
    rocketnet_check( rocketnet_platform_hostname_resolve( "localhost", "40000", &address ) );
    rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 || address.type == ROCKETNET_ADDRESS_IPV6 );
    rocketnet_check( address.port == 40000 );
#endif // #if ROCKETNET_DEV

    const bool ipv6_available = rocketnet_platform_client_ipv6_available();

    rocketnet_test_platform_thread();
    rocketnet_platform_primitives_priority();

    int status = ROCKETNET_STATUS_OK;
    rocketnet_check( rocketnet_frame_sync_create_ex( NULL, 0.0, &status ) == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_INVALID_ARGUMENT );
#if ROCKETNET_DEV
    rocketnet_test_frame_sync_high_resolution_timer_required();
#endif // #if ROCKETNET_DEV
    rocketnet_test_frame_sync_wait();

    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_thread_priority( priority_enabled );
    printf( "platform primitives OK: DEV=%d, real environment/lookup/thread/frame sync, IPv6=%d\n",
            ROCKETNET_DEV, ipv6_available ? 1 : 0 );
    return 0;
}

#endif // #if defined(ROCKETNET_PLATFORM_PRIMITIVES_STANDALONE)
