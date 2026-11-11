/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public API cases for create and the address and lifecycle getters
// (sdk/rocketnet.h):
//
//   * test_public_server_create_refuses_split_ports: with both addresses the
//     two share one port, so create refuses an IPv4 and an IPv6 address on
//     different ports with INVALID_ADDRESS, and accepts the same port or one
//     address alone;
//   * test_public_server_ipv6_address_reads_the_running_address: a running
//     server that serves IPv6 writes "[::1]:port" for its bound port, and
//     one that does not serves no IPv6 address;
//   * test_public_null_lifecycle_and_simulator_calls_count_misuse: a NULL
//     object to rocketnet_client_disconnected, rocketnet_server_stopped and,
//     in a development build, the two network simulator calls counts one
//     misuse, fires one assert and returns false or does nothing;
//   * test_public_server_keygen_failure_leaves_the_block_unwritten: a create
//     refused because the key cannot be made frees its block without writing
//     it, so a default server does not touch the pages of its whole plan.
//
// The loopback transport carries packets in memory; callers provide every
// time, and no case sleeps or opens a socket.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_connect_token.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static void api_core_server_config( rocketnet_server_config_t * config )
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0xC0 ^ i );
    }

    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = 0xA91C0BE5C0FFEE01ULL;
    config->max_clients = 1;
    memcpy( config->backend_public_key, backend_public, sizeof( backend_public ) );
    const uint8_t filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x24, 0x68, 0xAC, 0xE0 };
    memcpy( config->packet_filter, filter, sizeof( filter ) );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// --- create refuses an IPv4 and an IPv6 address on different ports ---------------

static void test_public_server_create_refuses_split_ports()
{
    rocketnet_server_config_t config;
    api_core_server_config( &config );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:40000", "[::1]:40001", &config, &status );
    check( server == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );

    status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:40001", "[::1]:40000", &config, &status );
    check( server == NULL );
    check( status == ROCKETNET_STATUS_INVALID_ADDRESS );

    const char * accepted[][2] = {
        { "127.0.0.1:40000", "[::1]:40000" },
        { "127.0.0.1:40000", NULL },
        { NULL, "[::1]:40001" },
    };

    for ( int i = 0; i < (int) ( sizeof( accepted ) / sizeof( accepted[0] ) ); i++ )
    {
        status = ROCKETNET_STATUS_INVALID_ADDRESS;
        server = rocketnet_server_create( NULL, accepted[i][0], accepted[i][1], &config, &status );
        check( server != NULL );
        check( status == ROCKETNET_STATUS_OK );
        rocketnet_server_destroy( server );
    }
}

REGISTER_UNIT_TEST( test_public_server_create_refuses_split_ports );

// --- the IPv6 address of a running server -----------------------------------------

static void api_core_check_ipv6_address( rocketnet_server_t * server )
{
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char expected[ROCKETNET_MAX_ADDRESS_STRING_BYTES];

    // stopped: no address, and the buffer is emptied

    memset( address, 'x', sizeof( address ) );
    check( !rocketnet_server_ipv6_address( server, address, sizeof( address ) ) );
    check( address[0] == '\0' );

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    const uint16_t port = rocketnet_server_port( server );
    check( port != 0 );
    snprintf( expected, sizeof( expected ), "[::1]:%d", (int) port );

    memset( address, 'x', sizeof( address ) );
    check( rocketnet_server_ipv6_address( server, address, sizeof( address ) ) );
    check( strcmp( address, expected ) == 0 );

    // exactly the string and its terminator fits; one byte fewer is refused
    // and leaves an empty string

    const size_t needed = strlen( expected ) + 1;
    check( rocketnet_server_ipv6_address( server, address, needed ) );
    check( strcmp( address, expected ) == 0 );
    check( !rocketnet_server_ipv6_address( server, address, needed - 1 ) );
    check( address[0] == '\0' );

    rn_test_server_stop_and_destroy( server );
}

static void test_public_server_ipv6_address_reads_the_running_address()
{
    rocketnet_quiet( 1 );

    rocketnet_server_config_t config;
    api_core_server_config( &config );

    rocketnet_server_t * ipv6_only = rocketnet_server_create( NULL, NULL, "[::1]:0", &config, NULL );
    check( ipv6_only != NULL );
    api_core_check_ipv6_address( ipv6_only );

    rocketnet_server_t * both = rocketnet_server_create( NULL, "127.0.0.1:0", "[::1]:0", &config, NULL );
    check( both != NULL );
    api_core_check_ipv6_address( both );

    // a server that serves IPv4 only has no IPv6 address

    rocketnet_server_t * ipv4_only = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( ipv4_only != NULL );
    check( rocketnet_server_start( ipv4_only ) == ROCKETNET_STATUS_OK );
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    memset( address, 'x', sizeof( address ) );
    check( !rocketnet_server_ipv6_address( ipv4_only, address, sizeof( address ) ) );
    check( address[0] == '\0' );
    rn_test_server_stop_and_destroy( ipv4_only );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_public_server_ipv6_address_reads_the_running_address );

