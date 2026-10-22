/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The load_split delivery capture (tools/load_split_capture.h) over a fake
// source, and the SDK's delivery capture (rocketnet_layers.h, development
// builds) over a public server and step-driven client on the loopback
// transport. Callers supply every time from 1000.0; nothing sleeps or opens
// a socket.

#include "../sdk/rocketnet.h"

// The capture reads the development library only (ROCKETNET_DEV).
#if ROCKETNET_DEV

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_config.h"

#include "../tools/load_split_capture.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define CAPTURE_CONNECTIONS 3
#define CAPTURE_LINES 512
#define CAPTURE_LINE_BYTES SPLIT_CAPTURE_LINE_BYTES

// ---------------------------------------------------------------
//                     the fake source and emitter
// ---------------------------------------------------------------

struct capture_fake_t
{
    int exists[CAPTURE_CONNECTIONS];
    int connected[CAPTURE_CONNECTIONS];
    int state[CAPTURE_CONNECTIONS];
    uint64_t received_sequence[CAPTURE_CONNECTIONS];
    uint64_t acked_sequence[CAPTURE_CONNECTIONS];
    double last_receive_age[CAPTURE_CONNECTIONS];
    uint64_t global[ROCKETNET_NUM_COUNTERS];
    uint64_t own[CAPTURE_CONNECTIONS][ROCKETNET_NUM_COUNTERS];
    int samples;

    char lines[CAPTURE_LINES][CAPTURE_LINE_BYTES];
    int num_lines;
};

static int capture_fake_sample( void * context, int index, double now, struct split_capture_sample_t * out )
{
    capture_fake_t * fake = (capture_fake_t *) context;
    fake->samples++;
    out->time = now;
    if ( !fake->exists[index] )
        return 0;
    out->state = fake->state[index];
    out->packets_sent = 100 + (uint64_t) index;
    out->packets_received = 90 + (uint64_t) index;
    out->delivery.connected = fake->connected[index];
    if ( fake->connected[index] )
    {
        out->delivery.server_mapping_known = 1;
        out->delivery.server_slot = index + 20;
        snprintf( out->delivery.remote_endpoint, sizeof( out->delivery.remote_endpoint ), "192.0.2.9:%d", 40000 + index );
        out->delivery.highest_received_sequence = fake->received_sequence[index];
        out->delivery.highest_sent_sequence = 500;
        out->delivery.largest_acked_sequence = fake->acked_sequence[index];
        out->delivery.last_receive_age_seconds = fake->last_receive_age[index];
        out->delivery.reliable_unacked = 2;
        out->delivery.reliable_oldest_unacked_age_seconds = 0.5;
    }
    return 1;
}

static uint64_t capture_fake_counter( void * context, int index, int counter )
{
    const capture_fake_t * fake = (const capture_fake_t *) context;
    if ( counter < 0 || counter >= ROCKETNET_NUM_COUNTERS )
        return 0;
    return index < 0 ? fake->global[counter] : fake->own[index][counter];
}

static void capture_fake_line( void * context, const char * line )
{
    capture_fake_t * fake = (capture_fake_t *) context;
    check( fake->num_lines < CAPTURE_LINES );
    snprintf( fake->lines[fake->num_lines], CAPTURE_LINE_BYTES, "%s", line );
    fake->num_lines++;
}

static void capture_fake_init( capture_fake_t * fake )
{
    memset( fake, 0, sizeof( *fake ) );
    for ( int i = 0; i < CAPTURE_CONNECTIONS; i++ )
    {
        fake->exists[i] = 1;
        fake->connected[i] = 1;
        fake->state[i] = ROCKETNET_CLIENT_STATE_CONNECTED;
        fake->received_sequence[i] = 40;
        fake->last_receive_age[i] = 0.0;
    }
}

