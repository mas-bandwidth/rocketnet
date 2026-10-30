/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Shared by the functional suite only. The unit suite does not include this.
    These tests open sockets or start a real transport. The same rules, where a
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
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_XDP
#include "rocketnet_server_socket.h"
#endif // #if ROCKETNET_XDP

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_gso.h"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

#include "../portable_crt.h"

#if !ROCKETNET_DEV
#include "rocketnet_base64.h"
#include <sodium.h>
// Production functional shim: test-owned production keys (never the published
// development keys), minted only with the production token writer. The
// production library under test supplies every transport; this code runs in
// the test executable only and is compiled only for functional_production.
static const uint64_t rocketnet_functional_production_protocol_id = UINT64_C( 0x50524F4455435401 );
static void rocketnet_functional_production_keys( uint8_t * backend_public, uint8_t * backend_secret, uint8_t * server_public, uint8_t * server_secret )
{
    uint8_t seed[32];

    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x50 + i );
    }

    if ( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) != ROCKETNET_STATUS_OK )
    {
        exit( 1 );
    }

    for ( int i = 0; i < 32; i++ )
    {
        server_secret[i] = (uint8_t) ( 0x70 + i );
    }

    if ( crypto_scalarmult_base( server_public, server_secret ) != 0 )
    {
        exit( 1 );
    }
}
const uint8_t rocketnet_functional_production_backend_secret_key[64] = { 0 };
void rocketnet_functional_production_server_config( struct rocketnet_server_transport_config_t * config )
{
    if ( !config )
        return;

    uint8_t backend_public[32], backend_secret[64], server_public[32], server_secret[32];
    rocketnet_functional_production_keys( backend_public, backend_secret, server_public, server_secret );
    rocketnet_server_transport_default_config_current( config );
    config->protocol_id = rocketnet_functional_production_protocol_id;
    memcpy( config->backend_public_key, backend_public, 32 );
    memcpy( config->server_public_key, server_public, 32 );
    memcpy( config->server_secret_key, server_secret, 32 );

    static const uint8_t production_filter[4] = { 0x51, 0x52, 0x53, 0x54 };
    memcpy( config->packet_filter, production_filter, sizeof( production_filter ) );
}
enum rocketnet_status_t rocketnet_functional_production_write_token( const char * server_address_string, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes )
{
    uint8_t backend_public[32], backend_secret[64], server_public[32], server_secret[32];
    rocketnet_functional_production_keys( backend_public, backend_secret, server_public, server_secret );

    const char * addresses[1] = { server_address_string };
    return rocketnet_write_production_connect_token( backend_secret, server_public, rocketnet_functional_production_protocol_id, addresses, 1, client_id, current_timestamp, current_timestamp + 30, 10, NULL, connect_token, connect_token_bytes );
}
enum rocketnet_status_t rocketnet_functional_production_create_token( const char * server_address_ipv4, const char * server_address_ipv6, const uint8_t * server_public_key, int expire_seconds, int timeout_seconds, uint64_t client_id, uint64_t protocol_id, uint64_t unix_timestamp, const uint8_t * user_data, char * connect_token )
{
    uint8_t backend_public[32], backend_secret[64], server_public[32], server_secret[32];
    rocketnet_functional_production_keys( backend_public, backend_secret, server_public, server_secret );

    const char * addresses[2];
    int count = 0;

    if ( server_address_ipv4 )
    {
        addresses[count++] = server_address_ipv4;
    }

    if ( server_address_ipv6 )
    {
        addresses[count++] = server_address_ipv6;
    }

    if ( count == 0 || !server_public_key || !connect_token )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    enum rocketnet_status_t status = rocketnet_write_production_connect_token( backend_secret, server_public_key, protocol_id, addresses, count, client_id, unix_timestamp, unix_timestamp + (uint64_t) expire_seconds, timeout_seconds, user_data, token, sizeof( token ) );
    if ( status != ROCKETNET_STATUS_OK )
        return status;

    return rocketnet_base64_encode_data( token, sizeof( token ), connect_token, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) > 0 ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_INVALID_ARGUMENT;
}
#endif // #if !ROCKETNET_DEV

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

