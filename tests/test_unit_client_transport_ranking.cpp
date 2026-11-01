/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client transport order (sdk/rocketnet_drivers.inc, the client column;
// TRANSPORTS.md "Client transports"): on every OS the native client ranks
// first and portable ranks last, the fallback; loopback is pin-only.
//
//   * test_client_ranking_portable_is_last_everywhere: the registry's client
//     order for every platform bit, read from the rows, not from this build;
//   * test_client_ranking_auto_picks_native_client: an unpinned create on
//     this OS opens the native client and nothing else;
//   * test_client_ranking_falls_back_to_portable: a native client whose
//     create fails yields portable, the refusal logged once, and a refused
//     portable exhausts the ladder;
//   * test_client_ranking_pin_is_honoured: each pin gives that client or a
//     status, never a silent substitute.
//
// The creates run on the caller's fake operations (tests/fake_client_driver.h)
// in place of every candidate's backend, with the candidate's name in the
// create's config, so no socket opens. No thread, no real time: the clock
// starts at 1000.0.
//

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "test_env_source.h"
#if ROCKETNET_DEV
#include "fake_client_driver.h"
#include "portable_crt.h"
#endif // #if ROCKETNET_DEV

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_driver.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// the native client of each platform the registry names one for; NULL: the
// platform has none, and portable is its whole automatic order

static const char * ranking_native_client( int platform )
{
    switch ( platform )
    {
        case ROCKETNET_PLATFORM_LINUX:   return "gso";
        case ROCKETNET_PLATFORM_MAC:     return "msgx";
        case ROCKETNET_PLATFORM_WINDOWS: return "iocp";
        default:                         return NULL;
    }
}

// ---- the registry ---------------------------------------------------------------

// the automatic client order the registry gives one platform: rows on that
// platform with a client rank below pin-only, lowest rank first, ties in
// row order (the insertion order of sdk/rocketnet_driver.cpp)

static int ranking_client_order( int platform, int * order )
{
    int count = 0;

    for ( int id = 0; id < ROCKETNET_NUM_TRANSPORTS; id++ )
    {
        const rocketnet_transport_definition_t * definition = rocketnet_transport_definition( id );
        check( definition != NULL );
        if ( !( definition->platforms & ( 1u << platform ) ) )
            continue;

        const int rank = definition->client_preference;
        if ( rank == 0 || rank >= ROCKETNET_TRANSPORT_PIN_ONLY )
            continue;

        int index = count++;
        while ( index > 0 && rocketnet_transport_definition( order[index - 1] )->client_preference > rank )
        {
            order[index] = order[index - 1];
            index--;
        }

        order[index] = id;
    }

    return count;
}

static void test_client_ranking_portable_is_last_everywhere()
{
    for ( int platform = 0; platform < ROCKETNET_PLATFORM_MAX; platform++ )
    {
        int order[ROCKETNET_NUM_TRANSPORTS];
        const int count = ranking_client_order( platform, order );
        check( count >= 1 );
        check( order[count - 1] == ROCKETNET_TRANSPORT_PORTABLE );

        const char * native = ranking_native_client( platform );
        if ( native )
        {
            check( count >= 2 );
            check( strcmp( rocketnet_transport_name( order[0] ), native ) == 0 );
        }
        else
        {
            check( count == 1 );
        }
    }

    // loopback is chosen by a pin only, on the client as on the server

    check( rocketnet_transport_definition( ROCKETNET_TRANSPORT_LOOPBACK )->client_preference == ROCKETNET_TRANSPORT_PIN_ONLY );
    check( rocketnet_transport_pin_only( ROCKETNET_TRANSPORT_LOOPBACK, false ) );

    // the native clients rank in the automatic order: none is pin-only

    check( !rocketnet_transport_pin_only( ROCKETNET_TRANSPORT_GSO, false ) );
    check( !rocketnet_transport_pin_only( ROCKETNET_TRANSPORT_MSGX, false ) );

    // this build's own availability list is the registry order for its
    // platform, then the pin-only rows

    int expected[ROCKETNET_NUM_TRANSPORTS];
    const int expected_count = ranking_client_order( ROCKETNET_PLATFORM, expected );
    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );
    check( num_available == expected_count + 1 );
    for ( int i = 0; i < expected_count; i++ )
    {
        check( available[i] == expected[i] );
    }
    check( available[num_available - 1] == ROCKETNET_TRANSPORT_LOOPBACK );
}
REGISTER_UNIT_TEST( test_client_ranking_portable_is_last_everywhere );

