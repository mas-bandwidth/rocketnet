/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_gaps_secrets_keys.cpp -- the server transport's keys at create:
//
//   - the missing-keys check (rocketnet_server_config_has_keys in
//     sdk/rocketnet_server_transport.cpp): each of the three keys all zero is
//     refused with ROCKETNET_STATUS_MISSING_KEYS, written through the status
//     pointer, and each key zero in every byte but its last is a key. The
//     config carries protocol_id 0, so a config that passes the key check is
//     refused at the next check (INVALID_PROTOCOL_ID) before anything is
//     allocated: the two statuses say on which side of the key check a config
//     fell. Registered in every build: the key check runs before the
//     production refusals.
//   - an unparsable public address is refused with INVALID_ADDRESS in every
//     build, written through the status pointer.
//   - a NULL config is the default shape, which has no keys (development).
//   - the DEV secret span seams of the server and client transports: a NULL
//     transport has no spans, span 0 is the server secret key, the server has
//     four spans, and neither seam writes past the caller's max.
//   - the cookie key (development, read through the server transport's span
//     seam): drawn at create, different per server, not rotated before its
//     period, rotated once the period has passed.
//
// Time is an argument starting at 1000.0; the loopback transport opens no
// socket, and the token epoch is configured, so no wall clock is read.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loopback_hub.h"

// a check whose failure names the fault before the condition is printed
#define gaps_keys_check( condition, fault )                                                                       \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
    {                                                                                                             \
        printf( "mutation gap fault: %s\n", fault );                                                              \
        fflush( stdout );                                                                                         \
    }                                                                                                             \
    rocketnet_unit_check( condition );                                                                            \
} while ( 0 )

// a config every build accepts up to the key check: its own packet filter
// (a production build refuses the published test filter), keys that are not
// the published development keys, the loopback transport, and protocol_id 0
// so a config that passes the key check stops at the next check
static void gaps_keys_config( struct rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config( config );
    memset( config->packet_filter, 0x5A, sizeof( config->packet_filter ) );
    memset( config->backend_public_key, 0x42, 32 );
    memset( config->server_public_key, 0x43, 32 );
    memset( config->server_secret_key, 0x44, 32 );
    snprintf( config->transport, sizeof( config->transport ), "%s", "loopback" );
    config->loopback_hub = rocketnet_test_loopback_hub();
    config->max_clients = 1;
    config->protocol_id = 0;
}

static int gaps_keys_create_status( const struct rocketnet_server_transport_config_t * config )
{
    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_internal( NULL, "127.0.0.1:12345", config, 1000.0, &status );
    gaps_keys_check( server == NULL, "a config with protocol_id 0 created a server transport" );
    return status;
}

// mutation gap: hunt 2/4736 hunt 2/4741 hunt 2/4742 hunt 2/4746 hunt 2/4747
// mutation gap: hunt 2/4750 hunt 2/4751 hunt 2/4752 hunt 2/4938
static void test_gaps_secrets_missing_keys_refused()
{
    rocketnet_quiet( 1 );

    // all three keys: the key check passes and protocol_id 0 is what refuses

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID,
                         "a config with all three keys was not passed by the key check" );
    }

    // each key all zero is missing. num_xdp_queues, the field after the
    // secret key, is nonzero, so a compare that runs one byte past the secret
    // key reads a nonzero byte and must not decide the outcome

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.server_secret_key, 0, sizeof( config.server_secret_key ) );
        config.num_xdp_queues = 1;
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_MISSING_KEYS,
                         "an all-zero server_secret_key was not refused as MISSING_KEYS" );
    }

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.server_public_key, 0, sizeof( config.server_public_key ) );
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_MISSING_KEYS,
                         "an all-zero server_public_key was not refused as MISSING_KEYS" );
    }

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.backend_public_key, 0, sizeof( config.backend_public_key ) );
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_MISSING_KEYS,
                         "an all-zero backend_public_key was not refused as MISSING_KEYS" );
    }

    // each key zero in bytes 0..30 with byte 31 set is a key: the check reads
    // all 32 bytes

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.server_secret_key, 0, sizeof( config.server_secret_key ) );
        config.server_secret_key[31] = 1;
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID,
                         "a server_secret_key nonzero only in byte 31 was refused as missing" );
    }

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.server_public_key, 0, sizeof( config.server_public_key ) );
        config.server_public_key[31] = 1;
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID,
                         "a server_public_key nonzero only in byte 31 was refused as missing" );
    }

    {
        struct rocketnet_server_transport_config_t config;
        gaps_keys_config( &config );
        memset( config.backend_public_key, 0, sizeof( config.backend_public_key ) );
        config.backend_public_key[31] = 1;
        gaps_keys_check( gaps_keys_create_status( &config ) == ROCKETNET_STATUS_INVALID_PROTOCOL_ID,
                         "a backend_public_key nonzero only in byte 31 was refused as missing" );
    }

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_missing_keys_refused );