static void capture_start( struct split_capture_t * capture, struct split_capture_entry_t * entries, capture_fake_t * fake,
                           const char * role, int per_connection )
{
    const struct split_capture_source_t source = { fake, capture_fake_sample, capture_fake_counter, per_connection };
    const struct split_capture_emit_t emit = { fake, capture_fake_line };
    split_capture_init( capture, role, 0x957b7d7a91c89091ULL, 7, CAPTURE_CONNECTIONS, entries, &source, &emit );
}

// lines with this prefix that contain every one of the fragments
static int capture_count( const capture_fake_t * fake, const char * prefix, const char * a, const char * b = NULL, const char * c = NULL )
{
    int found = 0;
    for ( int i = 0; i < fake->num_lines; i++ )
    {
        const char * line = fake->lines[i];
        if ( strncmp( line, prefix, strlen( prefix ) ) != 0 ) continue;
        if ( a && !strstr( line, a ) ) continue;
        if ( b && !strstr( line, b ) ) continue;
        if ( c && !strstr( line, c ) ) continue;
        found++;
    }
    return found;
}

// every field is key=value and no key repeats: the evidence parser
// (tools/check_split_load.py records) refuses a line otherwise
static int capture_line_well_formed( const char * line )
{
    const char * colon = strchr( line, ':' );
    if ( !colon || colon[1] != ' ' ) return 0;
    char keys[64][48];
    int num_keys = 0;
    const char * p = colon + 2;
    while ( *p )
    {
        const char * end = strchr( p, ' ' );
        const size_t length = end ? (size_t) ( end - p ) : strlen( p );
        const char * equals = (const char *) memchr( p, '=', length );
        if ( !equals || equals == p || equals == p + length - 1 ) return 0;
        const size_t key_length = (size_t) ( equals - p );
        if ( key_length >= sizeof( keys[0] ) || num_keys == 64 ) return 0;
        memcpy( keys[num_keys], p, key_length );
        keys[num_keys][key_length] = '\0';
        for ( int k = 0; k < num_keys; k++ )
            if ( strcmp( keys[k], keys[num_keys] ) == 0 ) return 0;
        num_keys++;
        p = end ? end + 1 : p + length;
    }
    return num_keys > 0;
}

// ---------------------------------------------------------------
//                     the capture over a fake source
// ---------------------------------------------------------------

static void test_load_split_capture_samples_once_per_interval()
{
    static capture_fake_t fake;
    capture_fake_init( &fake );
    struct split_capture_entry_t entries[CAPTURE_CONNECTIONS];
    struct split_capture_t capture;
    capture_start( &capture, entries, &fake, "server", 0 );

    check( split_capture_tick( &capture, 1000.0 ) == 0 );
    check( fake.samples == CAPTURE_CONNECTIONS );
    check( split_capture_tick( &capture, 1000.0 + SPLIT_CAPTURE_INTERVAL_S * 0.5 ) == 0 );
    check( fake.samples == CAPTURE_CONNECTIONS );
    check( split_capture_tick( &capture, 1000.0 + SPLIT_CAPTURE_INTERVAL_S ) == 0 );
    check( fake.samples == 2 * CAPTURE_CONNECTIONS );
    check( fake.num_lines == 0 );                    // a quiet run prints nothing until the end
}