// ---- creates on a recording fake -------------------------------------------------

#if ROCKETNET_DEV
#define RANKING_MAX_CREATES 8

// every create the ladder asked for, by name, and the names it refuses
static struct
{
    int num_creates;
    char creates[RANKING_MAX_CREATES][16];
    const char * refuse[2];
} ranking_record;

static void ranking_record_reset( const char * refuse_first, const char * refuse_second )
{
    memset( &ranking_record, 0, sizeof( ranking_record ) );
    ranking_record.refuse[0] = refuse_first;
    ranking_record.refuse[1] = refuse_second;
}

static void * ranking_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    check( config && config->name );
    check( ranking_record.num_creates < RANKING_MAX_CREATES );
    snprintf( ranking_record.creates[ranking_record.num_creates++], sizeof( ranking_record.creates[0] ), "%s", config->name );
    for ( int i = 0; i < 2; i++ )
        if ( ranking_record.refuse[i] && strcmp( ranking_record.refuse[i], config->name ) == 0 )
            return NULL;    // a refusal: no allocation failed, so the ladder falls

    return rocketnet_fake_client_create( context, config, bind );
}

// the log lines one create wrote, warnings and info
#define RANKING_LOG_LINES 64

static struct
{
    int num_lines;
    char lines[RANKING_LOG_LINES][256];
} ranking_log;

static void ranking_log_capture( void *, enum rocketnet_log_level_t, const char * message, size_t )
{
    if ( ranking_log.num_lines < RANKING_LOG_LINES )
    {
        snprintf( ranking_log.lines[ranking_log.num_lines++], sizeof( ranking_log.lines[0] ), "%s", message );
    }
}

static int ranking_log_count( const char * text )
{
    int count = 0;
    for ( int i = 0; i < ranking_log.num_lines; i++ )
        if ( strstr( ranking_log.lines[i], text ) )
            count++;

    return count;
}

static double ranking_clock = 1000.0;
static double ranking_time( void * ) { return ranking_clock; }

// a connect token naming 127.0.0.1:40000, IPv4 only, so no family race opens a
// second endpoint

static void ranking_token( uint8_t * token )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = 1;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 127, 0, 0, 1, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    rocketnet_write_signed_connect_data( &signed_fields, token );
}

// a CI leg's client pin is set aside while these cases name their own. The
// unit suite reads the in-memory environment table, never the process one.
struct ranking_env_t
{
    bool was_set;
    char value[256];
};

static void ranking_env_set_aside( ranking_env_t * saved )
{
    saved->was_set = rocketnet_test_env_getenv( "ROCKETNET_CLIENT_TRANSPORT", saved->value, sizeof( saved->value ) ) != 0;
    rocketnet_test_env_unsetenv( "ROCKETNET_CLIENT_TRANSPORT" );
}

static void ranking_env_put_back( const ranking_env_t * saved )
{
    if ( !saved->was_set )
        return;
    rocketnet_test_env_setenv( "ROCKETNET_CLIENT_TRANSPORT", saved->value, 1 );
}

struct ranking_result_t
{
    bool created;
    int status;
    char name[16];
    int destroy_calls;
};

// one create with the transport list given, on the recording fake, its log
// captured at info; the client is destroyed before returning

