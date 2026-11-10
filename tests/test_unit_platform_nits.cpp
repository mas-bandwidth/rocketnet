/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The platform-layer nits a unit case shows with no socket, no thread and no
// clock: the non-blocking flag merge the POSIX socket create paths pass to
// fcntl F_SETFL (rocketnet_platform_nonblocking_flags in
// sdk/rocketnet_platform.cpp, called by both POSIX socket create paths), which
// keeps the descriptor's other flags; and the platform mutex, which is not
// recursive on any platform.

#include "test_unit_registry.h"
#include "resource_file.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"

#include <string>

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

static bool platform_nits_read_source( const char * relative_path, std::string & contents )
{
    std::string path = __FILE__;
    const char * markers[] = { "tests/test_unit_platform_nits.cpp", "tests\\test_unit_platform_nits.cpp" };
    size_t at = std::string::npos;
    for ( size_t i = 0; i < sizeof( markers ) / sizeof( markers[0] ); i++ )
    {
        at = path.find( markers[i] );
        if ( at != std::string::npos )
            break;
    }
    if ( at == std::string::npos )
        return false;

    path.resize( at );
    path += relative_path;

    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
        return false;

    char buffer[4096];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        contents.append( buffer, n );
    fclose( file );

    return true;
}

static std::string platform_nits_function_body( const std::string & text, const char * signature )
{
    const size_t begin = text.find( signature );
    if ( begin == std::string::npos )
        return std::string();

    const size_t end = text.find( "\n}", begin );
    if ( end == std::string::npos )
        return std::string();

    return text.substr( begin, end - begin );
}

static void test_platform_msvc_relaxed_u64_load_is_not_a_locked_rmw()
{
    std::string header;
    rocketnet_unit_check( platform_nits_read_source( "sdk/rocketnet_platform.h", header ) );

    const std::string load = platform_nits_function_body( header, "inline uint64_t rocketnet_platform_atomic_load_u64" );
    rocketnet_unit_check( !load.empty() );

    const size_t w64 = load.find( "#elif defined( _M_X64 ) || defined( _M_ARM64 )" );
    rocketnet_unit_check( w64 != std::string::npos );
    const size_t w64_end = load.find( "#else", w64 );
    rocketnet_unit_check( w64_end != std::string::npos );

    const std::string w64_body = load.substr( w64, w64_end - w64 );
    rocketnet_unit_check( w64_body.find( "__iso_volatile_load64" ) != std::string::npos );
    rocketnet_unit_check( w64_body.find( "_InterlockedCompareExchange64" ) == std::string::npos );

    std::string cpp;
    rocketnet_unit_check( platform_nits_read_source( "sdk/rocketnet_platform.cpp", cpp ) );

    const std::string resets = platform_nits_function_body( cpp, "uint64_t rocketnet_platform_receive_resets()" );
    rocketnet_unit_check( !resets.empty() );
    rocketnet_unit_check( resets.find( "rocketnet_platform_atomic_load_u64" ) != std::string::npos );
    rocketnet_unit_check( resets.find( "InterlockedCompareExchange64" ) == std::string::npos );
}

REGISTER_UNIT_TEST( test_platform_msvc_relaxed_u64_load_is_not_a_locked_rmw );

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
