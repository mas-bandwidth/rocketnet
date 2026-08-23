/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.

    Stage 1 tests, in the idiom of flow's flow_tests.cpp.

    Two kinds of test live here and the distinction matters:

    MOVED-SUBSYSTEM TESTS prove the code that genuinely works: addresses
    (ported from flow's own address tests), time, hooks (allocator, log,
    assert), lifecycle, error strings, defaults, random keys.

    STUB-CONTRACT TESTS pin the stage 1 behavior of everything that is not
    implemented yet: every one of the 72 exported symbols is called at least
    once here, so linking this binary proves they all resolve, and each
    unimplemented path is checked to return the documented error for the
    situation the caller is actually in rather than crashing. When a later
    stage implements a subsystem, its stub-contract checks are the ones that
    go red — that is the mechanism working, not a nuisance.
*/

#include "rocketnet_tests.h"
#include "rocketnet.h"

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_address.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void rocketnet_check_handler( const char * condition,
                                     const char * function,
                                     const char * file,
                                     int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
#ifndef NDEBUG
    #if defined( __GNUC__ )
        __builtin_trap();
    #elif defined( _MSC_VER )
        __debugbreak();
    #endif
#endif
    exit( 1 );
}

#define rocketnet_check( condition )                                                                            \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );    \
    }                                                                                                           \
} while(0)

// ---------------------------------------------------------------
// moved subsystem: time
// ---------------------------------------------------------------

void rocketnet_test_time()
{
    double start = rocketnet_time();
    rocketnet_check( start >= 0.0 );
    rocketnet_platform_sleep( 0.1 );
    double finish = rocketnet_time();
    rocketnet_check( finish > start );
}

// ---------------------------------------------------------------
// moved subsystem: error strings — every code the header defines
// ---------------------------------------------------------------

void rocketnet_test_error_strings()
{
    const int codes[] =
    {
        ROCKETNET_OK,
        ROCKETNET_ERROR,
        ROCKETNET_ERROR_INVALID_ARGUMENT,
        ROCKETNET_ERROR_OUT_OF_MEMORY,
        ROCKETNET_ERROR_NOT_CONNECTED,
        ROCKETNET_ERROR_UNKNOWN_CONNECTION,
        ROCKETNET_ERROR_UNKNOWN_FLOW,
        ROCKETNET_ERROR_UNKNOWN_CHANNEL,
        ROCKETNET_ERROR_POOL_EXHAUSTED,
        ROCKETNET_ERROR_QUOTA_EXCEEDED,
        ROCKETNET_ERROR_QUEUE_FULL,
        ROCKETNET_ERROR_PAYLOAD_TOO_LARGE,
        ROCKETNET_ERROR_STALE_BUFFER,
        ROCKETNET_ERROR_CHANNEL_MISMATCH,
        ROCKETNET_ERROR_TOKEN_INVALID,
        ROCKETNET_ERROR_TOKEN_EXPIRED,
        ROCKETNET_ERROR_SERVER_FULL,
        ROCKETNET_ERROR_BACKEND_UNAVAILABLE,
    };

    const int num_codes = (int) ( sizeof(codes) / sizeof(codes[0]) );

    // the codes above are exactly 0 through -17 with no gaps
    rocketnet_check( num_codes == 18 );
    for ( int i = 0; i < num_codes; i++ )
    {
        rocketnet_check( codes[i] == -i );
    }

    // every defined code has real text
    for ( int i = 0; i < num_codes; i++ )
    {
        const char * string = rocketnet_error_string( codes[i] );
        rocketnet_check( string != NULL );
        rocketnet_check( strlen( string ) > 0 );
        rocketnet_check( strcmp( string, "unknown error" ) != 0 );
    }

    // every defined code's text is distinct
    for ( int i = 0; i < num_codes; i++ )
    {
        for ( int j = i + 1; j < num_codes; j++ )
        {
            rocketnet_check( strcmp( rocketnet_error_string( codes[i] ), rocketnet_error_string( codes[j] ) ) != 0 );
        }
    }

    // never null, even for values this library does not recognise
    rocketnet_check( rocketnet_error_string( -18 ) != NULL );
    rocketnet_check( rocketnet_error_string( -1000 ) != NULL );
    rocketnet_check( rocketnet_error_string( 1 ) != NULL );
    rocketnet_check( strcmp( rocketnet_error_string( -1000 ), "unknown error" ) == 0 );
}

// ---------------------------------------------------------------
// backends: strings for every value, availability honest for stage 1
// ---------------------------------------------------------------

void rocketnet_test_backend_strings()
{
    const int backends[] =
    {
        ROCKETNET_BACKEND_AUTO,
        ROCKETNET_BACKEND_SOCKET,
        ROCKETNET_BACKEND_XDP,
        ROCKETNET_BACKEND_IOCP,
        ROCKETNET_BACKEND_KQUEUE,
    };

    const int num_backends = (int) ( sizeof(backends) / sizeof(backends[0]) );

    for ( int i = 0; i < num_backends; i++ )
    {
        const char * string = rocketnet_backend_string( backends[i] );
        rocketnet_check( string != NULL );
        rocketnet_check( strcmp( string, "unknown" ) != 0 );
    }

    rocketnet_check( strcmp( rocketnet_backend_string( 100 ), "unknown" ) == 0 );
    rocketnet_check( strcmp( rocketnet_backend_string( -1 ), "unknown" ) == 0 );

    // stage 1 has no IO backends at all, so nothing is available yet. The
    // stage that lands the socket backend updates this test — going red here
    // on that day is the point.
    for ( int i = 0; i < num_backends; i++ )
    {
        rocketnet_check( rocketnet_backend_available( backends[i] ) == false );
    }
}

