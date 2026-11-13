/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps (round 2, group r2-me) in create refusals and the message
// layer above the transport: the message client's CONFIG_OUT_OF_RANGE
// statuses, the loopback transport's refusal of a config with no transport
// config, the packet builder's refusal of a call with no payloads and its
// undo of a budget-refused packet's level clocks, and the client's public
// messages_resent counter. The loopback transport carries packets in memory;
// no socket, no thread, no real time; clocks start at 1000.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "unit_arena.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_packet_builder.h"
#include "../sdk/rocketnet_send_budget.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// mutation gap: me rocketnet_message_client.cpp:202 *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE deleted
// mutation gap: me rocketnet_message_client.cpp:226 *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE deleted
//
// The message client refuses a send budget above the maximum, a message
// config it cannot size and a budget the bucket refuses, before the
// transport opens anything, and names each CONFIG_OUT_OF_RANGE: the status
// starts as INVALID_ARGUMENT, so a refusal that does not write it reports
// the wrong cause.

static void gaps_message_client_refused( const struct rocketnet_message_config_t * message_config, uint32_t send_kbps, const char * what )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    memset( token, 0, sizeof( token ) );

    int status = ROCKETNET_STATUS_OK;
    struct rocketnet_message_client_t * client =
        rocketnet_message_client_create_internal( NULL, token, NULL, message_config, NULL, 0, send_kbps, &status, 0 );
    check( client == NULL );
    if ( status != ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE )
    {
        printf( "fault: the message client refused %s with status %d, not CONFIG_OUT_OF_RANGE (%d)\n",
                what, status, (int) ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    }
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

static void test_gaps_r2_me_message_client_refusals_are_config_out_of_range()
{
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    gaps_message_client_refused( NULL, (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS + 1, "a send budget above the maximum" );
    gaps_message_client_refused( NULL, 0xFFFFFFFFu, "a send budget of 2^32 - 1" );

    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 0;
    gaps_message_client_refused( &config, 0, "a message config with no channels" );

    rocketnet_log_level( saved_level );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_message_client_refusals_are_config_out_of_range );

// mutation gap: me rocketnet_driver_loopback.cpp:653 config && config->transport -> config || config->transport
//
// An io config with no transport config has no loopback_hub to bind on: the
// loopback create refuses it (the "no loopback_hub" error) and must not read
// through the NULL transport: a crash in this case (SIGSEGV in
// loopback_server_create or loopback_client_create) is that read.

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

static void test_gaps_r2_me_loopback_refuses_a_config_without_transport()
{
    const enum rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const rocketnet_transport_backend_t * backend = &rocketnet_loopback_transport;

    rocketnet_address_t bind;
    check( rocketnet_address_parse( &bind, "127.0.0.1:0" ) );

    const rocketnet_server_transport_io_config_t server_setup = { "loopback", NULL, NULL, NULL };
    rocketnet_server_transport_io_t server = {};
    const bool server_opened = rocketnet_server_transport_io_open( backend->server, NULL, &server_setup, &bind, &server );
    if ( server_opened )
    {
        printf( "fault: the loopback server opened with no transport config and so no loopback_hub\n" );
    }
    check( !server_opened );

    rocketnet_address_t client_bind;
    check( rocketnet_address_parse( &client_bind, "127.0.0.1:0" ) );
    const rocketnet_client_transport_io_config_t client_setup = { "loopback", NULL, NULL };
    rocketnet_client_transport_io_t client = {};
    const bool client_opened = rocketnet_client_transport_io_open( backend->client, NULL, &client_setup, &client_bind, &client );
    if ( client_opened )
    {
        printf( "fault: the loopback client opened with no transport config and so no loopback_hub\n" );
    }
    check( !client_opened );

    rocketnet_log_level( saved_level );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_loopback_refuses_a_config_without_transport );

// ---- the packet builder ------------------------------------------------------

static rocketnet_counters_t gaps_builder_counters;

static void gaps_builder_item( struct rn_packet_item_t * item, uint16_t channel, int code, int send_class, const uint8_t * data, int bytes )
{
    memset( item, 0, sizeof( *item ) );
    item->channel = channel;
    item->code = (uint8_t) code;
    item->send_class = (uint8_t) send_class;
    item->data = data;
    item->bytes = (uint32_t) bytes;
    item->number = ROCKETNET_PACKET_NUMBER_NONE;
    item->descriptor = RN_DESCRIPTOR_NONE;
}

// mutation gap: me rocketnet_packet_builder.cpp:448 num_payloads < 1 -> num_payloads < 0
//
// A call with no payload frames to write into writes nothing and touches
// nothing: in particular a plan whose items are all taken already must not
// reset the scheduler's level clocks, which carry the priority weights from
// one update to the next.

static void test_gaps_r2_me_packet_builder_no_payloads_touches_nothing()
{
    memset( &gaps_builder_counters, 0, sizeof( gaps_builder_counters ) );
    rn_arena_t * arena = rn_unit_arena_create( NULL, 1, &gaps_builder_counters );
    check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_packet_builder_t builder;
    check( rn_packet_builder_init( &builder, region, NULL, NULL, 4, 1191 ) );

    // clocks well away from their seeds, as several updates leave them

    const uint64_t clocks[ROCKETNET_PACKET_BUILDER_LEVELS] = { 101, 205, 314, 435 };
    for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
    {
        builder.level_clock[l] = clocks[l];
    }
    builder.clock_base = 100;

    static uint8_t data[32];
    memset( data, 0x5A, sizeof( data ) );
    struct rn_packet_item_t item;
    gaps_builder_item( &item, 0, ROCKETNET_MESSAGE_CODE_UNRELIABLE, ROCKETNET_SEND_CLASS_UNRELIABLE, data, 32 );
    item.taken = 1;

    static uint8_t payload[1192];
    uint8_t * frames[1] = { payload };

    check( rn_packet_builder_write( &builder, &item, 1, frames, 0 ) == 0 );

    bool unchanged = builder.clock_base == 100;
    for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
    {
        unchanged = unchanged && builder.level_clock[l] == clocks[l];
    }
    if ( !unchanged )
    {
        printf( "fault: a write with no payload frames changed the scheduler clocks (level 0 %llu, base %llu; were 101, 100)\n",
                (unsigned long long) builder.level_clock[0], (unsigned long long) builder.clock_base );
    }
    check( unchanged );

    rn_unit_arena_destroy( arena );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_packet_builder_no_payloads_touches_nothing );

// mutation gap: me rocketnet_packet_builder.cpp:618 for ( l = 0 ... ) -> for ( l = 1 ... )
//
// A packet the budget refuses is undone whole: its items go back, and every
// level clock its takes moved returns to its value before the packet. A
// level-0 (IMMEDIATE) item moves level_clock[0].

static void test_gaps_r2_me_packet_builder_refused_packet_restores_level_zero()
{
    memset( &gaps_builder_counters, 0, sizeof( gaps_builder_counters ) );
    rn_arena_t * arena = rn_unit_arena_create( NULL, 1, &gaps_builder_counters );
    check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );

    struct rocketnet_send_budget_t poor;
    check( rocketnet_send_budget_init( &poor, ROCKETNET_SEND_BUDGET_MIN_KBPS, 1000.0 ) );
    poor.tokens = 1.0;                                          // less than any packet's wire bytes

    rn_packet_builder_t builder;
    check( rn_packet_builder_init( &builder, region, NULL, &poor, 4, 1191 ) );

    uint64_t before[ROCKETNET_PACKET_BUILDER_LEVELS];
    for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
    {
        before[l] = builder.level_clock[l];
    }
    const uint64_t base_before = builder.clock_base;

    static uint8_t data[32];
    memset( data, 0x6B, sizeof( data ) );
    struct rn_packet_item_t item;
    gaps_builder_item( &item, 0, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_SEND_CLASS_NEW_RELIABLE, data, 32 );
    item.number = 0;
    item.priority = ROCKETNET_PRIORITY_IMMEDIATE;

    static uint8_t payload[1192];
    uint8_t * frames[1] = { payload };

    check( rn_packet_builder_write( &builder, &item, 1, frames, 1 ) == 0 );
    check( item.taken == 0 );

    for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
    {
        if ( builder.level_clock[l] != before[l] )
        {
            printf( "fault: a budget-refused packet left level_clock[%d] at %llu, not %llu: its take was not undone\n",
                    l, (unsigned long long) builder.level_clock[l], (unsigned long long) before[l] );
        }
        check( builder.level_clock[l] == before[l] );
    }
    check( builder.clock_base == base_before );

    rn_unit_arena_destroy( arena );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_packet_builder_refused_packet_restores_level_zero );

#if ROCKETNET_DEV

#include "public_step_pair.h"

// mutation gap: me rocketnet_api.cpp:2233 stats->messages_resent = counts[RN_CLIENT_MESSAGES_RESENT] deleted (dev-only rig, production line)
//
// The client's public messages_resent counts its reliable resends. A
// connected client sends one reliable message while the server is not
// updated, so no ack comes back: after the initial resend delay the client
// sends it again, and its public stats say so.

static void test_gaps_r2_me_client_stats_count_resends()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    public_step_connect( &pair );

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( pair.client, &stats );
    const uint64_t before = stats.messages_resent;

    uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 );
    check( out != NULL );
    memset( out, 0xC4, 16 );
    check( rocketnet_client_finish_message( pair.client, 16 ) != 0 );

    // the client alone, past the initial resend delay (default 2000 ms)

    for ( int step = 0; step < 300; step++ )
    {
        public_step_now = pair.time;
        rocketnet_client_update( pair.client, pair.time );
        rocketnet_client_send_packets( pair.client );
        pair.time += 0.01;
    }

    rocketnet_client_stats( pair.client, &stats );
    if ( stats.messages_resent <= before )
    {
        printf( "fault: the client resent an unacknowledged reliable message but its public messages_resent stayed %llu\n",
                (unsigned long long) stats.messages_resent );
    }
    check( stats.messages_resent > before );

    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_client_stats_count_resends );

#endif // #if ROCKETNET_DEV
