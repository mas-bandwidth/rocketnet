/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Shared by the functional suite only. The unit suite does not include this.
    These tests open sockets or start a real driver. The same rules, where a
    state machine or the in-memory simulator can reach them, stay in
    sdk/rocketnet_tests.cpp.
*/

#include "rocketnet.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_protocol.h"

#include <climits>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_server_iocp.h"
#endif

#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_gso.h"
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static int setenv( const char * name, const char * value, int overwrite )
{
    if ( !overwrite && getenv( name ) != NULL )
        return 0;
    return _putenv_s( name, value );
}

static int unsetenv( const char * name ) { return _putenv_s( name, "" ); }

#endif

#define ROCKETNET_FUNCTIONAL_LOG_RING_LINES 32
#define ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES 1024

static char rocketnet_test_log_ring[ROCKETNET_FUNCTIONAL_LOG_RING_LINES][ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES];
static int rocketnet_test_log_ring_lines;
static void (*rocketnet_test_suite_log_function)( int level, const char * format, ... ) = NULL;
static char rocketnet_test_captured_log[16384];

static void rocketnet_test_log_capture( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    char line[ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES];
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );

    if ( level == ROCKETNET_LOG_LEVEL_NONE )
    {
        printf( "%s\n", line );
        fflush( stdout );
        return;
    }

    char * slot = rocketnet_test_log_ring[rocketnet_test_log_ring_lines % ROCKETNET_FUNCTIONAL_LOG_RING_LINES];
    snprintf( slot, ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES, "%.6f: %s: %s", rocketnet_platform_time(), rocketnet_log_level_string( level ), line );
    rocketnet_test_log_ring_lines++;
}

static void rocketnet_test_log_ring_clear()
{
    rocketnet_test_log_ring_lines = 0;
}

static void rocketnet_test_log_ring_dump()
{
    const int lines = rocketnet_test_log_ring_lines < ROCKETNET_FUNCTIONAL_LOG_RING_LINES ? rocketnet_test_log_ring_lines : ROCKETNET_FUNCTIONAL_LOG_RING_LINES;
    const int first = rocketnet_test_log_ring_lines - lines;
    for ( int i = 0; i < lines; i++ )
        printf( "%s\n", rocketnet_test_log_ring[( first + i ) % ROCKETNET_FUNCTIONAL_LOG_RING_LINES] );
    rocketnet_test_log_ring_lines = 0;
}

static void rocketnet_check_handler( const char * condition, const char * function, const char * file, int line )
{
    rocketnet_test_log_ring_dump();
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );         \
    }                                                                                                           \
} while(0)

struct rocketnet_test_env_guard_t
{
    const char * name;
    bool was_set;
    char saved[1024];
};

static void rocketnet_test_env_save( struct rocketnet_test_env_guard_t * guard, const char * name )
{
    guard->name = name;
    const char * value = getenv( name );
    guard->was_set = value != NULL;
    guard->saved[0] = '\0';
    if ( value )
        snprintf( guard->saved, sizeof(guard->saved), "%s", value );
}

static void rocketnet_test_env_clear( struct rocketnet_test_env_guard_t * guard, const char * name )
{
    rocketnet_test_env_save( guard, name );
    unsetenv( name );
}

static void rocketnet_test_env_restore( struct rocketnet_test_env_guard_t * guard )
{
    if ( guard->was_set )
        setenv( guard->name, guard->saved, 1 );
    else
        unsetenv( guard->name );
}

static void rocketnet_test_capture_log( int level, const char * format, ... )
{
    (void) level;
    va_list args;
    va_start( args, format );
    char line[1024];
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    const size_t used = strlen( rocketnet_test_captured_log );
    snprintf( rocketnet_test_captured_log + used, sizeof(rocketnet_test_captured_log) - used, "%s\n", line );
}

class rocketnet_test_environment_t
{
    struct entry_t { std::string name, value; bool present; };
    std::vector<entry_t> entries;
    void clear(const char * name)
    {
        if (strcmp(name, "ROCKETNET_DRIVER") == 0) return;
        for (const auto & entry : entries) if (entry.name == name) return;
        const char * value = getenv(name);
        entries.push_back({name, value ? value : "", value != NULL});
        rocketnet_check(unsetenv(name) == 0);
    }
public:
    rocketnet_test_environment_t()
    {
        for (int side = 0; side < 2; ++side)
        {
            int count = 0;
            const auto * fields = side ? rocketnet_server_transport_config_fields(&count)
                                       : rocketnet_client_transport_config_fields(&count);
            for (int i = 0; i < count; ++i)
            {
                char name[128];
                rocketnet_check(rocketnet_config_env_var_name(fields[i].name, name, sizeof(name)));
                clear(name);
            }
        }
        clear("ROCKETNET_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_NUM_QUEUES");
        clear("ROCKETNET_SERVER_NUM_OS_CPUS");
        clear("ROCKETNET_GATEWAY_ETHERNET_ADDRESS");
        clear("ROCKETNET_PUMP");
    }
    ~rocketnet_test_environment_t()
    {
        for (const auto & entry : entries)
            rocketnet_check((entry.present ? setenv(entry.name.c_str(), entry.value.c_str(), 1)
                                           : unsetenv(entry.name.c_str())) == 0);
    }
};

// The create half of config validation: an out-of-range env value refuses
// the whole create. The struct checks stay in the unit suite.

void rocketnet_test_config_out_of_range_refuses_create()
{
    struct rocketnet_test_env_guard_t guard;
    rocketnet_test_env_save( &guard, "ROCKETNET_MAX_CLIENTS" );
    setenv( "ROCKETNET_MAX_CLIENTS", "0", 1 );
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config, rocketnet_platform_time() ) == NULL );
    rocketnet_test_env_restore( &guard );
}