// mutation gap: hunt 2/4950
static void test_gaps_secrets_invalid_address_status()
{
    rocketnet_quiet( 1 );

    struct rocketnet_server_transport_config_t config;
    gaps_keys_config( &config );
    config.protocol_id = 1;

    int status = -1;
    gaps_keys_check( rocketnet_server_transport_create_internal( NULL, "not-an-address", &config, 1000.0, &status ) == NULL,
                     "an unparsable public address created a server transport" );
    gaps_keys_check( status == ROCKETNET_STATUS_INVALID_ADDRESS,
                     "an unparsable public address was refused without INVALID_ADDRESS in the status" );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_invalid_address_status );

#if ROCKETNET_DEV

// mutation gap: hunt 2/4916
static void test_gaps_secrets_null_config_is_the_keyless_default()
{
    rocketnet_quiet( 1 );

    int status = -1;
    gaps_keys_check( rocketnet_server_transport_create_internal( NULL, "127.0.0.1:12345", NULL, 1000.0, &status ) == NULL,
                     "a NULL config (the keyless default) created a server transport" );
    gaps_keys_check( status == ROCKETNET_STATUS_MISSING_KEYS,
                     "a NULL config was not resolved to the default config and refused as MISSING_KEYS" );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_null_config_is_the_keyless_default );

// a started server transport with a known secret key and a configured token
// epoch (no wall clock read at the first update)
static struct rocketnet_server_transport_t * gaps_keys_server( const char * address, double time )
{
    struct rocketnet_server_transport_config_t config;
    gaps_keys_config( &config );
    config.protocol_id = 1;
    config.token_epoch_unix_seconds = 1000000;

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_internal( NULL, address, &config, time, &status );
    if ( !server )
    {
        printf( "server transport create at %s: status %d\n", address, status );
    }
    gaps_keys_check( server != NULL && status == ROCKETNET_STATUS_OK, "the keyed loopback server transport was not created" );
    return server;
}

// span s of the server transport's secret spans, copied out
static void gaps_keys_span( struct rocketnet_server_transport_t * server, int span, uint8_t * out, size_t bytes )
{
    size_t offsets[8];
    size_t sizes[8];
    const int spans = rocketnet_server_transport_secret_spans_for_test( server, offsets, sizes, 8 );
    gaps_keys_check( span < spans && sizes[span] == bytes, "the server transport span seam does not hold the span asked for" );
    memcpy( out, (const uint8_t *) server + offsets[span], bytes );
}

static bool gaps_keys_all_zero( const uint8_t * bytes, size_t count )
{
    for ( size_t i = 0; i < count; i++ )
    {
        if ( bytes[i] != 0 )
        {
            return false;
        }
    }
    return true;
}

// mutation gap: hunt 2/5285 hunt 2/5286 hunt 2/5291 hunt 2/5294 hunt 2/5296 hunt 2/7334
static void test_gaps_secrets_span_seams()
{
    rocketnet_quiet( 1 );

    size_t offsets[8];
    size_t sizes[8];

    gaps_keys_check( rocketnet_server_transport_secret_spans_for_test( NULL, offsets, sizes, 8 ) == 0,
                     "the server transport span seam reported spans for a NULL transport" );

    struct rocketnet_server_transport_t * server = gaps_keys_server( "127.0.0.1:40101", 1000.0 );

    // all four: the secret key, the cookie key, the previous cookie key and
    // the disconnect burst keys, the last a whole number of keys

    for ( int i = 0; i < 8; i++ )
    {
        offsets[i] = 0;
        sizes[i] = 0;
    }
    gaps_keys_check( rocketnet_server_transport_secret_spans_for_test( server, offsets, sizes, 8 ) == 4,
                     "the server transport span seam does not report its four secret spans" );
    gaps_keys_check( sizes[0] == 32, "server transport span 0 is not the 32-byte server secret key" );

    uint8_t expected[32];
    memset( expected, 0x44, sizeof( expected ) );
    gaps_keys_check( memcmp( (const uint8_t *) server + offsets[0], expected, 32 ) == 0,
                     "server transport span 0 does not hold the configured server secret key" );
    gaps_keys_check( sizes[3] > 0 && sizes[3] % 32 == 0,
                     "server transport span 3 is not the disconnect burst key ring (a whole number of keys)" );

    // a max below the count: exactly max entries, nothing past them

    const size_t sentinel = (size_t) 0x5E7715E7;
    for ( int i = 0; i < 8; i++ )
    {
        offsets[i] = sentinel;
        sizes[i] = sentinel;
    }
    gaps_keys_check( rocketnet_server_transport_secret_spans_for_test( server, offsets, sizes, 2 ) == 2,
                     "the server transport span seam returned more spans than max" );
    gaps_keys_check( offsets[2] == sentinel && sizes[2] == sentinel,
                     "the server transport span seam wrote an entry past the caller's max" );

    for ( int i = 0; i < 8; i++ )
    {
        offsets[i] = sentinel;
        sizes[i] = sentinel;
    }
    gaps_keys_check( rocketnet_client_transport_secret_spans_for_test( NULL, offsets, sizes, 2 ) == 2,
                     "the client transport span seam returned more spans than max" );
    gaps_keys_check( offsets[2] == sentinel && sizes[2] == sentinel,
                     "the client transport span seam wrote an entry past the caller's max" );

    rn_test_server_transport_stop_and_destroy( server );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_span_seams );

