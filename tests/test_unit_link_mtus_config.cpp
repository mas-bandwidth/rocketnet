/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The link MTU list in the config (link_mtus, num_link_mtus): its default
// {1500} on the public and the transport configs; each refusal at client and
// server create, public and transport (EMPTY for no entry or more than
// ROCKETNET_MAX_LINK_MTUS, TOO_LARGE for an entry above 1500, NOT_DESCENDING
// including equal entries, TOO_SMALL below 546, the smallest IPv4 link that
// carries the 518-byte connection request, and TOO_SMALL_FOR_MESSAGES when
// the message config cannot fit its largest message at the smallest entry);
// valid lists create; the environment form, decimal entries joined by
// commas, parses and junk is refused; an older caller's config reads the
// list as its default; and the list reaches the client and server
// protocols. No socket and no real time: the creates refuse before a
// socket, a create that succeeds opens none, and the one pair that starts
// and connects runs on the loopback transport, step driven, at a fixed
// time. The environment cases set one variable per side and restore it
// before returning.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#include "portable_crt.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_layers.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "loopback_hub.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define LINK_MTUS_COUNT( a ) ( (int) ( sizeof( a ) / sizeof( a[0] ) ) )

static const uint8_t link_mtus_packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x4C, 0x4D, 0x54, 0x55 };

// the defaults with the test's own packet filter and backend key (never the
// published ones, which a production library refuses)

static void link_mtus_client_config( struct rocketnet_client_config_t * config )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    memcpy( config->packet_filter, link_mtus_packet_filter, sizeof( config->packet_filter ) );
}

static void link_mtus_server_config( struct rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = 0x4C4D545553ULL;
    config->max_clients = 4;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x4C ^ i );
    }
    uint8_t backend_public_key[32];
    uint8_t backend_secret_key[64];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public_key, backend_secret_key ) == ROCKETNET_STATUS_OK );
    memcpy( config->backend_public_key, backend_public_key, 32 );
    memcpy( config->packet_filter, link_mtus_packet_filter, sizeof( config->packet_filter ) );
}

static void link_mtus_set( int * link_mtus, int * num_link_mtus, const int * entries, int count )
{
    for ( int i = 0; i < count && i < ROCKETNET_MAX_LINK_MTUS; i++ )
    {
        link_mtus[i] = entries[i];
    }
    *num_link_mtus = count;
}

// a list fills every one of the ROCKETNET_MAX_LINK_MTUS entries; the
// count past them is the over-long refusal
static const int link_mtus_full[ROCKETNET_MAX_LINK_MTUS] = { 1500, 1400, 1300, 1200, 1100, 1000, 900, 800 };

struct link_mtus_refusal_t
{
    const char * what;
    int entries[ROCKETNET_MAX_LINK_MTUS];
    int count;
    int status;
};

static const link_mtus_refusal_t link_mtus_refusals[] =
{
    { "empty", { 1500 }, 0, ROCKETNET_STATUS_LINK_MTUS_EMPTY },
    { "nine entries", { 1500, 1400, 1300, 1200, 1100, 1000, 900, 800 }, ROCKETNET_MAX_LINK_MTUS + 1, ROCKETNET_STATUS_LINK_MTUS_EMPTY },
    { "negative count", { 1500 }, -1, ROCKETNET_STATUS_LINK_MTUS_EMPTY },
    { "equal entries", { 1500, 1500 }, 2, ROCKETNET_STATUS_LINK_MTUS_NOT_DESCENDING },
    { "ascending", { 1200, 1300 }, 2, ROCKETNET_STATUS_LINK_MTUS_NOT_DESCENDING },
    { "1501", { 1501 }, 1, ROCKETNET_STATUS_LINK_MTUS_TOO_LARGE },
    { "545", { 545 }, 1, ROCKETNET_STATUS_LINK_MTUS_TOO_SMALL },
    { "1500 then 545", { 1500, 545 }, 2, ROCKETNET_STATUS_LINK_MTUS_TOO_SMALL },
};

