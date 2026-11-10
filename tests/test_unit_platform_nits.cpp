/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The platform-layer nits a unit case shows with no socket and no clock: the
// non-blocking flag merge the POSIX socket create paths pass to fcntl F_SETFL
// (rocketnet_platform_nonblocking_flags in sdk/rocketnet_platform.cpp, called
// by both POSIX socket create paths), which keeps the descriptor's other
// flags; the platform mutex, which is not recursive on any platform; and the
// spin lock's acquire loop, which must pause each spin and yield after a
// bounded number so a preempted holder is not starved.

#include "test_unit_registry.h"
#include "resource_file.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#include <fcntl.h>

int rocketnet_platform_nonblocking_flags( int flags );

// F_SETFL replaces the status flags, so the merge keeps every flag the
// descriptor had and adds O_NONBLOCK.
static void test_platform_nonblocking_flags_keep_existing_flags()
{
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_APPEND ) == ( O_APPEND | O_NONBLOCK ) );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( 0 ) == O_NONBLOCK );
    rocketnet_unit_check( rocketnet_platform_nonblocking_flags( O_NONBLOCK ) == O_NONBLOCK );
}

REGISTER_UNIT_TEST( test_platform_nonblocking_flags_keep_existing_flags );

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_DEV

// The mutex is not recursive on any platform (PTHREAD_MUTEX_NORMAL on POSIX,
// an SRWLOCK on Windows), so the thread that holds it cannot take it again.
static void test_platform_mutex_try_acquire_fails_for_the_owner()
{
    struct rocketnet_platform_mutex_t mutex;
    rocketnet_unit_check( rocketnet_platform_mutex_create( &mutex ) );

    rocketnet_unit_check( rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_unit_check( !rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_platform_mutex_acquire( &mutex );
    rocketnet_unit_check( !rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_unit_check( rocketnet_platform_mutex_try_acquire( &mutex ) );
    rocketnet_platform_mutex_release( &mutex );

    rocketnet_platform_mutex_destroy( &mutex );
}

REGISTER_UNIT_TEST( test_platform_mutex_try_acquire_fails_for_the_owner );

#endif // #if ROCKETNET_DEV

// The spin lock guards the sent-history ring. Under contention every waiter's
// increment lands exactly once, and the acquire loop waits with a CPU pause on
// each spin and a thread yield after a bounded number of them, so a holder
// that was preempted gets the CPU back instead of the waiter burning its whole
// scheduling quantum. The source shape is checked because the pause and yield
// are timing behaviour a portable unit case cannot observe; the contention run
// above them proves the same lock still excludes. Reads one repository file,
// starts no socket and reads no clock.

static std::string platform_nits_read_repo_file( const char * relative )
{
    std::string file = __FILE__;
    for ( size_t i = 0; i < file.size(); i++ )
    {
        if ( file[i] == '\\' )
            file[i] = '/';
    }

    const std::string suffix = "tests/test_unit_platform_nits.cpp";
    const size_t at = file.rfind( suffix );
    if ( at == std::string::npos )
        return std::string();

    const std::string path = file.substr( 0, at ) + relative;
    FILE * handle = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( !handle )
        return std::string();

    std::string text;
    char buffer[4096];
    size_t got;
    while ( ( got = fread( buffer, 1, sizeof( buffer ), handle ) ) > 0 )
        text.append( buffer, got );
    fclose( handle );

    return text;
}

static void test_platform_spin_lock_acquire_release_under_contention()
{
    enum
    {
        THREADS = 4,
        ITERATIONS = 20000
    };

    volatile uint64_t lock = 0;
    uint64_t guarded = 0;               // ordered by the lock alone, not atomic
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };

    std::vector<std::thread> threads;
    for ( int i = 0; i < THREADS; i++ )
    {
        threads.push_back( std::thread( [&lock, &guarded, &ready, &go]()
                                        {
            ready.fetch_add( 1, std::memory_order_release );
            while ( !go.load( std::memory_order_acquire ) )
            {
            }

            for ( int n = 0; n < ITERATIONS; n++ )
            {
                rocketnet_platform_spin_lock_u64( &lock );
                guarded++;
                rocketnet_platform_spin_unlock_u64( &lock );
            } } ) );
    }

    while ( ready.load( std::memory_order_acquire ) != THREADS )
    {
    }
    go.store( true, std::memory_order_release );

    for ( size_t i = 0; i < threads.size(); i++ )
        threads[i].join();

    rocketnet_unit_check( guarded == (uint64_t) THREADS * ITERATIONS );
    rocketnet_unit_check( lock == 0 );

    const std::string header = platform_nits_read_repo_file( "sdk/rocketnet_platform.h" );
    rocketnet_unit_check( !header.empty() );

    const std::string lock_name = "inline void rocketnet_platform_spin_lock_u64";
    const std::string unlock_name = "inline void rocketnet_platform_spin_unlock_u64";
    const size_t lock_at = header.find( lock_name );
    const size_t unlock_at = header.find( unlock_name, lock_at );
    rocketnet_unit_check( lock_at != std::string::npos );
    rocketnet_unit_check( unlock_at != std::string::npos );

    const std::string acquire = header.substr( lock_at, unlock_at - lock_at );
    rocketnet_unit_check( acquire.find( "ROCKETNET_PLATFORM_SPIN_YIELD_AFTER" ) != std::string::npos );
    rocketnet_unit_check( acquire.find( "_mm_pause" ) != std::string::npos );
    rocketnet_unit_check( acquire.find( "__builtin_ia32_pause" ) != std::string::npos );
    rocketnet_unit_check( acquire.find( "sched_yield" ) != std::string::npos );
    rocketnet_unit_check( acquire.find( "SwitchToThread" ) != std::string::npos );

    // the MSVC unlock releases the word; it is not another compare-exchange
    const std::string release = header.substr( unlock_at );
    rocketnet_unit_check( release.find( "_InterlockedExchange64" ) != std::string::npos );
    rocketnet_unit_check( release.find( "_InterlockedCompareExchange64" ) == std::string::npos );
}

REGISTER_UNIT_TEST( test_platform_spin_lock_acquire_release_under_contention );