static ranking_result_t ranking_create_client( const char * transport, const char * refuse_first, const char * refuse_second )
{
    ranking_record_reset( refuse_first, refuse_second );
    memset( &ranking_log, 0, sizeof( ranking_log ) );

    rocketnet_client_transport_io_operations_t operations = rocketnet_fake_client_operations_io;
    operations.create = ranking_create;

    rocketnet_fake_client_transport_io_t * fake = new rocketnet_fake_client_transport_io_t();

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    ranking_token( token );

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof( config.transport ), "%s", transport );
    config.time_callback = ranking_time;

    const rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( ranking_log_capture, NULL );

    ranking_result_t result;
    memset( &result, 0, sizeof( result ) );
    result.status = -1;
    ranking_clock = 1000.0;

    rocketnet_client_transport_t * client = rocketnet_client_transport_create_step_driven_ex( NULL, token, &config, ranking_clock, &operations, fake, &result.status );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );

    result.created = client != NULL;

    if ( client )
    {
        snprintf( result.name, sizeof( result.name ), "%s", rocketnet_client_transport_io_name( client ) );
        rn_test_client_transport_stop_and_destroy( client );
    }

    result.destroy_calls = fake->destroy_calls;
    delete fake;

    return result;
}

static void test_client_ranking_auto_picks_native_client()
{
    ranking_env_t env;
    ranking_env_set_aside( &env );

    const char * native = ranking_native_client( ROCKETNET_PLATFORM );
    const char * expected = native ? native : "portable";

    const char * lists[] = { "auto", "", " auto " };

    for ( int i = 0; i < 3; i++ )
    {
        const ranking_result_t result = ranking_create_client( lists[i], NULL, NULL );
        check( result.created );
        check( result.status == ROCKETNET_STATUS_OK );
        check( strcmp( result.name, expected ) == 0 );

        // the first rung grants: one create, and it is the native client

        check( ranking_record.num_creates == 1 );
        check( strcmp( ranking_record.creates[0], expected ) == 0 );
        check( ranking_log_count( "refused" ) == 0 );
        check( result.destroy_calls == 1 );
    }

    ranking_env_put_back( &env );
}