// --- NULL objects to the lifecycle polls and the network simulators ----------------

static int api_core_asserts = 0;

static void api_core_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    api_core_asserts++;
}

// a build with asserts compiled out (ROCKETNET_ASSERTS 0) still counts the
// misuse and refuses; a build with them fires exactly one
#if ROCKETNET_ASSERTS
#define API_CORE_ASSERTS_FIRED 1
#else // #if ROCKETNET_ASSERTS
#define API_CORE_ASSERTS_FIRED 0
#endif // #if ROCKETNET_ASSERTS

#define API_CORE_ONE_MISUSE( call )                                       \
    do                                                                    \
    {                                                                     \
        const uint64_t misuse_before = rocketnet_api_misuse_count();      \
        const int asserts_before = api_core_asserts;                      \
        call;                                                             \
        check( rocketnet_api_misuse_count() == misuse_before + 1 );       \
        check( api_core_asserts == asserts_before + API_CORE_ASSERTS_FIRED ); \
    } while ( 0 )

static void test_public_null_lifecycle_and_simulator_calls_count_misuse()
{
    rocketnet_quiet( 1 );
    rocketnet_assert_function( api_core_count_assert );

    API_CORE_ONE_MISUSE( check( !rocketnet_client_disconnected( NULL ) ) );
    API_CORE_ONE_MISUSE( check( !rocketnet_server_stopped( NULL ) ) );
#if ROCKETNET_DEV
    API_CORE_ONE_MISUSE( rocketnet_client_network_simulator( NULL, 0, 0, 0, 0 ) );
    API_CORE_ONE_MISUSE( rocketnet_server_network_simulator( NULL, 0, 0, 0, 0 ) );
#endif // #if ROCKETNET_DEV

    rocketnet_assert_function( NULL );
    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_public_null_lifecycle_and_simulator_calls_count_misuse );

#if ROCKETNET_DEV

#include "../sdk/rocketnet_test_seams.h"

// --- a refused key leaves the server block unwritten --------------------------------

// an allocator that fills every block with a pattern and, while watching,
// records whether the largest block it frees still holds the pattern in its
// middle: create's block is the largest thing a failed create frees

struct api_core_block_allocator_t
{
    int live;
    bool watching;
    size_t largest;
    bool largest_intact;
};

static void * api_core_block_allocate( void * context, size_t bytes )
{
    api_core_block_allocator_t * state = (api_core_block_allocator_t *) context;

    void * base = malloc( bytes + 128 );
    if ( !base )
        return NULL;

    uintptr_t address = ( (uintptr_t) base + 2 * sizeof( void * ) + 63 ) & ~(uintptr_t) 63;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memcpy( (void *) ( address - 2 * sizeof( void * ) ), &bytes, sizeof( bytes ) );
    memset( (void *) address, 0xA5, bytes );
    ++state->live;

    return (void *) address;
}

static void api_core_block_release( void * context, void * pointer )
{
    api_core_block_allocator_t * state = (api_core_block_allocator_t *) context;
    void * base = NULL;
    size_t bytes = 0;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    memcpy( &bytes, (uint8_t *) pointer - 2 * sizeof( void * ), sizeof( bytes ) );

    if ( state->watching && bytes > state->largest )
    {
        state->largest = bytes;
        state->largest_intact = ( (const uint8_t *) pointer )[bytes / 2] == 0xA5 && ( (const uint8_t *) pointer )[bytes - 1] == 0xA5;
    }

    --state->live;
    free( base );
}

static void test_public_server_keygen_failure_leaves_the_block_unwritten()
{
    api_core_block_allocator_t state;
    memset( &state, 0, sizeof( state ) );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    int init_status = -1;
    check( rocketnet_init_ex( &state, api_core_block_allocate, api_core_block_release, &init_status ) );
    check( init_status == ROCKETNET_STATUS_OK );
    rocketnet_test_loopback_hub_context( &state );

    rocketnet_server_config_t config;
    api_core_server_config( &config );

    state.watching = true;
    rocketnet_dev_set_server_keygen_failure( true );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( &state, "127.0.0.1:0", NULL, &config, &status );
    rocketnet_dev_set_server_keygen_failure( false );
    state.watching = false;

    check( server == NULL );
    check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED );
    check( state.largest > 0 );
    check( state.largest_intact );

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_ex() == ROCKETNET_STATUS_OK );
    check( state.live == 0 );
    check( rocketnet_allocator_ex( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_public_server_keygen_failure_leaves_the_block_unwritten );

#endif // #if ROCKETNET_DEV