static void test_link_mtus_default_is_1500()
{
    struct rocketnet_client_config_t client;
    check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    check( client.num_link_mtus == 1 );
    check( client.link_mtus[0] == 1500 );

    struct rocketnet_server_config_t server;
    check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    check( server.num_link_mtus == 1 );
    check( server.link_mtus[0] == 1500 );

    struct rocketnet_client_transport_config_t client_transport;
    rocketnet_client_transport_default_config( &client_transport );
    check( client_transport.num_link_mtus == 1 );
    check( client_transport.link_mtus[0] == 1500 );

    struct rocketnet_server_transport_config_t server_transport;
    rocketnet_server_transport_default_config( &server_transport );
    check( server_transport.num_link_mtus == 1 );
    check( server_transport.link_mtus[0] == 1500 );

    struct rocketnet_server_transport_config_t server_transport_current;
    rocketnet_server_transport_default_config_current( &server_transport_current );
    check( server_transport_current.num_link_mtus == 1 );
    check( server_transport_current.link_mtus[0] == 1500 );
}

// each refusal at the public client and server create, with its own status

static void test_link_mtus_public_create_refusals()
{
    rocketnet_quiet( 1 );

    for ( int i = 0; i < LINK_MTUS_COUNT( link_mtus_refusals ); i++ )
    {
        const link_mtus_refusal_t * refusal = &link_mtus_refusals[i];

        struct rocketnet_client_config_t client_config;
        link_mtus_client_config( &client_config );
        link_mtus_set( client_config.link_mtus, &client_config.num_link_mtus, refusal->entries, refusal->count );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
        if ( client != NULL || (int) status != refusal->status )
        {
            printf( "client create, %s: status %d, expected %d\n", refusal->what, (int) status, refusal->status );
        }
        check( client == NULL );
        check( (int) status == refusal->status );

        struct rocketnet_server_config_t server_config;
        link_mtus_server_config( &server_config );
        link_mtus_set( server_config.link_mtus, &server_config.num_link_mtus, refusal->entries, refusal->count );
        status = ROCKETNET_STATUS_OK;
        struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
        if ( server != NULL || (int) status != refusal->status )
        {
            printf( "server create, %s: status %d, expected %d\n", refusal->what, (int) status, refusal->status );
        }
        check( server == NULL );
        check( (int) status == refusal->status );
    }

    // the default message config's largest message (1 MiB) takes more
    // fragments than its window of 1024 below a 1109-byte link: 2185 of 480
    // bytes at 576, 1041 of 1008 at 1108

    const int smallest[] = { 576, 1108 };

    for ( int i = 0; i < LINK_MTUS_COUNT( smallest ); i++ )
    {
        const int two_step[] = { 1500, smallest[i] };

        struct rocketnet_client_config_t client_config;
        link_mtus_client_config( &client_config );
        link_mtus_set( client_config.link_mtus, &client_config.num_link_mtus, two_step, 2 );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_LINK_MTUS_TOO_SMALL_FOR_MESSAGES );

        struct rocketnet_server_config_t server_config;
        link_mtus_server_config( &server_config );
        link_mtus_set( server_config.link_mtus, &server_config.num_link_mtus, two_step, 2 );
        status = ROCKETNET_STATUS_OK;
        check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
        check( status == ROCKETNET_STATUS_LINK_MTUS_TOO_SMALL_FOR_MESSAGES );
    }

    rocketnet_quiet( 0 );
}

// the same list refusals at the transport creates, before the connect token
// is read (it is all zero here) and before any socket