static int setenv( const char * name, const char * value, int overwrite )
{
    size_t required = 0;
    if ( !overwrite && getenv_s( &required, NULL, 0, name ) == 0 && required > 0 )
        return 0;
    return _putenv_s( name, value );
}

static int unsetenv( const char * name ) { return _putenv_s( name, "" ); }

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

// The unix timestamp a functional case stamps on a connect token, from the
// clock that judges the token. The server transport reads token_epoch_unix_seconds
// plus its update time when the epoch is set, else the wall clock
// (sdk/rocketnet_server_transport.cpp:1711), and the client transport reads
// its own config's epoch the same way (sdk/rocketnet_client_transport.cpp:762).
// A case passes the epoch it configures and the time it drives the server
// with; an epoch of zero returns the wall clock both ends then read.
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time )
{
    return token_epoch_unix_seconds != 0 ? token_epoch_unix_seconds + (uint64_t) time : ( uint64_t )::time( NULL );
}

#define ROCKETNET_FUNCTIONAL_LOG_RING_LINES 32
#define ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES 1024
// a slot holds "<time>: <level>: " in front of a whole captured line: the
// level name is at most 7 bytes (sdk/rocketnet.cpp:179), so 64 bytes hold
// the prefix for any clock reading below 10^40 seconds
#define ROCKETNET_FUNCTIONAL_LOG_RING_PREFIX_BYTES 64
#define ROCKETNET_FUNCTIONAL_LOG_RING_SLOT_BYTES ( ROCKETNET_FUNCTIONAL_LOG_RING_PREFIX_BYTES + ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES )

static char rocketnet_test_log_ring[ROCKETNET_FUNCTIONAL_LOG_RING_LINES][ROCKETNET_FUNCTIONAL_LOG_RING_SLOT_BYTES];
static int rocketnet_test_log_ring_lines;
static rocketnet_log_function_t rocketnet_test_suite_log_function = NULL;
static char rocketnet_test_captured_log[16384];

static void rocketnet_test_log_capture( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;

    char line[ROCKETNET_FUNCTIONAL_LOG_RING_LINE_BYTES];
    const int line_length = snprintf( line, sizeof( line ), "%s", message );
    if ( line_length < 0 || (size_t) line_length >= sizeof( line ) )
        return;

    if ( level == ROCKETNET_LOG_LEVEL_NONE )
    {
        printf( "%s\n", line );
        fflush( stdout );
        return;
    }

    char * slot = rocketnet_test_log_ring[rocketnet_test_log_ring_lines % ROCKETNET_FUNCTIONAL_LOG_RING_LINES];
    const int slot_length = snprintf( slot, ROCKETNET_FUNCTIONAL_LOG_RING_SLOT_BYTES, "%.6f: %s: %s", rocketnet_platform_time(), rocketnet_log_level_name( level ), line );
    if ( slot_length < 0 || (size_t) slot_length >= ROCKETNET_FUNCTIONAL_LOG_RING_SLOT_BYTES )
        return;
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
    {
        printf( "%s\n", rocketnet_test_log_ring[( first + i ) % ROCKETNET_FUNCTIONAL_LOG_RING_LINES] );
    }

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
    guard->was_set = rocketnet_test_getenv( name, guard->saved, sizeof( guard->saved ) ) != 0;
}

static void rocketnet_test_env_clear( struct rocketnet_test_env_guard_t * guard, const char * name )
{
    rocketnet_test_env_save( guard, name );
    unsetenv( name );
}

static void rocketnet_test_env_restore( struct rocketnet_test_env_guard_t * guard )
{
    if ( guard->was_set )
    {
        setenv( guard->name, guard->saved, 1 );
    }
    else
    {
        unsetenv( guard->name );
    }
}

static void rocketnet_test_capture_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    (void) level;

    char line[1024];
    snprintf( line, sizeof( line ), "%s", message );

    const size_t used = strlen( rocketnet_test_captured_log );
    snprintf( rocketnet_test_captured_log + used, sizeof( rocketnet_test_captured_log ) - used, "%s\n", line );
}