static void test_load_split_capture_prints_the_last_connected_sample_at_a_kick()
{
    // connection 1 is ended between two samples: the SDK has reset the slot,
    // so the kick line carries the sample from before, aged at the print
    static capture_fake_t fake;
    capture_fake_init( &fake );
    struct split_capture_entry_t entries[CAPTURE_CONNECTIONS];
    struct split_capture_t capture;
    capture_start( &capture, entries, &fake, "server", 0 );
    fake.received_sequence[1] = 77;
    fake.last_receive_age[1] = 0.5;
    fake.global[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE] = 3;
    fake.global[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] = 9;
    check( split_capture_tick( &capture, 1000.0 ) == 0 );

    fake.connected[1] = 0;
    fake.state[1] = ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT;
    fake.received_sequence[1] = 0;
    check( split_capture_tick( &capture, 1001.0 ) == 1 );
    check( capture_count( &fake, "split_capture: ", "why=kick" ) == 1 );
    check( capture_count( &fake, "split_capture: ", "client=8 ", "why=kick", "received_sequence=77 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "why=kick", "sample_age_s=1.000 ", "last_receive_age_s=0.500 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "why=kick", "server_slot=21 ", "remote_endpoint=192.0.2.9:40001 " ) == 1 );
    char state_now[48];
    snprintf( state_now, sizeof( state_now ), "state_now=%d ", (int) ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT );
    check( capture_count( &fake, "split_capture: ", "why=kick", state_now ) == 1 );

    // the side's counters once: every named one, zero or not, and every
    // other one only when nonzero
    check( capture_count( &fake, "split_counter: ", "client=all " ) == SPLIT_CAPTURE_NUM_NAMED + 1 );
    check( capture_count( &fake, "split_counter: ", "name=ACK_UNKNOWN_SEQUENCE ", "value=3" ) == 1 );
    check( capture_count( &fake, "split_counter: ", "name=SEND_RING_FULL ", "value=0" ) == 1 );
    check( capture_count( &fake, "split_counter: ", "name=CONNECTION_REQUEST_RECEIVED ", "value=9" ) == 1 );

    // a kick prints once: later ticks find it still ended and stay quiet
    const int lines = fake.num_lines;
    check( split_capture_tick( &capture, 1002.0 ) == 0 );
    check( fake.num_lines == lines );
    for ( int i = 0; i < fake.num_lines; i++ )
        check( capture_line_well_formed( fake.lines[i] ) );
}

static void test_load_split_capture_marks_acks_past_run()
{
    // RUN's mark is the first sequence sent after RUN was queued: an
    // acknowledgement at or past it is yes, below it no, no mark na
    static capture_fake_t fake;
    capture_fake_init( &fake );
    struct split_capture_entry_t entries[CAPTURE_CONNECTIONS];
    struct split_capture_t capture;
    capture_start( &capture, entries, &fake, "server", 0 );
    split_capture_mark_run( &capture, 0, 1000.25, 120 );
    split_capture_mark_run( &capture, 1, 1000.25, 120 );
    split_capture_mark_run( &capture, CAPTURE_CONNECTIONS, 1000.25, 120 );   // out of range: ignored
    fake.acked_sequence[0] = 120;
    fake.acked_sequence[1] = 119;
    split_capture_end( &capture, 1010.25 );
    check( capture_count( &fake, "split_capture: ", "client=7 ", "acked_past_run=yes", "run_age_s=10.000 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "client=8 ", "acked_past_run=no", "run_sequence=120 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "client=9 ", "acked_past_run=na", "run_age_s=-1.000 " ) == 1 );
}