// mutation gap: hunt 2/5233
static void test_gaps_secrets_cookie_key_drawn_per_server()
{
    rocketnet_quiet( 1 );

    // one after the other: the loopback hub's budget holds one server

    uint8_t key_a[32];
    uint8_t key_b[32];

    struct rocketnet_server_transport_t * a = gaps_keys_server( "127.0.0.1:40102", 1000.0 );
    gaps_keys_span( a, 1, key_a, sizeof( key_a ) );
    rn_test_server_transport_stop_and_destroy( a );

    struct rocketnet_server_transport_t * b = gaps_keys_server( "127.0.0.1:40103", 1000.0 );
    gaps_keys_span( b, 1, key_b, sizeof( key_b ) );
    rn_test_server_transport_stop_and_destroy( b );

    const bool a_zero = gaps_keys_all_zero( key_a, sizeof( key_a ) );
    const bool b_zero = gaps_keys_all_zero( key_b, sizeof( key_b ) );
    const bool same = memcmp( key_a, key_b, sizeof( key_a ) ) == 0;

    gaps_keys_check( !a_zero && !b_zero, "a server transport's cookie key is all zero at create (a known, shared cookie key)" );
    gaps_keys_check( !same, "two server transports were created with the same cookie key" );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_cookie_key_drawn_per_server );

// mutation gap: hunt 2/5131 hunt 2/5240 hunt 2/5241
static void test_gaps_secrets_cookie_key_rotates_on_its_period()
{
    rocketnet_quiet( 1 );

    struct rocketnet_server_transport_t * server = gaps_keys_server( "127.0.0.1:40104", 1000.0 );

    uint8_t first[32];
    uint8_t now[32];
    uint8_t previous[32];
    gaps_keys_span( server, 1, first, sizeof( first ) );

    // a tenth of a second after create: no rotation

    rocketnet_server_transport_update( server, 1000.1 );
    gaps_keys_span( server, 1, now, sizeof( now ) );
    gaps_keys_span( server, 2, previous, sizeof( previous ) );
    const bool rotated_early = memcmp( first, now, sizeof( first ) ) != 0;
    const bool previous_early = !gaps_keys_all_zero( previous, sizeof( previous ) );

    // past the 60-second rotation period from create: rotated once, the
    // first key is now the previous key

    rocketnet_server_transport_update( server, 1061.0 );
    gaps_keys_span( server, 1, now, sizeof( now ) );
    gaps_keys_span( server, 2, previous, sizeof( previous ) );
    const bool rotated = memcmp( first, now, sizeof( first ) ) != 0;
    const bool previous_is_first = memcmp( first, previous, sizeof( first ) ) == 0;

    rn_test_server_transport_stop_and_destroy( server );

    gaps_keys_check( !rotated_early, "the cookie key rotated 0.1 s after create, before its rotation period" );
    gaps_keys_check( !previous_early, "a previous cookie key was set 0.1 s after create, before any rotation" );
    gaps_keys_check( rotated, "the cookie key did not rotate once its 60 s rotation period had passed" );
    gaps_keys_check( previous_is_first, "after the first rotation the previous cookie key is not the key drawn at create" );

    rocketnet_quiet( 0 );
}

REGISTER_UNIT_TEST( test_gaps_secrets_cookie_key_rotates_on_its_period );

#endif // #if ROCKETNET_DEV