static void test_link_mtus_transport_create_refusals()
{
    rocketnet_quiet( 1 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    for ( int i = 0; i < LINK_MTUS_COUNT( link_mtus_refusals ); i++ )
    {
        const link_mtus_refusal_t * refusal = &link_mtus_refusals[i];

        struct rocketnet_client_transport_config_t client_config;
        rocketnet_client_transport_default_config( &client_config );
        memcpy( client_config.packet_filter, link_mtus_packet_filter, sizeof( client_config.packet_filter ) );
        link_mtus_set( client_config.link_mtus, &client_config.num_link_mtus, refusal->entries, refusal->count );
        int status = ROCKETNET_STATUS_OK;
        check( rocketnet_client_transport_create_internal( NULL, token, &client_config, 1000.0, &status ) == NULL );
        if ( status != refusal->status )
        {
            printf( "client transport create, %s: status %d, expected %d\n", refusal->what, status, refusal->status );
        }
        check( status == refusal->status );

        struct rocketnet_server_transport_config_t server_config;
        rocketnet_server_transport_default_config_current( &server_config );
        memcpy( server_config.packet_filter, link_mtus_packet_filter, sizeof( server_config.packet_filter ) );
        link_mtus_set( server_config.link_mtus, &server_config.num_link_mtus, refusal->entries, refusal->count );
        status = ROCKETNET_STATUS_OK;
        check( rocketnet_server_transport_create_internal( NULL, "127.0.0.1:12345", &server_config, 1000.0, &status ) == NULL );
        if ( status != refusal->status )
        {
            printf( "server transport create, %s: status %d, expected %d\n", refusal->what, status, refusal->status );
        }
        check( status == refusal->status );
    }

    rocketnet_quiet( 0 );
}

// Valid lists create on both sides: a three-step list down to the smallest
// entry, 576, once the message window holds the largest message there (1 MiB
// is 2185 fragments of 480 bytes, inside a window of 4096); a two-step list
// at 1109, the smallest entry the default window of 1024 holds; and a full
// list of eight.

static void link_mtus_creates( const int * entries, int count, int window )
{
    struct rocketnet_client_config_t client_config;
    link_mtus_client_config( &client_config );
    link_mtus_set( client_config.link_mtus, &client_config.num_link_mtus, entries, count );
    client_config.reliable_messages_in_flight = window;
    client_config.packets_in_flight = window;
    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    if ( client == NULL || status != ROCKETNET_STATUS_OK )
    {
        printf( "client create, %d entries down to %d, window %d: status %d\n", count, entries[count - 1], window, (int) status );
    }
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );

    struct rocketnet_server_config_t server_config;
    link_mtus_server_config( &server_config );
    link_mtus_set( server_config.link_mtus, &server_config.num_link_mtus, entries, count );
    server_config.reliable_messages_in_flight = window;
    server_config.packets_in_flight = window;
    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    if ( server == NULL || status != ROCKETNET_STATUS_OK )
    {
        printf( "server create, %d entries down to %d, window %d: status %d\n", count, entries[count - 1], window, (int) status );
    }
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rn_test_server_stop_and_destroy( server );
}

static void test_link_mtus_valid_list_creates()
{
    const int three_step[] = { 1492, 1200, 576 };
    link_mtus_creates( three_step, 3, 4096 );

    const int two_step[] = { 1500, 1109 };
    link_mtus_creates( two_step, 2, 1024 );

    link_mtus_creates( link_mtus_full, ROCKETNET_MAX_LINK_MTUS, 2048 );
}

// An older caller's config ends before the list: what lies past its size is
// never read, and the list takes its default {1500}. The transport configs
// read through their KNOWN_BYTES tables; the public configs through
// config_bytes, at create (the tail is 0xCD bytes, a count no create
// accepts, so a create that reads it would refuse).

static void test_link_mtus_older_config_reads_the_default()
{
    {
        static const size_t known[] = { ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES };
        struct rocketnet_client_transport_config_t defaults, source, read;
        rocketnet_client_transport_default_config( &defaults );
        memset( &source, 0xCD, sizeof( source ) );
        memcpy( &source, &defaults, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2 );
        memset( &read, 0xAB, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &source, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_2, known, LINK_MTUS_COUNT( known ) ) == 1 );
        check( read.num_link_mtus == 1 );
        check( read.link_mtus[0] == 1500 );

        struct rocketnet_client_transport_config_t sized;
        memset( &sized, 0xCD, sizeof( sized ) );
        check( rocketnet_client_transport_default_config_sized( &sized, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_3 ) == 1 );
        check( sized.num_link_mtus == 1 );
        check( sized.link_mtus[0] == 1500 );
    }

    {
        static const size_t known[] = { ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES };
        struct rocketnet_server_transport_config_t defaults, source, read;
        rocketnet_server_transport_default_config_current( &defaults );
        memset( &source, 0xCD, sizeof( source ) );
        memcpy( &source, &defaults, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_8 );
        memset( &read, 0xAB, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_server_transport_config_t", &read, &defaults, sizeof( defaults ), &source, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_8, known, LINK_MTUS_COUNT( known ) ) == 1 );
        check( read.num_link_mtus == 1 );
        check( read.link_mtus[0] == 1500 );

        struct rocketnet_server_transport_config_t sized;
        memset( &sized, 0xCD, sizeof( sized ) );
        check( rocketnet_server_transport_default_config_sized( &sized, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_9 ) == 1 );
        check( sized.num_link_mtus == 1 );
        check( sized.link_mtus[0] == 1500 );
    }

    {
        struct rocketnet_client_config_t old_caller;
        memset( &old_caller, 0xCD, sizeof( old_caller ) );
        old_caller.config_bytes = ROCKETNET_CLIENT_CONFIG_BYTES_4;
        check( rocketnet_client_default_config_internal( &old_caller ) == ROCKETNET_STATUS_OK );
        memcpy( old_caller.packet_filter, link_mtus_packet_filter, sizeof( old_caller.packet_filter ) );
        check( old_caller.num_link_mtus == (int) 0xCDCDCDCD );

        enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &old_caller, &status );
        check( client != NULL );
        check( status == ROCKETNET_STATUS_OK );
        rn_test_client_stop_and_destroy( client );
    }

    {
        struct rocketnet_server_config_t full;
        link_mtus_server_config( &full );

        struct rocketnet_server_config_t old_caller;
        memset( &old_caller, 0xCD, sizeof( old_caller ) );
        memcpy( &old_caller, &full, ROCKETNET_SERVER_CONFIG_BYTES_8 );
        old_caller.config_bytes = ROCKETNET_SERVER_CONFIG_BYTES_8;
        check( old_caller.num_link_mtus == (int) 0xCDCDCDCD );

        enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
        struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &old_caller, &status );
        check( server != NULL );
        check( status == ROCKETNET_STATUS_OK );
        rn_test_server_stop_and_destroy( server );
    }
}