static void test_load_split_capture_end_names_every_connection()
{
    // the end prints every connection: a live one sampled now, an ended one
    // from its last connected sample, one never connected from nothing; then
    // the counters, and on a side with a block per connection, each
    // connection's nonzero named counters from its last connected sample
    static capture_fake_t fake;
    capture_fake_init( &fake );
    fake.exists[2] = 0;
    struct split_capture_entry_t entries[CAPTURE_CONNECTIONS];
    struct split_capture_t capture;
    capture_start( &capture, entries, &fake, "client-shard", 1 );
    fake.own[1][ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED] = 4;
    check( split_capture_tick( &capture, 1000.0 ) == 0 );

    fake.connected[1] = 0;
    fake.own[1][ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED] = 0;   // the ended connection's block is gone
    fake.received_sequence[0] = 41;
    split_capture_end( &capture, 1000.5 );
    check( capture_count( &fake, "split_capture: ", "client=7 ", "why=end ", "received_sequence=41 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "client=8 ", "why=end_ended ", "received_sequence=40 " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "client=9 ", "why=end_never " ) == 1 );
    check( capture_count( &fake, "split_capture: ", "why=end_never ", "server_slot=na ", "remote_endpoint=na " ) == 1 );
    check( capture_count( &fake, "split_counter: ", "client=8 ", "name=CLIENT_PAYLOAD_ALREADY_RECEIVED ", "value=4" ) == 1 );
    check( capture_count( &fake, "split_counter: ", "client=7 " ) == 0 );
    check( capture_count( &fake, "split_counter: ", "client=all " ) == SPLIT_CAPTURE_NUM_NAMED );
    check( capture_count( &fake, "split_capture: ", "messages_resent=na:0 " ) == CAPTURE_CONNECTIONS );
    for ( int i = 0; i < fake.num_lines; i++ )
        check( capture_line_well_formed( fake.lines[i] ) );
}

static void test_load_split_capture_kick_prints_only_the_kicked_connection_counters()
{
    // a client shard keeps a block per connection: a kick inside the run
    // prints the side totals and the kicked connection's own counters, never
    // every connection's, so the tick that finds it stays small
    static capture_fake_t fake;
    capture_fake_init( &fake );
    struct split_capture_entry_t entries[CAPTURE_CONNECTIONS];
    struct split_capture_t capture;
    capture_start( &capture, entries, &fake, "client-shard", 1 );
    for ( int i = 0; i < CAPTURE_CONNECTIONS; i++ )
        fake.own[i][ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED] = 10 + (uint64_t) i;
    check( split_capture_tick( &capture, 1000.0 ) == 0 );
    fake.connected[2] = 0;
    check( split_capture_tick( &capture, 1001.0 ) == 1 );
    check( capture_count( &fake, "split_counter: ", "client=9 ", "name=CLIENT_PAYLOAD_DELIVERED ", "value=12" ) == 1 );
    check( capture_count( &fake, "split_counter: ", "client=7 " ) == 0 );
    check( capture_count( &fake, "split_counter: ", "client=8 " ) == 0 );
    check( capture_count( &fake, "split_counter: ", "client=all " ) == SPLIT_CAPTURE_NUM_NAMED );
}

static void test_load_split_capture_named_counters_are_distinct()
{
    for ( int i = 0; i < SPLIT_CAPTURE_NUM_NAMED; i++ )
    {
        check( split_capture_named_counters[i] >= 0 && split_capture_named_counters[i] < rocketnet_num_counters() );
        check( split_capture_is_named( split_capture_named_counters[i] ) );
        for ( int j = 0; j < i; j++ )
            check( split_capture_named_counters[i] != split_capture_named_counters[j] );
    }
}

REGISTER_UNIT_TEST( test_load_split_capture_samples_once_per_interval );
REGISTER_UNIT_TEST( test_load_split_capture_prints_the_last_connected_sample_at_a_kick );
REGISTER_UNIT_TEST( test_load_split_capture_marks_acks_past_run );
REGISTER_UNIT_TEST( test_load_split_capture_end_names_every_connection );
REGISTER_UNIT_TEST( test_load_split_capture_kick_prints_only_the_kicked_connection_counters );
REGISTER_UNIT_TEST( test_load_split_capture_named_counters_are_distinct );

// ---------------------------------------------------------------
//               the SDK's delivery capture over loopback
// ---------------------------------------------------------------

#define CAPTURE_PROTOCOL 0x51E7D41A92B3C6D9ULL

struct capture_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
};

static double capture_now = 1000.0;
static double capture_clock( void * ) { return capture_now; }

struct capture_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double time;
};

static void capture_pair_create( capture_pair_t * pair, capture_identity_t * identity )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    capture_now = 1000.0;
    rocketnet_quiet( 1 );
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5A ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = CAPTURE_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, identity->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( pair->server, server_public );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair->server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public, CAPTURE_PROTOCOL, addresses, 1,
        0x5EED, 1000, 1ULL << 40, 300, NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    rocketnet_client_test_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = capture_clock;
    pair->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair->client != NULL );
    check( rocketnet_client_connect( pair->client, token_string ) == ROCKETNET_STATUS_OK );
}

static void capture_pair_pump( capture_pair_t * pair )
{
    capture_now = pair->time;
    rocketnet_client_update( pair->client, pair->time );
    rocketnet_client_send_packets( pair->client );
    rocketnet_server_update( pair->server, pair->time );
    rocketnet_server_send_packets( pair->server );
    pair->time += 0.01;
}