static void test_client_ranking_falls_back_to_portable()
{
    ranking_env_t env;
    ranking_env_set_aside( &env );

    const char * native = ranking_native_client( ROCKETNET_PLATFORM );
    if ( native )
    {
        // the native client refuses: the next rung is portable, which grants
        const ranking_result_t result = ranking_create_client( "auto", native, NULL );
        check( result.created );
        check( result.status == ROCKETNET_STATUS_OK );
        check( strcmp( result.name, "portable" ) == 0 );
        check( ranking_record.num_creates == 2 );
        check( strcmp( ranking_record.creates[0], native ) == 0 );
        check( strcmp( ranking_record.creates[1], "portable" ) == 0 );
        check( result.destroy_calls == 1 );

        // the refusal is logged once, and the grant names it once

        char refused[64];
        snprintf( refused, sizeof( refused ), "client transport %s refused", native );
        check( ranking_log_count( refused ) == 1 );

        char granted[96];
        snprintf( granted, sizeof( granted ), "client transport ladder granted portable after refusals: %s", native );
        check( ranking_log_count( granted ) == 1 );
        check( ranking_log_count( "refused" ) == 1 );
    }

    // portable refusing too: the ladder is exhausted, never a pin-only rung
    {
        const ranking_result_t result = ranking_create_client( "auto", native, "portable" );
        check( !result.created );
        check( result.status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        check( ranking_record.num_creates == ( native ? 2 : 1 ) );
        check( strcmp( ranking_record.creates[ranking_record.num_creates - 1], "portable" ) == 0 );
        for ( int i = 0; i < ranking_record.num_creates; i++ )
        {
            check( strcmp( ranking_record.creates[i], "loopback" ) != 0 );
        }
        check( result.destroy_calls == 0 );
    }

    ranking_env_put_back( &env );
}

static void test_client_ranking_pin_is_honoured()
{
    ranking_env_t env;
    ranking_env_set_aside( &env );

    // every client this build has, pinned alone, gives that client and
    // opens nothing else

    int num_available = 0;
    const int * available = rocketnet_client_transport_io_availability( &num_available );

    for ( int i = 0; i < num_available; i++ )
    {
        const char * name = rocketnet_transport_name( available[i] );
        const ranking_result_t result = ranking_create_client( name, NULL, NULL );
        check( result.created );
        check( result.status == ROCKETNET_STATUS_OK );
        check( strcmp( result.name, name ) == 0 );
        check( ranking_record.num_creates == 1 );
        check( strcmp( ranking_record.creates[0], name ) == 0 );

        // the same pin refused is a status, never the next client in the order

        const ranking_result_t refused = ranking_create_client( name, name, NULL );
        check( !refused.created );
        check( refused.status == ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED );
        check( ranking_record.num_creates == 1 );
        check( strcmp( ranking_record.creates[0], name ) == 0 );
    }

    // a list is tried as written: portable first wins over the native client
    {
        const char * native = ranking_native_client( ROCKETNET_PLATFORM );
        char list[64];
        snprintf( list, sizeof( list ), "portable,%s", native ? native : "portable" );

        const ranking_result_t result = ranking_create_client( list, NULL, NULL );
        check( result.created );
        check( strcmp( result.name, "portable" ) == 0 );
        check( ranking_record.num_creates == 1 );
    }

    // a name that is no transport is a status
    {
        const ranking_result_t result = ranking_create_client( "fast", NULL, NULL );
        check( !result.created );
        check( result.status == ROCKETNET_STATUS_INVALID_TRANSPORT_LIST );
        check( ranking_record.num_creates == 0 );
    }

    // a pin of a client another OS has (one fleet-wide line read here) is
    // skipped for the platform, and the endpoint takes its own automatic
    // order loudly, with one warning naming the list
    {
        const char * elsewhere = ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC ? "gso" : "msgx";
        const ranking_result_t result = ranking_create_client( elsewhere, NULL, NULL );
        check( result.created );

        const char * native = ranking_native_client( ROCKETNET_PLATFORM );
        check( strcmp( result.name, native ? native : "portable" ) == 0 );
        check( ranking_log_count( "names no transport for this endpoint on this platform. using auto" ) == 1 );
    }

    ranking_env_put_back( &env );
}

static void test_client_ladder_parse_iocp_on_linux_takes_auto()
{
    ranking_env_t env;
    ranking_env_set_aside( &env );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    // "iocp" is a known Windows client: on a Linux client it is skipped, the
    // list names no candidate, and the endpoint falls back to auto (gso,portable)
    {
        int num_available = 0;
        const int * available = rocketnet_client_transport_io_availability( &num_available );
        struct rocketnet_transport_ladder_t ladder;
        check( rocketnet_transport_ladder_parse( "iocp", available, num_available, &ladder, false ) );
        check( ladder.num_skipped == 1 );
        check( ladder.skipped[0] == ROCKETNET_TRANSPORT_IOCP );
        check( ladder.num_candidates == 2 );
        check( ladder.candidates[0] == ROCKETNET_TRANSPORT_GSO );
        check( ladder.candidates[1] == ROCKETNET_TRANSPORT_PORTABLE );
    }

    {
        const ranking_result_t result = ranking_create_client( "iocp", NULL, NULL );
        check( result.created );
        check( result.status == ROCKETNET_STATUS_OK );
        check( strcmp( result.name, "gso" ) == 0 );
        check( ranking_record.num_creates == 1 );
        check( strcmp( ranking_record.creates[0], "gso" ) == 0 );
        check( ranking_log_count( "names no transport for this endpoint on this platform. using auto" ) == 1 );
        check( result.destroy_calls == 1 );
    }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    ranking_env_put_back( &env );
}

REGISTER_UNIT_TEST( test_client_ranking_auto_picks_native_client );
REGISTER_UNIT_TEST( test_client_ranking_falls_back_to_portable );
REGISTER_UNIT_TEST( test_client_ranking_pin_is_honoured );
REGISTER_UNIT_TEST( test_client_ladder_parse_iocp_on_linux_takes_auto );
#endif // #if ROCKETNET_DEV