#if ROCKETNET_DEV

// the environment form: ROCKETNET_CLIENT_LINK_MTUS and
// ROCKETNET_SERVER_LINK_MTUS, decimal entries joined by commas. A value that
// parses sets the entries and the count; one that does not is
// CONFIG_ENV_INVALID at create. Each variable is restored before returning.

// (an empty value is ignored, as for every config variable)
static const char * link_mtus_env_junk[] =
{
    "a",
    "1492,,576",
    ",1492",
    "1492,",
    " 1200",
    "1200 ",
    "1492;1200",
    "1500,1400,1300,1200,1100,1000,900,800,700",
    "99999999999",
};

static void link_mtus_env_restore( const char * name, bool had, const char * prior )
{
    if ( had )
    {
        rocketnet_test_env_setenv( name, prior, 1 );
    }
    else
    {
        rocketnet_test_env_unsetenv( name );
    }
}

static void test_link_mtus_env_parses_and_refuses_junk()
{
    rocketnet_quiet( 1 );

    char client_prior[4096];
    char server_prior[4096];
    const bool client_had = rocketnet_test_env_getenv( "ROCKETNET_CLIENT_LINK_MTUS", client_prior, sizeof( client_prior ) ) != 0;
    const bool server_had = rocketnet_test_env_getenv( "ROCKETNET_SERVER_LINK_MTUS", server_prior, sizeof( server_prior ) ) != 0;

    rocketnet_test_env_setenv( "ROCKETNET_CLIENT_LINK_MTUS", "1492,1200,576", 1 );
    rocketnet_test_env_setenv( "ROCKETNET_SERVER_LINK_MTUS", "1492,1200,576", 1 );

    {
        struct rocketnet_client_config_t config;
        link_mtus_client_config( &config );
        struct rocketnet_client_transport_config_t transport;
        struct rocketnet_message_config_t message;
        uint32_t kbps = 0;
        check( rocketnet_client_config_resolve( &config, true, &transport, &message, &kbps ) );
        check( transport.num_link_mtus == 3 );
        check( transport.link_mtus[0] == 1492 );
        check( transport.link_mtus[1] == 1200 );
        check( transport.link_mtus[2] == 576 );
    }

    {
        struct rocketnet_server_config_t config;
        link_mtus_server_config( &config );
        struct rocketnet_server_transport_config_t transport;
        struct rocketnet_message_config_t message;
        uint32_t kbps = 0;
        check( rocketnet_server_config_resolve( &config, true, &transport, &message, &kbps ) );
        check( transport.num_link_mtus == 3 );
        check( transport.link_mtus[0] == 1492 );
        check( transport.link_mtus[1] == 1200 );
        check( transport.link_mtus[2] == 576 );
    }

    for ( int i = 0; i < LINK_MTUS_COUNT( link_mtus_env_junk ); i++ )
    {
        rocketnet_test_env_setenv( "ROCKETNET_CLIENT_LINK_MTUS", link_mtus_env_junk[i], 1 );
        rocketnet_test_env_setenv( "ROCKETNET_SERVER_LINK_MTUS", link_mtus_env_junk[i], 1 );

        struct rocketnet_client_config_t client_config;
        link_mtus_client_config( &client_config );
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
        if ( client != NULL || status != ROCKETNET_STATUS_CONFIG_ENV_INVALID )
        {
            printf( "client env \"%s\": status %d\n", link_mtus_env_junk[i], (int) status );
        }
        check( client == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );

        struct rocketnet_server_config_t server_config;
        link_mtus_server_config( &server_config );
        status = ROCKETNET_STATUS_OK;
        struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
        if ( server != NULL || status != ROCKETNET_STATUS_CONFIG_ENV_INVALID )
        {
            printf( "server env \"%s\": status %d\n", link_mtus_env_junk[i], (int) status );
        }
        check( server == NULL );
        check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );
    }

    link_mtus_env_restore( "ROCKETNET_CLIENT_LINK_MTUS", client_had, client_prior );
    link_mtus_env_restore( "ROCKETNET_SERVER_LINK_MTUS", server_had, server_prior );

    rocketnet_quiet( 0 );
}