static void test_load_split_capture_sdk_reads_both_ends()
{
    capture_identity_t identity;
    capture_pair_t pair;
    capture_pair_create( &pair, &identity );

    // before the connection: both read as zeros, not connected
    struct rocketnet_delivery_capture_t server_side, client_side;
    rocketnet_server_delivery_capture( pair.server, 0, &server_side );
    rocketnet_client_delivery_capture( pair.client, &client_side );
    check( !server_side.connected && server_side.highest_sent_sequence == 0 );
    check( !server_side.server_mapping_known && !server_side.remote_endpoint[0] );
    check( !client_side.connected && client_side.highest_sent_sequence == 0 );

    for ( int i = 0; i < 400 && ( rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
                                  !rocketnet_server_client_connected( pair.server, 0 ) ); i++ )
        capture_pair_pump( &pair );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // one reliable message the server holds unacknowledged until the client
    // has it and says so
    uint8_t * out = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memset( out, 7, 4 );
    check( rocketnet_server_finish_message( pair.server, 4 ) != 0 );
    rocketnet_server_send_packets( pair.server );
    rocketnet_server_delivery_capture( pair.server, 0, &server_side );
    check( server_side.connected );
    check( server_side.server_mapping_known && server_side.server_slot == 0 );
    check( strncmp( server_side.remote_endpoint, "127.0.0.1:", 10 ) == 0 );
    check( server_side.reliable_unacked == 1 );
    const uint64_t sent_with_message = server_side.highest_sent_sequence;
    check( sent_with_message > 0 );

    for ( int i = 0; i < 100; i++ )
        capture_pair_pump( &pair );
    rocketnet_server_delivery_capture( pair.server, 0, &server_side );
    rocketnet_client_delivery_capture( pair.client, &client_side );
    check( server_side.connected && client_side.connected );
    check( server_side.reliable_unacked == 0 );
    check( server_side.largest_acked_sequence >= sent_with_message );
    check( server_side.highest_received_sequence > 0 && server_side.highest_received_sequence <= client_side.highest_sent_sequence );
    check( client_side.highest_received_sequence > 0 && client_side.highest_received_sequence <= server_side.highest_sent_sequence );
    // both ends heard the other within the last few steps, each on its own clock
    check( server_side.last_receive_age_seconds >= 0.0 && server_side.last_receive_age_seconds <= 0.05 );
    check( client_side.last_receive_age_seconds >= 0.0 && client_side.last_receive_age_seconds <= 0.05 );
    check( server_side.reliable_oldest_unacked_age_seconds < 0.0 );
    check( server_side.srtt_seconds >= 0.0 );

    // a slot out of range and NULL arguments read as not connected
    rocketnet_server_delivery_capture( pair.server, 5, &server_side );
    check( !server_side.connected && !server_side.server_mapping_known && !server_side.remote_endpoint[0] );
    rocketnet_server_delivery_capture( NULL, 0, &server_side );
    check( !server_side.connected && !server_side.server_mapping_known && !server_side.remote_endpoint[0] );

    // the counter blocks are the ones the endpoint counts in
    check( rocketnet_server_transport_counter_block( pair.server ) != NULL );
    check( rocketnet_server_message_counter_block( pair.server ) != NULL );
    check( rocketnet_client_transport_counter_block( pair.client ) != NULL );
    check( rocketnet_client_message_counter_block( pair.client ) != NULL );
    check( rocketnet_counter_value( rocketnet_server_transport_counter_block( pair.server ), ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED ) +
           rocketnet_counter_value( rocketnet_server_transport_counter_block( pair.server ), ROCKETNET_COUNTER_PAYLOAD_RECEIVED ) > 0 );

    rn_test_client_stop_and_destroy( pair.client );
    rn_test_server_stop_and_destroy( pair.server );
}

REGISTER_UNIT_TEST( test_load_split_capture_sdk_reads_both_ends );

#endif // #if ROCKETNET_DEV