// ---------------------------------------------------------------
// protocol version
// ---------------------------------------------------------------

void rocketnet_test_protocol_version()
{
    uint64_t version = rocketnet_protocol_version();
    rocketnet_check( version != 0 );

    // wire compatibility follows major.minor; patch must not move it
    uint64_t expected = ( (uint64_t) ROCKETNET_VERSION_MAJOR_INT << 32 ) | ( (uint64_t) ROCKETNET_VERSION_MINOR_INT << 16 );
    rocketnet_check( version == expected );
}

// ---------------------------------------------------------------
// moved subsystem: allocator hook
// ---------------------------------------------------------------

static int test_malloc_calls = 0;
static int test_free_calls = 0;
static void * test_allocator_expected_context = NULL;

static void * test_malloc_function( void * context, size_t bytes )
{
    rocketnet_check( context == test_allocator_expected_context );
    test_malloc_calls++;
    return malloc( bytes );
}

static void test_free_function( void * context, void * p )
{
    rocketnet_check( context == test_allocator_expected_context );
    test_free_calls++;
    free( p );
}

void rocketnet_test_allocator()
{
    test_malloc_calls = 0;
    test_free_calls = 0;
    test_allocator_expected_context = (void*) (uintptr_t) 0x12345678;

    rocketnet_allocator( test_malloc_function, test_free_function );

    // direct malloc/free through the hooks, context passed through
    void * p = rocketnet_malloc( test_allocator_expected_context, 100 );
    rocketnet_check( p );
    rocketnet_check( test_malloc_calls == 1 );
    rocketnet_free( test_allocator_expected_context, p );
    rocketnet_check( test_free_calls == 1 );

    // object creation allocates through the installed hooks with the
    // object's own create context, and destroy releases everything
    test_malloc_calls = 0;
    test_free_calls = 0;

    rocketnet_client_config_t config;
    rocketnet_default_client_config( &config );

    rocketnet_client_t * client = rocketnet_client_create( test_allocator_expected_context, &config );
    rocketnet_check( client );
    rocketnet_check( test_malloc_calls > 0 );

    rocketnet_client_destroy( client );
    rocketnet_check( test_free_calls == test_malloc_calls );

    // NULL/NULL restores the default allocator
    rocketnet_allocator( NULL, NULL );

    test_malloc_calls = 0;
    test_free_calls = 0;
    void * q = rocketnet_malloc( NULL, 16 );
    rocketnet_check( q );
    rocketnet_check( test_malloc_calls == 0 );
    rocketnet_free( NULL, q );
    rocketnet_check( test_free_calls == 0 );
}

// ---------------------------------------------------------------
// moved subsystem: log hooks
// ---------------------------------------------------------------

static int test_log_captured_level = -1;
static char test_log_captured_message[1024];

static void test_log_function( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    vsnprintf( test_log_captured_message, sizeof( test_log_captured_message ), format, args );
    va_end( args );
    test_log_captured_level = level;
}

void rocketnet_test_log()
{
    rocketnet_log_function( test_log_function );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

    // a message at or below the level is delivered, formatted
    test_log_captured_level = -1;
    test_log_captured_message[0] = '\0';
    rocketnet_printf( ROCKETNET_LOG_LEVEL_INFO, "hello %d", 123 );
    rocketnet_check( test_log_captured_level == ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_check( strcmp( test_log_captured_message, "hello 123" ) == 0 );

    // a message above the level is filtered before the hook sees it
    test_log_captured_level = -1;
    test_log_captured_message[0] = '\0';
    rocketnet_printf( ROCKETNET_LOG_LEVEL_DEBUG, "should not appear" );
    rocketnet_check( test_log_captured_level == -1 );
    rocketnet_check( test_log_captured_message[0] == '\0' );

    // raising the level lets it through
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_DEBUG );
    rocketnet_printf( ROCKETNET_LOG_LEVEL_DEBUG, "now it appears" );
    rocketnet_check( test_log_captured_level == ROCKETNET_LOG_LEVEL_DEBUG );
    rocketnet_check( strcmp( test_log_captured_message, "now it appears" ) == 0 );

    // quiet only affects the default log function; contract smoke only
    rocketnet_quiet( true );
    rocketnet_printf( ROCKETNET_LOG_LEVEL_INFO, "still hooked" );
    rocketnet_check( strcmp( test_log_captured_message, "still hooked" ) == 0 );

    // restore
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( NULL );
    rocketnet_quiet( true );    // the test binary runs quiet
}

// ---------------------------------------------------------------
// moved subsystem: assert hook (via the exported data symbol)
// ---------------------------------------------------------------

static int test_assert_calls = 0;

static void test_assert_handler( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    test_assert_calls++;
}

void rocketnet_test_assert_hook()
{
    test_assert_calls = 0;

    rocketnet_assert_function( test_assert_handler );

    // the exported data symbol now points at ours...
    rocketnet_check( rocketnet_assert_function_pointer == test_assert_handler );

    // ...and calling through it (what the rocketnet_assert macro compiles
    // to when asserts are on) reaches our handler rather than trapping
    rocketnet_assert_function_pointer( "test condition", "test_function", "test_file", 1 );
    rocketnet_check( test_assert_calls == 1 );

    // NULL restores the default
    rocketnet_assert_function( NULL );
    rocketnet_check( rocketnet_assert_function_pointer != test_assert_handler );
    rocketnet_check( rocketnet_assert_function_pointer != NULL );
}