// the list a create is given is the list the protocol holds, on the client
// and on the server, read through the development seams. The pair runs in
// memory: the loopback transport, step driven, time 1000.0, no socket and no
// clock. The server's transport is made at start, the client's at connect.

#define LINK_MTUS_PROTOCOL 0x4C4D5455504FULL

static double link_mtus_clock( void * ) { return 1000.0; }

static void link_mtus_pair_protocols( const int * entries, int count, int window, int * client_out, int * server_out, int * client_count, int * server_count )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x55 ^ i );
    }
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = LINK_MTUS_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    link_mtus_set( server_config.link_mtus, &server_config.num_link_mtus, entries, count );
    server_config.reliable_messages_in_flight = window;
    server_config.packets_in_flight = window;

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( backend_secret, server_public, LINK_MTUS_PROTOCOL, addresses, 1, 0x4C4D, 1000, 1ULL << 40, 300, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = link_mtus_clock;
    link_mtus_set( client_config.link_mtus, &client_config.num_link_mtus, entries, count );
    client_config.reliable_messages_in_flight = window;
    client_config.packets_in_flight = window;

    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token_string ) == ROCKETNET_STATUS_OK );

    memset( client_out, 0, sizeof( int ) * ROCKETNET_MAX_LINK_MTUS );
    memset( server_out, 0, sizeof( int ) * ROCKETNET_MAX_LINK_MTUS );
    *client_count = rocketnet_client_transport_link_mtus_for_test( rocketnet_message_client_transport( rocketnet_client_message_client( client ) ), client_out );
    *server_count = rocketnet_server_transport_link_mtus_for_test( rocketnet_message_server_transport( rocketnet_server_message_server( server ) ), server_out );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

static void test_link_mtus_reach_the_protocols()
{
    int client_link_mtus[ROCKETNET_MAX_LINK_MTUS];
    int server_link_mtus[ROCKETNET_MAX_LINK_MTUS];
    int client_count = 0;
    int server_count = 0;

    const int three_step[] = { 1492, 1200, 576 };
    link_mtus_pair_protocols( three_step, 3, 4096, client_link_mtus, server_link_mtus, &client_count, &server_count );
    check( client_count == 3 );
    check( client_link_mtus[0] == 1492 );
    check( client_link_mtus[1] == 1200 );
    check( client_link_mtus[2] == 576 );
    check( server_count == 3 );
    check( server_link_mtus[0] == 1492 );
    check( server_link_mtus[1] == 1200 );
    check( server_link_mtus[2] == 576 );

    // the default reaches the protocols as {1500}

    const int one[] = { 1500 };
    link_mtus_pair_protocols( one, 1, 1024, client_link_mtus, server_link_mtus, &client_count, &server_count );
    check( client_count == 1 );
    check( client_link_mtus[0] == 1500 );
    check( server_count == 1 );
    check( server_link_mtus[0] == 1500 );

    check( rocketnet_test_loopback_hub_release() );
}

#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_link_mtus_default_is_1500 );
REGISTER_UNIT_TEST( test_link_mtus_public_create_refusals );
REGISTER_UNIT_TEST( test_link_mtus_transport_create_refusals );
REGISTER_UNIT_TEST( test_link_mtus_valid_list_creates );
REGISTER_UNIT_TEST( test_link_mtus_older_config_reads_the_default );
#if ROCKETNET_DEV
REGISTER_UNIT_TEST( test_link_mtus_env_parses_and_refuses_junk );
REGISTER_UNIT_TEST( test_link_mtus_reach_the_protocols );
#endif // #if ROCKETNET_DEV