class rocketnet_test_environment_t
{
    struct entry_t
    {
        std::string name, value;
        bool present;
    };
    std::vector<entry_t> entries;
    void clear( const char * name )
    {
        if ( strcmp( name, "ROCKETNET_SERVER_TRANSPORT" ) == 0 )
            return;
        if ( strcmp( name, "ROCKETNET_CLIENT_TRANSPORT" ) == 0 )
            return;
        for ( const auto & entry : entries )
            if ( entry.name == name )
                return;

        char value[4096];
        const bool present = rocketnet_test_getenv( name, value, sizeof( value ) ) != 0;
        entries.push_back( { name, value, present } );
        rocketnet_check( unsetenv( name ) == 0 );
    }

public:
    rocketnet_test_environment_t()
    {
        for ( int side = 0; side < 2; ++side )
        {
            int count = 0;
            const auto * fields = side ? rocketnet_server_transport_config_fields( &count )
                                       : rocketnet_client_transport_config_fields( &count );
            for ( int i = 0; i < count; ++i )
            {
                char name[128];
                rocketnet_check( rocketnet_config_env_var_name_for( side ? "server" : "client", fields[i].name, name, sizeof( name ) ) );
                clear( name );
            }
        }

        clear( "ROCKETNET_SERVER_IPV4_ADDRESS" );
        clear( "ROCKETNET_SERVER_IPV6_ADDRESS" );
        clear( "ROCKETNET_SERVER_NUM_OS_CPUS" );
        clear( "ROCKETNET_GATEWAY_ETHERNET_ADDRESS" );
    }
    ~rocketnet_test_environment_t()
    {
        for ( const auto & entry : entries )
            rocketnet_check( ( entry.present ? setenv( entry.name.c_str(), entry.value.c_str(), 1 )
                                             : unsetenv( entry.name.c_str() ) ) == 0 );
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
// The grant is retried up to three times on address-in-use: the same port must
// still be free a moment later, when the caller's server binds it, so a second
// bind proves the port before it is handed out. If another process takes it in
// the gap, the proof refuses with address-in-use and another port is chosen.

uint16_t rocketnet_test_free_port()
{
    static const int kFreePortTries = 3;
    uint16_t port = 0;
    for ( int attempt = 0; attempt < kFreePortTries; ++attempt )
    {
        rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );

        struct rocketnet_platform_socket_t * scratch = rocketnet_platform_socket_create( NULL, &address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 65536, 65536 );
        rocketnet_check( scratch );
        rocketnet_check( address.port != 0 );

        char probe_address[64];
        snprintf( probe_address, sizeof( probe_address ), "127.0.0.1:%d", (int) address.port );

        rocketnet_address_t probe;
        rocketnet_check( rocketnet_address_parse( &probe, probe_address ) );

        rocketnet_platform_socket_destroy( scratch );

        struct rocketnet_platform_socket_t * held = rocketnet_platform_socket_create( NULL, &probe, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, 65536, 65536 );
        if ( held )
        {
            port = address.port;
            rocketnet_platform_socket_destroy( held );
            break;
        }
    }

    rocketnet_check( port != 0 );
    return port;
}

// "127.0.0.1:<port>" with a port from rocketnet_test_free_port

void rocketnet_test_free_loopback_address( char * buffer, size_t buffer_size )
{
    snprintf( buffer, buffer_size, "127.0.0.1:%d", (int) rocketnet_test_free_port() );
}

// Create a server transport on a free loopback port. The free-port grant and
// the create are one retried operation: the grant proves the port with a
// second bind, but another process can take it in the gap before this create
// binds it, so a create refused on address-in-use is retried with a new free
// port, up to three times. Any other refusal is final and returned as NULL.

struct rocketnet_server_transport_t * rocketnet_test_server_transport_create_on_free_port(
    const struct rocketnet_server_transport_config_t * config )
{
    static const int kServerCreateTries = 3;
    for ( int attempt = 0; attempt < kServerCreateTries; ++attempt )
    {
        char address[64];
        rocketnet_test_free_loopback_address( address, sizeof( address ) );

        int status = ROCKETNET_STATUS_OK;

        struct rocketnet_server_transport_t * server =
            rocketnet_server_transport_create_ex( NULL, address, config, rocketnet_platform_time(), &status );
        if ( server )
            return server;
        if ( status != ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED )
            return NULL;
    }

    return NULL;
}