// ---------------------------------------------------------------
// moved subsystem: addresses. Ported from flow's own address tests.
// ---------------------------------------------------------------

void rocketnet_test_address()
{
    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "[" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "[]" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "[]:" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, ":" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "1" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "12" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "123" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "1234" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "1234.0.12313.0000" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "1234.0.12313.0000.0.0.0.0.0" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131:1312313:123131" ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "." ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, ".." ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "..." ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "...." ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "....." ) != ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, "hostname.example.com" ) != ROCKETNET_OK );   // no hostname resolution, ever
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "127.0.0.1" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv4[0] == 127 );
        rocketnet_check( address.data.ipv4[1] == 0 );
        rocketnet_check( address.data.ipv4[2] == 0 );
        rocketnet_check( address.data.ipv4[3] == 1 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "107.77.207.77:40000" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV4 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv4[0] == 107 );
        rocketnet_check( address.data.ipv4[1] == 77 );
        rocketnet_check( address.data.ipv4[2] == 207 );
        rocketnet_check( address.data.ipv4[3] == 77 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "fe80::202:b3ff:fe1e:8329" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[1] == 0x0000 );
        rocketnet_check( address.data.ipv6[2] == 0x0000 );
        rocketnet_check( address.data.ipv6[3] == 0x0000 );
        rocketnet_check( address.data.ipv6[4] == 0x0202 );
        rocketnet_check( address.data.ipv6[5] == 0xb3ff );
        rocketnet_check( address.data.ipv6[6] == 0xfe1e );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::1" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 0 );
        for ( int i = 0; i < 7; i++ )
            rocketnet_check( address.data.ipv6[i] == 0x0000 );
        rocketnet_check( address.data.ipv6[7] == 0x0001 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[fe80::202:b3ff:fe1e:8329]:40000" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
        rocketnet_check( address.data.ipv6[0] == 0xfe80 );
        rocketnet_check( address.data.ipv6[7] == 0x8329 );
    }

    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "[::1]:40000" ) == ROCKETNET_OK );
        rocketnet_check( address.type == ROCKETNET_ADDRESS_IPV6 );
        rocketnet_check( address.port == 40000 );
    }
}

void rocketnet_test_address_round_trip()
{
    // guard the inet_ntop6 return value directly: flow's linux version
    // inverted it (== NULL), latent because to_string ignores the return.
    // This check is what makes that class of bug visible.
    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, "::1" ) == ROCKETNET_OK );
        uint16_t ipv6_network_order[8];
        for ( int i = 0; i < 8; i++ )
            ipv6_network_order[i] = rocketnet_platform_htons( address.data.ipv6[i] );
        char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_check( rocketnet_platform_inet_ntop6( ipv6_network_order, buffer, sizeof(buffer) ) == true );
        rocketnet_check( strcmp( buffer, "::1" ) == 0 );
    }

    const char * strings[] =
    {
        "127.0.0.1",
        "107.77.207.77:40000",
        "10.0.0.1:5000",
        "::1",
        "[::1]:40000",
        "[fe80::202:b3ff:fe1e:8329]:40000",
    };

    const int num_strings = (int) ( sizeof(strings) / sizeof(strings[0]) );

    for ( int i = 0; i < num_strings; i++ )
    {
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, strings[i] ) == ROCKETNET_OK );

        char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        const char * result = rocketnet_address_to_string( &address, buffer );
        rocketnet_check( result == buffer );
        rocketnet_check( strcmp( result, strings[i] ) == 0 );

        struct rocketnet_address_t reparsed;
        rocketnet_check( rocketnet_address_parse( &reparsed, result ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_address_equal( &address, &reparsed ) );
    }
}

