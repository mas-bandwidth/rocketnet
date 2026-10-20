/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Shared by the functional suite only. The unit suite does not include this.
    These tests open sockets or start a real driver. The same rules, where a
    state machine or the in-memory simulator can reach them, stay in
    sdk/rocketnet_tests.cpp.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"

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

// The unix timestamp a functional case stamps on a connect token, from the
// clock that judges the token. The server transport reads game_clock_epoch
// plus its update time when the epoch is set, else the wall clock
// (sdk/rocketnet_server_transport.cpp:1711), and the client transport reads
// its own config's epoch the same way (sdk/rocketnet_client_transport.cpp:762).
// A case passes the epoch it configures and the time it drives the server
// with; an epoch of zero returns the wall clock both ends then read.
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time )
{
    return game_clock_epoch != 0 ? game_clock_epoch + (uint64_t) time : (uint64_t) ::time( NULL );
}

#define ROCKETNET_FUNCTIONAL_LOG_RING_LINES 32
#define ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES 1024

static char rocketnet_test_log_ring[ROCKETNET_FUNCTIONAL_LOG_RING_LINES][ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES];
static int rocketnet_test_log_ring_lines;
static rocketnet_log_callback_t rocketnet_test_suite_log_function = NULL;
static char rocketnet_test_captured_log[16384];

static void rocketnet_test_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    char line[ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES];
    snprintf( line, sizeof(line), "%s", message );

    if ( level == ROCKETNET_LOG_LEVEL_NONE )
    {
        printf( "%s\n", line );
        fflush( stdout );
        return;
    }

    char * slot = rocketnet_test_log_ring[rocketnet_test_log_ring_lines % ROCKETNET_FUNCTIONAL_LOG_RING_LINES];
    snprintf( slot, ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES, "%.6f: %s: %.1000s", rocketnet_platform_time(), rocketnet_log_level_name( level ), line );
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

static void rocketnet_test_capture_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    (void) level;
    char line[1024];
    snprintf( line, sizeof(line), "%s", message );
    const size_t used = strlen( rocketnet_test_captured_log );
    snprintf( rocketnet_test_captured_log + used, sizeof(rocketnet_test_captured_log) - used, "%s\n", line );
}

class rocketnet_test_environment_t
{
    struct entry_t { std::string name, value; bool present; };
    std::vector<entry_t> entries;
    void clear(const char * name)
    {
        if (strcmp(name, "ROCKETNET_SERVER_TRANSPORT") == 0) return;
        if (strcmp(name, "ROCKETNET_CLIENT_TRANSPORT") == 0) return;
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
                rocketnet_check(rocketnet_config_env_var_name_for(side ? "server" : "client", fields[i].name, name, sizeof(name)));
                clear(name);
            }
        }
        clear("ROCKETNET_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_PUBLIC_ADDRESS");
        clear("ROCKETNET_SERVER_NUM_OS_CPUS");
        clear("ROCKETNET_GATEWAY_ETHERNET_ADDRESS");
        clear("ROCKETNET_PUMP");
        clear("ROCKETNET_DRIVER");
        clear("ROCKETNET_TRANSPORT");
        clear("ROCKETNET_SERVER_DRIVER");
        clear("ROCKETNET_CLIENT_DRIVER");
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
    rocketnet_test_env_save( &guard, "ROCKETNET_SERVER_MAX_CLIENTS" );
    setenv( "ROCKETNET_SERVER_MAX_CLIENTS", "0", 1 );
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config, rocketnet_platform_time() ) == NULL );
    rocketnet_test_env_restore( &guard );
}

// A free loopback port from the OS, for an address that must carry a real
// port (a rung that refuses port zero) without binding a fixed one: a scratch
// UDP socket binds 127.0.0.1:0, rocketnet_platform_socket_create writes the
// port the OS gave into the address, and the socket is closed before the
// port is returned. Two suites on one machine then never name the same port.

uint16_t rocketnet_test_free_port()
{
    rocketnet_address_t address;
    rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
    struct rocketnet_platform_socket_t * scratch = rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 65536, 65536 );
    rocketnet_check( scratch );
    rocketnet_check( address.port != 0 );
    rocketnet_platform_socket_destroy( scratch );
    return address.port;
}

// "127.0.0.1:<port>" with a port from rocketnet_test_free_port

void rocketnet_test_free_loopback_address( char * buffer, size_t buffer_size )
{
    snprintf( buffer, buffer_size, "127.0.0.1:%d", (int) rocketnet_test_free_port() );
}
