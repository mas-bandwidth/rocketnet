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

#include "../tests/portable_crt.h"

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
    size_t required = 0;
    if ( !overwrite && getenv_s( &required, NULL, 0, name ) == 0 && required > 0 )
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

// The key set a functional case connects with. A development build
// (ROCKETNET_DEV=1, ./bin/functional) uses the published development keys,
// the test packet filter and the layer tests' token, unchanged. A production
// build (ROCKETNET_DEV=0, ./bin/functional_production) refuses all three at
// create, so it uses a key set of its own, made the way a game's backend
// makes one: a backend keypair from a fixed seed, a server keypair generated
// once per process, a packet filter of its own on both ends, and tokens
// minted by the production token writer with the development token's expiry
// and timeout. The cases themselves are the same source in both builds.

#if ROCKETNET_DEV

void rocketnet_functional_server_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_dev_config( config );
}

void rocketnet_functional_client_default_config( struct rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
}

const struct rocketnet_client_transport_config_t * rocketnet_functional_client_config()
{
    return NULL;
}

enum rocketnet_status_t rocketnet_functional_write_token( const char * server_address, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes )
{
    return rocketnet_write_test_connect_token( server_address, client_id, current_timestamp, connect_token, connect_token_bytes );
}

#else // #if ROCKETNET_DEV

static const uint64_t rocketnet_functional_protocol_id = 0x46554E4350524F44ULL;
static const uint8_t rocketnet_functional_packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x46, 0x55, 0x4E, 0x43 };

struct rocketnet_functional_keys_t
{
    bool ready;
    uint8_t backend_public[32];
    uint8_t backend_secret[64];
    uint8_t server_public[32];
    uint8_t server_secret[32];
};

static struct rocketnet_functional_keys_t rocketnet_functional_keys;

static const struct rocketnet_functional_keys_t * rocketnet_functional_key_set()
{
    if ( !rocketnet_functional_keys.ready )
    {
        uint8_t seed[32];
        for ( int i = 0; i < 32; ++i )
            seed[i] = (uint8_t) ( 0x5A ^ i );
        if ( rocketnet_generate_backend_keypair_from_seed( seed, rocketnet_functional_keys.backend_public, rocketnet_functional_keys.backend_secret ) != ROCKETNET_STATUS_OK ||
             rocketnet_generate_server_keypair( rocketnet_functional_keys.server_public, rocketnet_functional_keys.server_secret ) != ROCKETNET_STATUS_OK )
        {
            printf( "error: the functional key set could not be generated\n" );
            fflush( stdout );
            exit( 1 );
        }
        rocketnet_functional_keys.ready = true;
    }
    return &rocketnet_functional_keys;
}

void rocketnet_functional_server_config( struct rocketnet_server_transport_config_t * config )
{
    const struct rocketnet_functional_keys_t * keys = rocketnet_functional_key_set();
    rocketnet_server_transport_default_config_current( config );
    config->protocol_id = rocketnet_functional_protocol_id;
    memcpy( config->packet_filter, rocketnet_functional_packet_filter, sizeof( config->packet_filter ) );
    memcpy( config->backend_public_key, keys->backend_public, 32 );
    memcpy( config->server_public_key, keys->server_public, 32 );
    memcpy( config->server_secret_key, keys->server_secret, 32 );
}

void rocketnet_functional_client_default_config( struct rocketnet_client_transport_config_t * config )
{
    rocketnet_client_transport_default_config( config );
    memcpy( config->packet_filter, rocketnet_functional_packet_filter, sizeof( config->packet_filter ) );
}

const struct rocketnet_client_transport_config_t * rocketnet_functional_client_config()
{
    // the whole current struct, as a NULL config gives the development build
    static struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    memcpy( config.packet_filter, rocketnet_functional_packet_filter, sizeof( config.packet_filter ) );
    return &config;
}

enum rocketnet_status_t rocketnet_functional_write_token( const char * server_address, uint64_t client_id, uint64_t current_timestamp, uint8_t * connect_token, size_t connect_token_bytes )
{
    const struct rocketnet_functional_keys_t * keys = rocketnet_functional_key_set();
    const char * addresses[] = { server_address };
    return rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public, rocketnet_functional_protocol_id,
                                                     addresses, 1, client_id, current_timestamp,
                                                     current_timestamp + ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                                                     ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, NULL,
                                                     connect_token, connect_token_bytes );
}

#endif // #if ROCKETNET_DEV

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
    const int line_length = snprintf( line, sizeof(line), "%s", message );
    if ( line_length < 0 || (size_t) line_length >= sizeof(line) )
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
    guard->was_set = rocketnet_test_getenv( name, guard->saved, sizeof(guard->saved) ) != 0;
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
        char value[4096];
        const bool present = rocketnet_test_getenv(name, value, sizeof(value)) != 0;
        entries.push_back({name, value, present});
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
        clear("ROCKETNET_SERVER_IPV4_ADDRESS");
        clear("ROCKETNET_SERVER_IPV6_ADDRESS");
        clear("ROCKETNET_SERVER_NUM_OS_CPUS");
        clear("ROCKETNET_GATEWAY_ETHERNET_ADDRESS");
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
    rocketnet_functional_server_config( &server_config );
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