void rocketnet_test_address_equal()
{
    struct rocketnet_address_t a, b, c, d, e;
    rocketnet_check( rocketnet_address_parse( &a, "127.0.0.1:40000" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_parse( &b, "127.0.0.1:40000" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_parse( &c, "127.0.0.1:40001" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_parse( &d, "127.0.0.2:40000" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_parse( &e, "[::1]:40000" ) == ROCKETNET_OK );

    rocketnet_check( rocketnet_address_equal( &a, &b ) );
    rocketnet_check( rocketnet_address_equal( &a, &a ) );
    rocketnet_check( !rocketnet_address_equal( &a, &c ) );      // different port
    rocketnet_check( !rocketnet_address_equal( &a, &d ) );      // different ip
    rocketnet_check( !rocketnet_address_equal( &a, &e ) );      // different family

    struct rocketnet_address_t v6a, v6b;
    rocketnet_check( rocketnet_address_parse( &v6a, "[::1]:40000" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_parse( &v6b, "[::2]:40000" ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_address_equal( &v6a, &e ) );
    rocketnet_check( !rocketnet_address_equal( &v6a, &v6b ) );
}

void rocketnet_test_address_opaque()
{
    // the pattern a foreign caller uses: allocate by size, never by layout
    size_t bytes = rocketnet_address_bytes();
    rocketnet_check( bytes > 0 );
    rocketnet_check( bytes < 256 );     // sanity: this is a small value type

    struct rocketnet_address_t * address = (struct rocketnet_address_t*) malloc( bytes );
    rocketnet_check( address );

    rocketnet_check( rocketnet_address_parse( address, "1.2.3.4:5" ) == ROCKETNET_OK );

    char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    rocketnet_check( strcmp( rocketnet_address_to_string( address, buffer ), "1.2.3.4:5" ) == 0 );

    free( address );
}

// ---------------------------------------------------------------
// default configs
// ---------------------------------------------------------------

void rocketnet_test_default_configs()
{
    {
        rocketnet_channel_config_t channel;

        rocketnet_default_channel_config( &channel, ROCKETNET_CHANNEL_UNRELIABLE );
        rocketnet_check( channel.kind == ROCKETNET_CHANNEL_UNRELIABLE );
        rocketnet_check( channel.max_payload_bytes == ROCKETNET_MAX_PAYLOAD_BYTES );    // no fragmentation by default
        rocketnet_check( channel.send_queue_size > 0 );
        rocketnet_check( channel.receive_queue_size > 0 );

        rocketnet_default_channel_config( &channel, ROCKETNET_CHANNEL_RELIABLE );
        rocketnet_check( channel.kind == ROCKETNET_CHANNEL_RELIABLE );

        rocketnet_default_channel_config( &channel, ROCKETNET_CHANNEL_LATEST );
        rocketnet_check( channel.kind == ROCKETNET_CHANNEL_LATEST );
    }

    {
        rocketnet_connection_config_t connection;
        rocketnet_default_connection_config( &connection );
        rocketnet_check( connection.num_channels >= 1 );
        rocketnet_check( connection.num_channels <= ROCKETNET_MAX_CHANNELS );
        rocketnet_check( connection.timeout_seconds > 0.0f );
        rocketnet_check( connection.keep_alive_seconds > 0.0f );
        rocketnet_check( connection.keep_alive_seconds < connection.timeout_seconds );
    }

    {
        rocketnet_client_config_t config;
        rocketnet_default_client_config( &config );
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, config.bind_address ) == ROCKETNET_OK );
        rocketnet_check( config.pool_bytes > 0 );
        rocketnet_check( config.backend == ROCKETNET_BACKEND_AUTO );
        rocketnet_check( config.receive_callback == NULL );
    }

    {
        rocketnet_server_config_t config;
        rocketnet_default_server_config( &config );
        struct rocketnet_address_t address;
        rocketnet_check( rocketnet_address_parse( &address, config.server_address ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_address_parse( &address, config.bind_address ) == ROCKETNET_OK );
        rocketnet_check( config.max_connections > 0 );
        rocketnet_check( config.max_flows > 0 );
        rocketnet_check( config.pool_bytes > 0 );
        rocketnet_check( config.max_pool_bytes_per_connection > 0 );
        rocketnet_check( config.max_pool_bytes_per_connection <= config.pool_bytes );
        rocketnet_check( config.backend == ROCKETNET_BACKEND_AUTO );
    }
}

// ---------------------------------------------------------------
// connect token and random keys
// ---------------------------------------------------------------

void rocketnet_test_random_key()
{
    uint8_t key_a[ROCKETNET_KEY_BYTES];
    uint8_t key_b[ROCKETNET_KEY_BYTES];
    memset( key_a, 0, sizeof(key_a) );
    memset( key_b, 0, sizeof(key_b) );

    rocketnet_check( rocketnet_random_key( key_a ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_random_key( key_b ) == ROCKETNET_OK );

    // two keys never collide, and neither is all zero
    rocketnet_check( memcmp( key_a, key_b, ROCKETNET_KEY_BYTES ) != 0 );

    uint8_t zero[ROCKETNET_KEY_BYTES];
    memset( zero, 0, sizeof(zero) );
    rocketnet_check( memcmp( key_a, zero, ROCKETNET_KEY_BYTES ) != 0 );

    rocketnet_check( rocketnet_random_key( NULL ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
}

void rocketnet_test_connect_token()
{
    uint8_t private_key[ROCKETNET_KEY_BYTES];
    memset( private_key, 0, sizeof(private_key) );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];

    const char * good_addresses[] = { "127.0.0.1:40000", "[::1]:40000" };
    const char * bad_addresses[] = { "not an address" };

    // stage 1: argument validation is real, token content is a placeholder
    rocketnet_check( rocketnet_generate_connect_token( 0x1122334455667788ULL, 1, 45, 10, 2, good_addresses, private_key, user_data, connect_token ) == ROCKETNET_OK );

    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, bad_addresses, private_key, user_data, connect_token ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 0, good_addresses, private_key, user_data, connect_token ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, good_addresses, private_key, user_data, NULL ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, good_addresses, NULL, user_data, connect_token ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, good_addresses, private_key, NULL, connect_token ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, NULL, private_key, user_data, connect_token ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
}

// ---------------------------------------------------------------
// client: lifecycle and the connect error path
// ---------------------------------------------------------------

static void test_client_receive_callback( struct rocketnet_client_t * client, void * context, int channel, const uint8_t * payload, int bytes )
{
    (void) client; (void) context; (void) channel; (void) payload; (void) bytes;
}

void rocketnet_test_client_lifecycle()
{
    // safe on NULL
    rocketnet_client_destroy( NULL );

    // create refuses a NULL config
    rocketnet_check( rocketnet_client_create( NULL, NULL ) == NULL );

    // create refuses a bad bind address
    {
        rocketnet_client_config_t config;
        rocketnet_default_client_config( &config );
        rocketnet_copy_string( config.bind_address, "not an address", sizeof( config.bind_address ) );
        rocketnet_check( rocketnet_client_create( NULL, &config ) == NULL );
    }

    // create refuses a bad channel kind
    {
        rocketnet_client_config_t config;
        rocketnet_default_client_config( &config );
        config.connection.num_channels = 1;
        config.connection.channel[0].kind = 42;
        rocketnet_check( rocketnet_client_create( NULL, &config ) == NULL );
    }

    // create refuses too many channels
    {
        rocketnet_client_config_t config;
        rocketnet_default_client_config( &config );
        config.connection.num_channels = ROCKETNET_MAX_CHANNELS + 1;
        rocketnet_check( rocketnet_client_create( NULL, &config ) == NULL );
    }

    // a good config creates, idles and destroys
    {
        rocketnet_client_config_t config;
        rocketnet_default_client_config( &config );

        rocketnet_client_t * client = rocketnet_client_create( NULL, &config );
        rocketnet_check( client );

        rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

        rocketnet_client_update( client );
        rocketnet_client_flush( client );

        rocketnet_check( rocketnet_client_port( client ) == 0 );                    // ephemeral bind, no socket yet
        rocketnet_check( rocketnet_client_server_address( client ) == NULL );       // no connection yet

        rocketnet_client_destroy( client );
    }
}

void rocketnet_test_client_connect_error_path()
{
    // the stage 1 connect path: the attempt starts, nothing can answer it,
    // and it times out through the documented state machine — never a crash

    uint8_t private_key[ROCKETNET_KEY_BYTES];
    memset( private_key, 0, sizeof(private_key) );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const char * server_addresses[] = { "127.0.0.1:40000" };
    rocketnet_check( rocketnet_generate_connect_token( 1, 1, 45, 10, 1, server_addresses, private_key, user_data, connect_token ) == ROCKETNET_OK );

    rocketnet_client_config_t config;
    rocketnet_default_client_config( &config );
    config.connection.timeout_seconds = 0.25f;      // keep the test fast

    rocketnet_client_t * client = rocketnet_client_create( NULL, &config );
    rocketnet_check( client );

    rocketnet_check( rocketnet_client_connect( client, connect_token ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );

    // bounded wait: at most ~3 seconds for a 0.25 second timeout
    for ( int i = 0; i < 60; i++ )
    {
        rocketnet_client_update( client );
        if ( rocketnet_client_state( client ) < 0 )
            break;
        rocketnet_platform_sleep( 0.05 );
    }

    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT );

    // disconnect returns it to DISCONNECTED, and the client is reusable
    rocketnet_client_disconnect( client );
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

    rocketnet_check( rocketnet_client_connect( client, connect_token ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST );
    rocketnet_client_disconnect( client );

    // connect refuses a NULL token
    rocketnet_check( rocketnet_client_connect( client, NULL ) == ROCKETNET_ERROR_INVALID_ARGUMENT );

    rocketnet_client_destroy( client );
}

void rocketnet_test_client_stub_contract()
{
    rocketnet_client_config_t config;
    rocketnet_default_client_config( &config );

    rocketnet_client_t * client = rocketnet_client_create( NULL, &config );
    rocketnet_check( client );

    // claim: no pool exists yet, and a failed claim zeroes the buffer
    {
        rocketnet_buffer_t buffer;
        memset( &buffer, 0xFF, sizeof(buffer) );
        rocketnet_check( rocketnet_client_claim( client, 1024, &buffer ) == ROCKETNET_ERROR_POOL_EXHAUSTED );
        rocketnet_check( buffer.data == NULL );
        rocketnet_check( buffer.token == 0 );
        rocketnet_check( buffer.bytes == 0 );

        memset( &buffer, 0xFF, sizeof(buffer) );
        rocketnet_check( rocketnet_client_claim( client, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, &buffer ) == ROCKETNET_ERROR_PAYLOAD_TOO_LARGE );
        rocketnet_check( buffer.data == NULL && buffer.token == 0 && buffer.bytes == 0 );

        rocketnet_check( rocketnet_client_claim( client, 0, &buffer ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
        rocketnet_check( rocketnet_client_claim( client, -1, &buffer ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    }

    // commit / abandon: no claim can be live and no connection exists
    {
        rocketnet_buffer_t buffer;
        memset( &buffer, 0, sizeof(buffer) );
        rocketnet_check( rocketnet_client_commit( client, 0, &buffer, 0 ) == ROCKETNET_ERROR_NOT_CONNECTED );
        rocketnet_check( rocketnet_client_commit( client, 99, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
        rocketnet_check( rocketnet_client_commit( client, -1, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
        rocketnet_check( rocketnet_client_commit( client, 0, &buffer, 1 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );    // bytes > claimed
        rocketnet_check( rocketnet_client_abandon( client, &buffer ) == ROCKETNET_ERROR_STALE_BUFFER );
    }

    // send: not connected is the truth in stage 1
    {
        uint8_t payload[64];
        memset( payload, 0, sizeof(payload) );
        rocketnet_check( rocketnet_client_send( client, 0, payload, sizeof(payload) ) == ROCKETNET_ERROR_NOT_CONNECTED );
        rocketnet_check( rocketnet_client_send( client, 99, payload, sizeof(payload) ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
        rocketnet_check( rocketnet_client_send( client, 0, payload, 0 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    }

    // receive: OK with zero payloads is the documented "nothing arrived"
    {
        rocketnet_payload_t payloads[8];
        int num_payloads = -1;
        rocketnet_check( rocketnet_client_receive( client, payloads, 8, &num_payloads ) == ROCKETNET_OK );
        rocketnet_check( num_payloads == 0 );
    }

    // release: zero is fine; a fake payload is stale, not corruption
    {
        rocketnet_payload_t payload;
        memset( &payload, 0, sizeof(payload) );
        rocketnet_check( rocketnet_client_release( client, &payload, 0 ) == ROCKETNET_OK );
        payload.token = 12345;
        rocketnet_check( rocketnet_client_release( client, &payload, 1 ) == ROCKETNET_ERROR_STALE_BUFFER );
    }

    // stats: false when not connected, and stats untouched
    {
        rocketnet_connection_stats_t stats;
        memset( &stats, 0xAB, sizeof(stats) );
        rocketnet_check( rocketnet_client_stats( client, &stats ) == false );
        rocketnet_check( ((uint8_t*)&stats)[0] == 0xAB );       // untouched
    }

    rocketnet_client_destroy( client );

    // both receive paths configured is an error, not a race
    {
        rocketnet_client_config_t both_config;
        rocketnet_default_client_config( &both_config );
        both_config.receive_callback = test_client_receive_callback;

        rocketnet_client_t * both = rocketnet_client_create( NULL, &both_config );
        rocketnet_check( both );

        rocketnet_payload_t payloads[8];
        int num_payloads = -1;
        rocketnet_check( rocketnet_client_receive( both, payloads, 8, &num_payloads ) == ROCKETNET_ERROR_INVALID_ARGUMENT );

        rocketnet_client_destroy( both );
    }
}

// ---------------------------------------------------------------
// server: lifecycle, stubs, flows, stats
// ---------------------------------------------------------------

static void test_server_receive_callback( struct rocketnet_server_t * server, void * context, rocketnet_connection_id_t connection_id, int channel, const uint8_t * payload, int bytes )
{
    (void) server; (void) context; (void) connection_id; (void) channel; (void) payload; (void) bytes;
}

static rocketnet_server_t * test_create_server( int max_flows )
{
    rocketnet_server_config_t config;
    rocketnet_default_server_config( &config );
    rocketnet_copy_string( config.server_address, "127.0.0.1:40000", sizeof( config.server_address ) );
    rocketnet_copy_string( config.bind_address, "0.0.0.0:40000", sizeof( config.bind_address ) );
    config.max_flows = max_flows;
    return rocketnet_server_create( NULL, &config );
}

void rocketnet_test_server_lifecycle()
{
    // safe on NULL
    rocketnet_server_destroy( NULL );

    // create refuses a NULL config and bad addresses
    rocketnet_check( rocketnet_server_create( NULL, NULL ) == NULL );

    {
        rocketnet_server_config_t config;
        rocketnet_default_server_config( &config );
        rocketnet_copy_string( config.bind_address, "not an address", sizeof( config.bind_address ) );
        rocketnet_check( rocketnet_server_create( NULL, &config ) == NULL );
    }

    {
        rocketnet_server_config_t config;
        rocketnet_default_server_config( &config );
        rocketnet_copy_string( config.server_address, "not an address", sizeof( config.server_address ) );
        rocketnet_check( rocketnet_server_create( NULL, &config ) == NULL );
    }

    {
        rocketnet_server_config_t config;
        rocketnet_default_server_config( &config );
        config.max_connections = 0;
        rocketnet_check( rocketnet_server_create( NULL, &config ) == NULL );
    }

    // a good config creates, reports its addresses, idles and destroys
    {
        rocketnet_server_t * server = test_create_server( 64 );
        rocketnet_check( server );

        rocketnet_check( rocketnet_server_port( server ) == 40000 );

        const struct rocketnet_address_t * address = rocketnet_server_address( server );
        rocketnet_check( address );
        char buffer[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_check( strcmp( rocketnet_address_to_string( address, buffer ), "127.0.0.1:40000" ) == 0 );

        rocketnet_server_update( server );
        rocketnet_server_flush( server );

        rocketnet_check( rocketnet_server_connection_count( server ) == 0 );

        rocketnet_connection_id_t ids[16];
        int num_ids = -1;
        rocketnet_check( rocketnet_server_connection_ids( server, ids, 16, &num_ids ) == ROCKETNET_OK );
        rocketnet_check( num_ids == 0 );

        rocketnet_check( rocketnet_server_connection_connected( server, 1 ) == false );
        rocketnet_check( rocketnet_server_connection_address( server, 1 ) == NULL );
        rocketnet_check( rocketnet_server_disconnect( server, 1 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_server_disconnect_all( server );

        rocketnet_server_destroy( server );
    }
}

void rocketnet_test_server_stub_contract()
{
    rocketnet_server_t * server = test_create_server( 64 );
    rocketnet_check( server );

    // claim: no pool exists yet, failed claim zeroes the buffer
    {
        rocketnet_buffer_t buffer;
        memset( &buffer, 0xFF, sizeof(buffer) );
        rocketnet_check( rocketnet_server_claim( server, 1024, &buffer ) == ROCKETNET_ERROR_POOL_EXHAUSTED );
        rocketnet_check( buffer.data == NULL && buffer.token == 0 && buffer.bytes == 0 );

        rocketnet_check( rocketnet_server_claim( server, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, &buffer ) == ROCKETNET_ERROR_PAYLOAD_TOO_LARGE );
        rocketnet_check( rocketnet_server_claim( server, 0, &buffer ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    }

    // commit: no connection can exist
    {
        rocketnet_buffer_t buffer;
        memset( &buffer, 0, sizeof(buffer) );
        rocketnet_check( rocketnet_server_commit( server, 1, 0, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_check( rocketnet_server_commit( server, 1, 99, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
        rocketnet_check( rocketnet_server_commit( server, 1, 0, &buffer, 1 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
        rocketnet_check( rocketnet_server_abandon( server, &buffer ) == ROCKETNET_ERROR_STALE_BUFFER );
    }

    // commit_batch: the call is well-formed, each entry's result is filled
    {
        rocketnet_send_t sends[2];
        memset( sends, 0, sizeof(sends) );
        sends[0].connection_id = 1;
        sends[0].channel = 0;
        sends[0].result = 12345;
        sends[1].connection_id = 2;
        sends[1].channel = 99;
        sends[1].result = 12345;
        rocketnet_check( rocketnet_server_commit_batch( server, sends, 2 ) == ROCKETNET_OK );
        rocketnet_check( sends[0].result == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_check( sends[1].result == ROCKETNET_ERROR_UNKNOWN_CHANNEL );

        rocketnet_check( rocketnet_server_commit_batch( server, NULL, 0 ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_server_commit_batch( server, NULL, 1 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    }

    // commit_many: no live handle can exist
    {
        rocketnet_connection_id_t ids[2] = { 1, 2 };
        rocketnet_buffer_t buffer;
        memset( &buffer, 0, sizeof(buffer) );
        rocketnet_check( rocketnet_server_commit_many( server, ids, 2, 0, &buffer, 0 ) == ROCKETNET_ERROR_STALE_BUFFER );
        rocketnet_check( rocketnet_server_commit_many( server, ids, 2, 99, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
        rocketnet_check( rocketnet_server_commit_many( server, NULL, 2, 0, &buffer, 0 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
    }

    // send: no connection can exist
    {
        uint8_t payload[64];
        memset( payload, 0, sizeof(payload) );
        rocketnet_check( rocketnet_server_send( server, 1, 0, payload, sizeof(payload) ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_check( rocketnet_server_send( server, 1, 99, payload, sizeof(payload) ) == ROCKETNET_ERROR_UNKNOWN_CHANNEL );
    }

    // receive on a server with no callback configured: OK, nothing arrived
    {
        rocketnet_payload_t payloads[8];
        int num_payloads = -1;
        rocketnet_check( rocketnet_server_receive( server, payloads, 8, &num_payloads ) == ROCKETNET_OK );
        rocketnet_check( num_payloads == 0 );
    }

    // release: zero fine, fake stale
    {
        rocketnet_payload_t payload;
        memset( &payload, 0, sizeof(payload) );
        rocketnet_check( rocketnet_server_release( server, &payload, 0 ) == ROCKETNET_OK );
        payload.token = 999;
        rocketnet_check( rocketnet_server_release( server, &payload, 1 ) == ROCKETNET_ERROR_STALE_BUFFER );
    }

    rocketnet_server_destroy( server );

    // both receive paths configured is an error, not a race
    {
        rocketnet_server_config_t config;
        rocketnet_default_server_config( &config );
        config.receive_callback = test_server_receive_callback;

        rocketnet_server_t * both = rocketnet_server_create( NULL, &config );
        rocketnet_check( both );

        rocketnet_payload_t payloads[8];
        int num_payloads = -1;
        rocketnet_check( rocketnet_server_receive( both, payloads, 8, &num_payloads ) == ROCKETNET_ERROR_INVALID_ARGUMENT );

        rocketnet_server_destroy( both );
    }
}

void rocketnet_test_server_flows()
{
    rocketnet_server_t * server = test_create_server( 4 );     // small max_flows to test exhaustion
    rocketnet_check( server );

    // the registry is real: create, destroy, capacity, channel validation
    rocketnet_flow_id_t flow = rocketnet_server_flow_create( server, 0 );
    rocketnet_check( flow != ROCKETNET_INVALID_FLOW_ID );

    rocketnet_check( rocketnet_server_flow_create( server, 99 ) == ROCKETNET_INVALID_FLOW_ID );     // bad channel
    rocketnet_check( rocketnet_server_flow_create( server, -1 ) == ROCKETNET_INVALID_FLOW_ID );

    // a live flow with no subscribers is a normal state of the world
    rocketnet_check( rocketnet_server_flow_subscriber_count( server, flow ) == 0 );

    {
        rocketnet_connection_id_t ids[16];
        int num_ids = -1;
        rocketnet_check( rocketnet_server_flow_subscribers( server, flow, ids, 16, &num_ids ) == ROCKETNET_OK );
        rocketnet_check( num_ids == 0 );
    }

    // subscription is real about the flow and honest about the connection
    rocketnet_check( rocketnet_server_flow_subscribe( server, flow, 1 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
    rocketnet_check( rocketnet_server_flow_subscribe( server, 999, 1 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );
    rocketnet_check( rocketnet_server_flow_subscribe( server, ROCKETNET_INVALID_FLOW_ID, 1 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );

    rocketnet_check( rocketnet_server_flow_unsubscribe( server, flow, 1 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
    rocketnet_check( rocketnet_server_flow_unsubscribe( server, 999, 1 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );

    {
        rocketnet_connection_id_t ids[2] = { 1, 2 };
        rocketnet_check( rocketnet_server_flow_subscribe_many( server, flow, ids, 2 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_check( rocketnet_server_flow_subscribe_many( server, flow, ids, 0 ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_server_flow_subscribe_many( server, flow, NULL, 0 ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_server_flow_subscribe_many( server, flow, NULL, 2 ) == ROCKETNET_ERROR_INVALID_ARGUMENT );
        rocketnet_check( rocketnet_server_flow_subscribe_many( server, 999, ids, 2 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );

        rocketnet_check( rocketnet_server_flow_unsubscribe_many( server, flow, ids, 2 ) == ROCKETNET_ERROR_UNKNOWN_CONNECTION );
        rocketnet_check( rocketnet_server_flow_unsubscribe_many( server, flow, ids, 0 ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_server_flow_unsubscribe_many( server, 999, ids, 2 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );
    }

    // commit to a flow: the flow check is real, the buffer cannot be live yet
    {
        rocketnet_buffer_t buffer;
        memset( &buffer, 0, sizeof(buffer) );
        rocketnet_check( rocketnet_server_commit_flow( server, flow, &buffer, 0 ) == ROCKETNET_ERROR_STALE_BUFFER );
        rocketnet_check( rocketnet_server_commit_flow( server, 999, &buffer, 0 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );
    }

    // capacity: fill the registry, exhaust it, free a slot, refill it
    {
        rocketnet_flow_id_t flows[3];
        for ( int i = 0; i < 3; i++ )
        {
            flows[i] = rocketnet_server_flow_create( server, 0 );
            rocketnet_check( flows[i] != ROCKETNET_INVALID_FLOW_ID );
        }

        rocketnet_check( rocketnet_server_flow_create( server, 0 ) == ROCKETNET_INVALID_FLOW_ID );  // max_flows reached

        rocketnet_check( rocketnet_server_flow_destroy( server, flows[1] ) == ROCKETNET_OK );
        rocketnet_check( rocketnet_server_flow_destroy( server, flows[1] ) == ROCKETNET_ERROR_UNKNOWN_FLOW );   // already dead

        rocketnet_flow_id_t reused = rocketnet_server_flow_create( server, 0 );
        rocketnet_check( reused != ROCKETNET_INVALID_FLOW_ID );

        // dead flow ids answer UNKNOWN_FLOW everywhere
        rocketnet_check( rocketnet_server_flow_destroy( server, 999 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );
        rocketnet_check( rocketnet_server_flow_subscriber_count( server, 999 ) == ROCKETNET_ERROR_UNKNOWN_FLOW );
    }

    rocketnet_server_destroy( server );
}

void rocketnet_test_server_stats()
{
    rocketnet_server_t * server = test_create_server( 8 );
    rocketnet_check( server );

    rocketnet_flow_id_t flow_a = rocketnet_server_flow_create( server, 0 );
    rocketnet_flow_id_t flow_b = rocketnet_server_flow_create( server, 0 );
    rocketnet_check( flow_a != ROCKETNET_INVALID_FLOW_ID );
    rocketnet_check( flow_b != ROCKETNET_INVALID_FLOW_ID );

    rocketnet_server_stats_t stats;
    memset( &stats, 0xAB, sizeof(stats) );
    rocketnet_check( rocketnet_server_stats( server, &stats ) == true );
    rocketnet_check( stats.num_connections == 0 );
    rocketnet_check( stats.num_flows == 2 );
    rocketnet_check( stats.pool_bytes_in_use == 0 );
    rocketnet_check( stats.packets_sent == 0 );
    rocketnet_check( stats.backend == ROCKETNET_BACKEND_AUTO );     // stage 1: nothing to resolve to yet

    rocketnet_check( rocketnet_server_flow_destroy( server, flow_a ) == ROCKETNET_OK );
    rocketnet_check( rocketnet_server_stats( server, &stats ) == true );
    rocketnet_check( stats.num_flows == 1 );

    // per-connection stats: false for an unknown id, stats untouched
    rocketnet_connection_stats_t connection_stats;
    memset( &connection_stats, 0xCD, sizeof(connection_stats) );
    rocketnet_check( rocketnet_server_connection_stats( server, 1, &connection_stats ) == false );
    rocketnet_check( ((uint8_t*)&connection_stats)[0] == 0xCD );

    rocketnet_server_destroy( server );
}

// ---------------------------------------------------------------

#define RUN_TEST( test_function )                                           \
    do                                                                      \
    {                                                                       \
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, #test_function );       \
        fflush( stdout );                                                   \
        rocketnet_##test_function();                                        \
    }                                                                       \
    while (0)

void rocketnet_run_tests()
{
    RUN_TEST( test_time );
    RUN_TEST( test_error_strings );
    RUN_TEST( test_backend_strings );
    RUN_TEST( test_protocol_version );
    RUN_TEST( test_allocator );
    RUN_TEST( test_log );
    RUN_TEST( test_assert_hook );
    RUN_TEST( test_address );
    RUN_TEST( test_address_round_trip );
    RUN_TEST( test_address_equal );
    RUN_TEST( test_address_opaque );
    RUN_TEST( test_default_configs );
    RUN_TEST( test_random_key );
    RUN_TEST( test_connect_token );
    RUN_TEST( test_client_lifecycle );
    RUN_TEST( test_client_connect_error_path );
    RUN_TEST( test_client_stub_contract );
    RUN_TEST( test_server_lifecycle );
    RUN_TEST( test_server_stub_contract );
    RUN_TEST( test_server_flows );
    RUN_TEST( test_server_stats );
}
