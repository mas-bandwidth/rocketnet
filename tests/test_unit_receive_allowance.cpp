/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's per-client receive allowance (rocketnet_server_config_t
// per_client_receive_kbps, sdk/rocketnet_server_protocol.h) against its model,
// proofs/tla/RNReceiveAllowance.tla. Three levels: the bucket arithmetic, the
// protocol's sealed-packet path under fixed keys, and a public step-driven
// server with real clients over the loopback transport. Time is always an
// argument from 1000.0: nothing sleeps, nothing opens a socket.

#include "test_unit_standard.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_read_write.h"
#include "rocketnet_replay.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_layers.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

// a slot's byte allowance as a connection starts it: zeroed by the slot's
// reset (rocketnet_server_connection_reset), then full at the connection's
// time (rocketnet_server_process_connection_response). The control tokens
// and the ack credit are left empty, so only the bytes pay; the credits'
// own cases start them with rocketnet_server_receive_allowance_start.
static void allowance_start( struct rocketnet_server_receive_allowance_t * allowance, double capacity, double time )
{
    memset( allowance, 0, sizeof( *allowance ) );
    allowance->bytes = capacity;
    allowance->time = time;
    allowance->control_time = time;
}

// ---- the arithmetic ---------------------------------------------------------

static void test_receive_allowance_rate_and_capacity()
{
    // twice the setting, in bytes per second: kbps * 1000 / 8 * 2
    check( ROCKETNET_RECEIVE_ALLOWANCE_FACTOR == 2 );
    check( rocketnet_server_receive_allowance_rate( 0 ) == 2.0 * ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS * 125.0 );
    check( rocketnet_server_receive_allowance_rate( 256 ) == 64000.0 );
    check( rocketnet_server_receive_allowance_rate( 1 ) == 250.0 );
    check( rocketnet_server_receive_allowance_rate( ROCKETNET_SEND_KBPS_UNLIMITED - 1 ) > 0.0 );

    // the unlimited value turns the refusal off; a negative value never
    // reaches here (create refuses it) and reads as off, never as a tiny rate
    check( rocketnet_server_receive_allowance_rate( ROCKETNET_SEND_KBPS_UNLIMITED ) == 0.0 );
    check( rocketnet_server_receive_allowance_rate( -1 ) == 0.0 );

    // the capacity holds the connection's timeout of the rate, at least one
    // second, and one maximum packet on top: a maximum packet fits at the
    // lowest setting
    check( ROCKETNET_SEND_BUDGET == 1452 );
    check( rocketnet_server_receive_allowance_capacity( 64000.0, 10 ) == 640000.0 + 1452.0 );
    check( rocketnet_server_receive_allowance_capacity( 64000.0, 1 ) == 64000.0 + 1452.0 );
    check( rocketnet_server_receive_allowance_capacity( 64000.0, 0 ) == 64000.0 + 1452.0 );
    {
        const double low_rate = rocketnet_server_receive_allowance_rate( 1 );
        const double low_capacity = rocketnet_server_receive_allowance_capacity( low_rate, 1 );
        struct rocketnet_server_receive_allowance_t low;
        allowance_start( &low, low_capacity, 1000.0 );
        check( rocketnet_server_receive_allowance_take( &low, low_rate, low_capacity, 1000.0, ROCKETNET_SEND_BUDGET ) );
        check( low.refused_packets == 0 );
    }
}

// a sender at exactly the allowed rate, any spacing, is never refused; one
// byte past what the allowance holds is

static void test_receive_allowance_at_the_rate_never_refused()
{
    const double rate = rocketnet_server_receive_allowance_rate( 256 );
    const double capacity = rocketnet_server_receive_allowance_capacity( rate, 10 );

    struct rocketnet_server_receive_allowance_t allowance;
    allowance_start( &allowance, capacity, 1000.0 );

    // empty it in one instant, from full
    int spent = 0;
    while ( spent + 1000 <= (int) capacity )
    {
        check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, 1000 ) );
        spent += 1000;
    }
    check( allowance.bytes == capacity - spent );

    const int rest = (int) allowance.bytes;
    if ( rest > 0 )
    {
        check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, rest ) );
    }
    check( allowance.bytes == 0.0 );

    // exactly the bytes held: taken; one more: refused, and the refusal spends nothing
    check( !rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, 1 ) );
    check( allowance.bytes == 0.0 );
    check( allowance.refused_packets == 1 && allowance.refused_bytes == 1 );

    // then at exactly the rate, with uneven spacing on times a double holds
    // exactly: 64000 bytes a second as 1000 bytes every 1/64 s and 2000
    // bytes every 1/32 s, for 100 simulated seconds: never refused
    double time = 1000.0;
    for ( int i = 0; i < 4266; i++ )
    {
        const double dt = ( i % 2 ) ? 1.0 / 32.0 : 1.0 / 64.0;
        const int bytes = ( i % 2 ) ? 2000 : 1000;
        time += dt;
        check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, time, bytes ) );
    }
    check( allowance.refused_packets == 1 );
}

// the client's budget is half the rate: everything it could have sent during
// T-1 seconds of silence, delivered in one instant after it, is held, even
// from an allowance emptied just before the silence began

static void test_receive_allowance_burst_after_silence_not_refused()
{
    const int timeout = 10;
    const int kbps = 256;
    const double rate = rocketnet_server_receive_allowance_rate( kbps );
    const double capacity = rocketnet_server_receive_allowance_capacity( rate, timeout );
    const double budget = (double) kbps * 125.0;

    for ( int emptied = 0; emptied < 2; emptied++ )
    {
        struct rocketnet_server_receive_allowance_t allowance;
        allowance_start( &allowance, capacity, 1000.0 );
        if ( emptied )
        {
            check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, (int) capacity ) );
            check( allowance.bytes == 0.0 );
        }

        // T-1 seconds of nothing, then the whole outage's budget at once, in
        // maximum packets
        double time = 1000.0 + ( timeout - 1 );
        int burst = (int) ( budget * ( timeout - 1 ) );
        while ( burst > 0 )
        {
            const int bytes = burst < ROCKETNET_SEND_BUDGET ? burst : ROCKETNET_SEND_BUDGET;
            check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, time, bytes ) );
            burst -= bytes;
        }
        check( allowance.refused_packets == 0 );
    }
}

// a sender past twice the setting, sustained, is refused; what it gets
// through never exceeds the capacity plus the rate times the time

static void test_receive_allowance_sustained_over_twice_refused()
{
    const double rate = rocketnet_server_receive_allowance_rate( 256 );
    const double capacity = rocketnet_server_receive_allowance_capacity( rate, 10 );

    struct rocketnet_server_receive_allowance_t allowance;
    allowance_start( &allowance, capacity, 1000.0 );

    // 2.5 times the allowance's rate, 800 bytes every 5 ms, for 60 seconds
    double time = 1000.0;
    uint64_t taken = 0;
    uint64_t refused = 0;
    uint64_t refused_bytes = 0;
    for ( int i = 0; i < 12000; i++ )
    {
        time += 0.005;
        if ( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, time, 800 ) )
        {
            taken += 800;
        }
        else
        {
            refused++;
            refused_bytes += 800;
        }
    }

    check( refused > 0 );
    check( allowance.refused_packets == refused );
    check( allowance.refused_bytes == refused_bytes );
    check( (double) taken <= capacity + rate * 60.0 + 1e-6 );
}

// a time at or before the last refill adds nothing, and a jump forward fills
// to the capacity and no further

static void test_receive_allowance_time_back_and_jump_mint_nothing()
{
    const double rate = 1000.0;
    const double capacity = 5000.0;

    struct rocketnet_server_receive_allowance_t allowance;
    allowance_start( &allowance, capacity, 1000.0 );
    check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, 5000 ) );

    // back: nothing added, and the refill time stays at 1000
    check( !rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 995.0, 1 ) );
    check( allowance.bytes == 0.0 && allowance.time == 1000.0 );

    // back to the same time: still nothing, so 995 .. 1000 is not credited
    check( !rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.0, 1 ) );
    check( allowance.bytes == 0.0 );

    // half a second on: 500 bytes, no more
    check( !rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.5, 501 ) );
    check( allowance.bytes == 500.0 );
    check( rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1000.5, 500 ) );

    // a jump of a million seconds fills to the capacity
    check( !rocketnet_server_receive_allowance_take( &allowance, rate, capacity, 1001000.5, 5001 ) );
    check( allowance.bytes == capacity );
    check( allowance.refused_packets == 4 );
    check( allowance.refused_bytes == 1 + 1 + 501 + 5001 );
}

// ---- the protocol path ------------------------------------------------------

#define ALLOWANCE_FIXTURE_CLIENTS 2

static const uint8_t allowance_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
static const uint8_t allowance_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58 };

struct allowance_fixture_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_server_receive_allowance_t allowances[ALLOWANCE_FIXTURE_CLIENTS];
    struct rocketnet_address_t from[ALLOWANCE_FIXTURE_CLIENTS];
    uint8_t keys[ALLOWANCE_FIXTURE_CLIENTS][ROCKETNET_KEY_BYTES];
    struct rocketnet_encryption_mapping_t * mappings[ALLOWANCE_FIXTURE_CLIENTS];
    uint64_t sequence[ALLOWANCE_FIXTURE_CLIENTS];
};

// two connected clients in slots 0 and 1 of a server that allows rate bytes
// a second each, over a one-second timeout; each allowance begins holding
// 1000 bytes, as a connection that has spent the rest of its capacity

static void allowance_fixture_setup( struct allowance_fixture_t * fixture, double rate, double time )
{
    memset( fixture, 0, sizeof( *fixture ) );

    fixture->server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( fixture->server.packet_filter, allowance_filter, ROCKETNET_FILTER_VALUE_BYTES );
    fixture->server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    fixture->server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    fixture->server.counters = &fixture->counters;
    fixture->server.receive_allowances = fixture->allowances;
    fixture->server.receive_bytes_per_second = rate;
    check( fixture->server.encryption_map != NULL && fixture->server.client_slots != NULL );

    for ( int c = 0; c < ALLOWANCE_FIXTURE_CLIENTS; c++ )
    {
        check( rocketnet_address_parse( &fixture->from[c], c == 0 ? "198.51.100.7:56789" : "198.51.100.8:56790" ) );
        for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
        {
            fixture->keys[c][i] = (uint8_t) ( 0x30 + 0x40 * c + i );
        }

        const int index = rocketnet_encryption_map_add( fixture->server.encryption_map, &fixture->from[c], time + 5000.0 );
        check( index >= 0 );

        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( fixture->server.encryption_map, index );
        memcpy( mapping->keys.session_client_to_server, fixture->keys[c], ROCKETNET_KEY_BYTES );
        memcpy( mapping->connection_salt, allowance_salt, ROCKETNET_CONNECTION_SALT_BYTES );
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
        memset( user_data, 0, sizeof( user_data ) );
        check( rocketnet_client_slots_assign( fixture->server.client_slots, (uint64_t) c + 1, index, (uint64_t) c + 1, user_data ) == c );
        mapping->control->client_index = c;
        mapping->control->timeout_seconds = 1;
        mapping->control->last_receive_time = time;
        fixture->mappings[c] = mapping;
        fixture->sequence[c] = 1;

        allowance_start( &fixture->allowances[c], rocketnet_server_receive_allowance_capacity( rate, 1 ), time );
        fixture->allowances[c].bytes = 1000.0;
        fixture->allowances[c].ack_credit_spent = mapping->send_sequence_session;      // the server has sent nothing
    }
}

static void allowance_fixture_teardown( struct allowance_fixture_t * fixture )
{
    rocketnet_encryption_map_destroy( fixture->server.encryption_map );
    rocketnet_client_slots_destroy( fixture->server.client_slots );
}

// a sealed session packet from client c: a fresh ack block and type_bytes of
// type data, at the given sequence, sealed under key (the client's own, or not)

static int allowance_seal( const struct allowance_fixture_t * fixture, int c, uint8_t type, uint64_t sequence, int type_bytes,
                           const uint8_t * key, uint8_t * out )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( plaintext, 0, sizeof( plaintext ) );
    for ( int i = 0; i < type_bytes; i++ )
    {
        plaintext[ROCKETNET_ACK_BLOCK_MIN_BYTES + i] = (uint8_t) ( c + i );
    }
    (void) fixture;
    return rocketnet_write_sealed_packet( out, type, allowance_filter, sequence, NULL,
                                          plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES + type_bytes, ROCKETNET_TEST_PROTOCOL_ID,
                                          allowance_salt, NULL, key );
}

struct allowance_outcome_t
{
    int bytes;
    int payload_bytes;
    int payload_client_index;
};

static struct allowance_outcome_t allowance_send( struct allowance_fixture_t * fixture, int c, uint8_t type, int type_bytes, double time,
                                                  const uint8_t * key = NULL, uint64_t sequence = 0 )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    if ( sequence == 0 )
    {
        sequence = fixture->sequence[c]++;
    }

    struct allowance_outcome_t outcome;
    outcome.bytes = allowance_seal( fixture, c, type, sequence, type_bytes, key ? key : fixture->keys[c], packet );
    check( outcome.bytes > 0 );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int response_bytes = 0;
    outcome.payload_bytes = 0;
    outcome.payload_client_index = -1;
    rocketnet_server_protocol_process_packet( &fixture->server, &fixture->from[c], packet, outcome.bytes, 0, time,
                                              response, &response_bytes, payload, &outcome.payload_bytes, &outcome.payload_client_index );
    return outcome;
}

// A refused packet authenticated, marked its sequence, and did nothing else:
// no keep-alive, payload, ack or disconnect taken, no idle clock refreshed,
// no ack owed. Its replayed copy stays refused after the allowance refills.
// A packet that does not open is not counted against the allowance. The
// other client is untouched throughout.

static void test_receive_allowance_refused_packet_does_nothing()
{
    static struct allowance_fixture_t fixture;
    const double rate = 1000.0;
    allowance_fixture_setup( &fixture, rate, 1000.0 );
    uint64_t * counters = fixture.counters.values;

    // keep-alives are 41 bytes: 24 fit in 1000, the 25th does not
    for ( int i = 0; i < 24; i++ )
    {
        struct allowance_outcome_t outcome = allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, 1000.0 );
        check( outcome.bytes == ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    }
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 24 );
    check( fixture.allowances[0].bytes == 1000.0 - 24 * 41 );

    // refused at a later time that refills too little: the idle clock does
    // not move, nothing is accepted, and the refusal is counted once per type
    const double later = 1000.015625;    // 1/64 s: +15.625 bytes, 31.625 held
    const uint64_t refused_sequence = fixture.sequence[0];
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, later );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_OVER_ALLOWANCE] == 1 );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 24 );
    check( fixture.mappings[0]->control->last_receive_time == 1000.0 );
    check( rocketnet_replay_already_received( &fixture.mappings[0]->replay_session, refused_sequence ) );
    check( !rocketnet_replay_already_received( &fixture.mappings[0]->accepted_session, refused_sequence ) );

    struct allowance_outcome_t payload = allowance_send( &fixture, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 100, later );
    check( payload.payload_bytes == 0 && payload.payload_client_index == -1 );
    check( counters[ROCKETNET_COUNTER_PAYLOAD_OVER_ALLOWANCE] == 1 );
    check( counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] == 0 );

    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, later );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] == 1 );
    check( counters[ROCKETNET_COUNTER_ACK_ACCEPTED] == 0 );

    // a 33-byte disconnect does not fit the 31.625 held: the slot stays
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 0, later );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_OVER_ALLOWANCE] == 1 );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] == 0 );
    check( fixture.mappings[0]->control->client_index == 0 );

    check( fixture.allowances[0].refused_packets == 4 );
    check( fixture.allowances[0].refused_bytes == (uint64_t) ( 41 + ( 33 + 100 ) + 33 + 33 ) );
    check( fixture.allowances[0].bytes == 1000.0 - 24 * 41 + 15.625 );

    // forged under the wrong key: does not open, is not the client's, and the
    // allowance neither spends nor counts it
    uint8_t wrong_key[ROCKETNET_KEY_BYTES];
    memset( wrong_key, 0xEE, sizeof( wrong_key ) );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1000, later, wrong_key );
    check( counters[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] == 1 );
    check( counters[ROCKETNET_COUNTER_PAYLOAD_OVER_ALLOWANCE] == 1 );
    check( fixture.allowances[0].refused_packets == 4 );
    check( fixture.allowances[0].bytes == 1000.0 - 24 * 41 + 15.625 );

    // the other client, at the same instants, has its whole second
    for ( int i = 0; i < 24; i++ )
    {
        allowance_send( &fixture, 1, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, later );
    }
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 48 );
    check( fixture.allowances[1].refused_packets == 0 && fixture.allowances[1].refused_bytes == 0 );
    check( fixture.mappings[1]->control->last_receive_time == later );

    // refilled a second later: the refused sequence's copy is a replay, and a
    // fresh sequence is accepted
    const double refilled = 1001.5;
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, refilled, NULL, refused_sequence );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED] == 1 );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 48 );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, refilled );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 49 );
    check( fixture.mappings[0]->control->last_receive_time == refilled );

    // every refusal balances in its family
    check( rocketnet_counters_conserved( &fixture.counters, NULL, 0 ) == 1 );

    allowance_fixture_teardown( &fixture );
}

// a server with no allowance (the unlimited setting) refuses nothing

static void test_receive_allowance_off_refuses_nothing()
{
    static struct allowance_fixture_t fixture;
    allowance_fixture_setup( &fixture, rocketnet_server_receive_allowance_rate( ROCKETNET_SEND_KBPS_UNLIMITED ), 1000.0 );
    check( fixture.server.receive_bytes_per_second == 0.0 );

    for ( int i = 0; i < 2000; i++ )
    {
        allowance_send( &fixture, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, 1000.0 );
    }
    check( fixture.counters.values[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] == 2000 );
    check( fixture.counters.values[ROCKETNET_COUNTER_PAYLOAD_OVER_ALLOWANCE] == 0 );
    check( fixture.allowances[0].refused_packets == 0 );

    allowance_fixture_teardown( &fixture );
}

// The ack credit (proofs/tla/RNReceiveAllowance.tla TakeAck, ServerAck): a
// standalone ack is paid by an eliciting packet the server sent that client,
// whatever the bytes hold; past the credit it is the bytes', here none, so
// refused. The server's own standalone acks give no credit, and one client's
// credit is never another's.

static void test_receive_allowance_ack_credit()
{
    static struct allowance_fixture_t fixture;
    allowance_fixture_setup( &fixture, 1000.0, 1000.0 );
    fixture.server.acks = rocketnet_server_acks_create( NULL, 4 );
    check( fixture.server.acks != NULL );
    uint64_t * counters = fixture.counters.values;
    fixture.allowances[0].bytes = 0.0;
    fixture.allowances[1].bytes = 0.0;

    // nothing sent to client 0: no credit
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, 1000.0 );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] == 1 && counters[ROCKETNET_COUNTER_ACK_ACCEPTED] == 0 );

    // five keep-alives sent to client 0: five standalone acks, no more, all in one instant
    uint8_t out[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < 5; i++ )
    {
        check( rocketnet_server_write_keep_alive( &fixture.server, 0, 1000.0, out ) > 0 );
    }
    for ( int i = 0; i < 5; i++ )
    {
        allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, 1000.0 );
    }
    check( counters[ROCKETNET_COUNTER_ACK_ACCEPTED] == 5 );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, 1000.0 );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] == 2 );
    check( fixture.allowances[0].bytes == 0.0 );

    // client 1 was sent nothing: client 0's credit is not its
    allowance_send( &fixture, 1, ROCKETNET_CONNECTION_ACK_PACKET, 0, 1000.0 );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] == 3 );
    check( fixture.allowances[1].refused_packets == 1 );

    // a keep-alive from client 0 (paid in bytes) puts the server in its debt;
    // the standalone ack the server answers with elicits nothing, so it gives
    // no credit
    fixture.allowances[0].bytes = (double) ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, 1000.0 );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 1 );
    check( rocketnet_server_protocol_write_ack_if_due( &fixture.server, 0, 1000.0, out ) > 0 );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, 1000.0 );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] == 4 && counters[ROCKETNET_COUNTER_ACK_ACCEPTED] == 5 );
    check( fixture.allowances[0].ack_credit_spent == fixture.mappings[0]->send_sequence_session );
    check( fixture.allowances[0].refused_packets == 3 );

    check( rocketnet_counters_conserved( &fixture.counters, NULL, 0 ) == 1 );

    rocketnet_server_acks_destroy( fixture.server.acks );
    fixture.server.acks = NULL;
    allowance_fixture_teardown( &fixture );
}

// The control tokens (proofs/tla/RNReceiveAllowance.tla TakeControl): with no
// bytes at all, a client's keep-alives on its send tick for a minute, the
// tick's worth of a whole timeout arriving at once, and its disconnect are
// never refused; keep-alives past the tokens are, and a disconnect then
// waits for a token like any of them.

static void test_receive_allowance_control_tokens()
{
    static struct allowance_fixture_t fixture;
    allowance_fixture_setup( &fixture, 1000.0, 1000.0 );
    uint64_t * counters = fixture.counters.values;

    // a one-second timeout: 2 x 10 x 1 + 2 = 22 tokens, refilled at 20 a second
    for ( int c = 0; c < ALLOWANCE_FIXTURE_CLIENTS; c++ )
    {
        rocketnet_server_receive_allowance_start( &fixture.allowances[c], 1000.0, 1, 1000.0, fixture.mappings[c]->send_sequence_session );
        check( fixture.allowances[c].control == 22.0 );
        fixture.allowances[c].bytes = 0.0;
    }

    // client 1, correct: a keep-alive every tick (0.1 s) for a minute, then
    // the ticks of a whole timeout held up and arriving at once, then its
    // disconnect
    double time = 1000.0;
    for ( int i = 0; i < 600; i++ )
    {
        time += 0.1;
        allowance_send( &fixture, 1, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, time );
    }
    time += 1.0;
    for ( int i = 0; i < 11; i++ )
    {
        allowance_send( &fixture, 1, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, time );
    }
    allowance_send( &fixture, 1, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 0, time );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 611 );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] == 1 );
    check( fixture.allowances[1].refused_packets == 0 );

    // client 0 floods: 22 keep-alives at once are taken, the 23rd is not
    for ( int i = 0; i < 22; i++ )
    {
        allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, 1000.0 );
    }
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED] == 611 + 22 );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 8, 1000.0 );
    check( counters[ROCKETNET_COUNTER_KEEP_ALIVE_OVER_ALLOWANCE] == 1 );

    // and its disconnect waits for a token: refused now, taken 1/16 s on
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 0, 1000.0 );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_OVER_ALLOWANCE] == 1 );
    check( fixture.mappings[0]->control->client_index == 0 );
    allowance_send( &fixture, 0, ROCKETNET_CONNECTION_DISCONNECT_PACKET, 0, 1000.0625 );
    check( counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED] == 2 );

    check( rocketnet_counters_conserved( &fixture.counters, NULL, 0 ) == 1 );

    allowance_fixture_teardown( &fixture );
}

// A flooder of payload at 2.5 times the allowance and of standalone acks
// past its credit, for 60 seconds: refused, and what is taken never exceeds
// the byte capacity plus the rate times the time, plus one standalone ack per
// eliciting packet the server sent it.

static void test_receive_allowance_flooder_bounded()
{
    static struct allowance_fixture_t fixture;
    const double rate = rocketnet_server_receive_allowance_rate( 8 );         // 2000 bytes a second
    allowance_fixture_setup( &fixture, rate, 1000.0 );
    uint64_t * counters = fixture.counters.values;
    rocketnet_server_receive_allowance_start( &fixture.allowances[0], rate, 1, 1000.0, fixture.mappings[0]->send_sequence_session );
    const double capacity = rocketnet_server_receive_allowance_capacity( rate, 1 );

    // each 1/16 s: one keep-alive sent to it, then 312.5 bytes of payload
    // (2.5 x 2000 / 16, as packets of a 92-byte payload, 125 on the wire,
    // two or three a tick) and ten standalone acks
    uint8_t out[ROCKETNET_RECEIVE_CEILING];
    double time = 1000.0;
    uint64_t sent_eliciting = 0;
    uint64_t taken_bytes = 0;
    for ( int tick = 0; tick < 960; tick++ )
    {
        time += 0.0625;
        check( rocketnet_server_write_keep_alive( &fixture.server, 0, time, out ) > 0 );
        sent_eliciting++;
        const int payloads = ( tick % 2 ) ? 2 : 3;
        for ( int i = 0; i < payloads; i++ )
        {
            const uint64_t delivered = counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];
            struct allowance_outcome_t outcome = allowance_send( &fixture, 0, ROCKETNET_CONNECTION_PAYLOAD_PACKET, 92, time );
            check( outcome.bytes == 125 );
            if ( counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED] != delivered )
            {
                taken_bytes += (uint64_t) outcome.bytes;
            }
        }
        for ( int i = 0; i < 10; i++ )
        {
            const uint64_t accepted = counters[ROCKETNET_COUNTER_ACK_ACCEPTED];
            struct allowance_outcome_t outcome = allowance_send( &fixture, 0, ROCKETNET_CONNECTION_ACK_PACKET, 0, time );
            if ( counters[ROCKETNET_COUNTER_ACK_ACCEPTED] != accepted && i > 0 )
            {
                taken_bytes += (uint64_t) outcome.bytes;        // the first each tick is the credit's; any more, the bytes'
            }
        }
    }

    check( counters[ROCKETNET_COUNTER_PAYLOAD_OVER_ALLOWANCE] > 0 );
    check( counters[ROCKETNET_COUNTER_ACK_OVER_ALLOWANCE] > 0 );
    check( counters[ROCKETNET_COUNTER_ACK_ACCEPTED] >= sent_eliciting );
    check( (double) taken_bytes <= capacity + rate * 60.0 );
    check( fixture.allowances[0].ack_credit_spent == fixture.mappings[0]->send_sequence_session );
    check( fixture.allowances[1].refused_packets == 0 );

    check( rocketnet_counters_conserved( &fixture.counters, NULL, 0 ) == 1 );

    allowance_fixture_teardown( &fixture );
}

REGISTER_UNIT_TEST( test_receive_allowance_rate_and_capacity );
REGISTER_UNIT_TEST( test_receive_allowance_ack_credit );
REGISTER_UNIT_TEST( test_receive_allowance_control_tokens );
REGISTER_UNIT_TEST( test_receive_allowance_flooder_bounded );
REGISTER_UNIT_TEST( test_receive_allowance_at_the_rate_never_refused );
REGISTER_UNIT_TEST( test_receive_allowance_burst_after_silence_not_refused );
REGISTER_UNIT_TEST( test_receive_allowance_sustained_over_twice_refused );
REGISTER_UNIT_TEST( test_receive_allowance_time_back_and_jump_mint_nothing );
REGISTER_UNIT_TEST( test_receive_allowance_refused_packet_does_nothing );
REGISTER_UNIT_TEST( test_receive_allowance_off_refuses_nothing );

// ---- the public server ------------------------------------------------------

#if ROCKETNET_DEV

#include "unit_shutdown.h"
#include "loopback_hub.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"

// the step-driven public pair's pieces (as tests/public_step_pair.h), for a
// server of two slots and clients with a token of their own timeout

#define ALLOWANCE_PROTOCOL 0x52A110C4A2C3E5F7ULL

struct allowance_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
};

static double allowance_now = 1000.0;
static double allowance_clock( void * ) { return allowance_now; }


#define ALLOWANCE_TIMEOUT_SECONDS 10

struct allowance_public_t
{
    struct allowance_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[2];
    double time;
    int timeout_seconds;    // the timeout in the clients' tokens
};

static int allowance_log_lines = 0;

static void allowance_log_count( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    (void) length;
    if ( strstr( message, "over its receive allowance" ) != NULL )
    {
        allowance_log_lines++;
    }
}

static void allowance_public_create( struct allowance_public_t * pair, int per_client_receive_kbps )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    pair->timeout_seconds = ALLOWANCE_TIMEOUT_SECONDS;
    allowance_now = pair->time;
    rocketnet_quiet( 1 );

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, pair->identity.backend_public, pair->identity.backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = ALLOWANCE_PROTOCOL;
    memcpy( config.backend_public_key, pair->identity.backend_public, sizeof( config.backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.max_clients = 2;
    config.per_client_receive_kbps = per_client_receive_kbps;
    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );
}

static void allowance_public_pump( struct allowance_public_t * pair, bool update_server = true, bool update_clients = true )
{
    allowance_now = pair->time;
    for ( int c = 0; c < 2; c++ )
    {
        if ( pair->clients[c] && update_clients )
        {
            rocketnet_client_update( pair->clients[c], pair->time );
            rocketnet_client_send_packets( pair->clients[c] );
            while ( rocketnet_client_receive_messages( pair->clients[c] ) != NULL )
            {
            }
        }
    }
    if ( update_server )
    {
        rocketnet_server_update( pair->server, pair->time );
        rocketnet_server_send_packets( pair->server );
        while ( rocketnet_server_receive_messages( pair->server ) != NULL )
        {
        }
    }
    pair->time += 0.01;
}

// connects client c with a token of the pair's timeout, sending at send_kbps
static void allowance_public_connect( struct allowance_public_t * pair, int c, int send_kbps )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( pair->server, server_public );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair->server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( pair->identity.backend_secret, server_public, ALLOWANCE_PROTOCOL, addresses, 1,
                                                     0x5EED + (uint64_t) c, 1000, 1ULL << 40, pair->timeout_seconds, NULL,
                                                     token, sizeof( token ) ) == ROCKETNET_STATUS_OK );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;
    config.time_callback = allowance_clock;
    config.send_kbps = send_kbps == ROCKETNET_SEND_KBPS_UNLIMITED ? 0 : send_kbps;
    pair->clients[c] = rocketnet_client_create( NULL, &config, NULL );
    check( pair->clients[c] != NULL );
    if ( send_kbps == ROCKETNET_SEND_KBPS_UNLIMITED )
    {
        check( rocketnet_client_set_send_kbps( pair->clients[c], ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    }
    check( rocketnet_client_connect( pair->clients[c], token_string ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && rocketnet_client_state( pair->clients[c] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        allowance_public_pump( pair );
    }
    check( rocketnet_client_state( pair->clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( pair->clients[c] ) == c );
}

static void allowance_public_destroy( struct allowance_public_t * pair )
{
    for ( int c = 0; c < 2; c++ )
    {
        if ( pair->clients[c] )
        {
            rn_test_client_stop_and_destroy( pair->clients[c] );
        }
    }
    rocketnet_server_stop( pair->server );
    for ( int step = 0; step < 32 && !rocketnet_server_stopped( pair->server ); step++ )
    {
    }
    check( rocketnet_server_stopped( pair->server ) );
    rocketnet_server_destroy( pair->server );
    rocketnet_log_function( NULL, NULL );
}

static void allowance_public_refused( struct allowance_public_t * pair, int c, uint64_t * packets, uint64_t * bytes )
{
    struct rocketnet_conditions_t conditions;
    memset( &conditions, 0xFF, sizeof( conditions ) );
    check( rocketnet_server_client_conditions( pair->server, c, &conditions, (int) sizeof( conditions ) ) );
    *packets = conditions.receive_refused_packets;
    *bytes = conditions.receive_refused_bytes;
}

// queues what client c's budget may or may not carry this step: unreliable
// messages of `unreliable` bytes, `count` of them, and one small reliable one
static void allowance_public_queue( struct allowance_public_t * pair, int c, int count, int unreliable )
{
    for ( int i = 0; i < count; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair->clients[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE, unreliable );
        if ( out )
        {
            memset( out, (uint8_t) i, (size_t) unreliable );
            rocketnet_client_finish_message( pair->clients[c], unreliable );
        }
    }

    uint8_t * out = rocketnet_client_start_message( pair->clients[c], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 );
    if ( out )
    {
        memset( out, 0x5A, 64 );
        rocketnet_client_finish_message( pair->clients[c], 64 );
    }
}

static void allowance_public_server_queue( struct allowance_public_t * pair, int c, int count )
{
    for ( int i = 0; i < count; i++ )
    {
        uint8_t * out = rocketnet_server_start_message( pair->server, (uint64_t) c, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 200 );
        if ( out )
        {
            memset( out, (uint8_t) i, 200 );
            rocketnet_server_finish_message( pair->server, 200 );
        }
    }
}

// A correct client is never refused: its send budget at the server's
// setting, saturated with unreliable traffic and reliable traffic resent
// through loss and duplication both ways, its standalone acks to a server
// that sends to it, its keep-alives while idle, and the burst after a T-1
// outage of the link and after a T-1 stall of the server.

static void test_receive_allowance_correct_client_never_refused()
{
    const int kbps = 256;
    struct allowance_public_t pair;
    allowance_public_create( &pair, kbps );
    allowance_public_connect( &pair, 0, kbps );

    rocketnet_client_network_simulator( pair.clients[0], 0, 0, 10, 10 );
    rocketnet_server_network_simulator( pair.server, 0, 0, 10, 0 );

    // 20 seconds saturated: 3 KB queued every 10 ms is 2.4 Mbps, ten times the budget
    for ( int i = 0; i < 2000; i++ )
    {
        allowance_public_queue( &pair, 0, 3, 1000 );
        allowance_public_server_queue( &pair, 0, 1 );
        allowance_public_pump( &pair );
    }

    // 20 seconds idle at the client, the server sending: acks and keep-alives
    for ( int i = 0; i < 2000; i++ )
    {
        allowance_public_server_queue( &pair, 0, 2 );
        allowance_public_pump( &pair );
    }

    // the link down for T-1 seconds with the client still queuing, then up
    rocketnet_client_network_simulator( pair.clients[0], 0, 0, 100, 0 );
    for ( int i = 0; i < ( ALLOWANCE_TIMEOUT_SECONDS - 1 ) * 100; i++ )
    {
        allowance_public_queue( &pair, 0, 3, 1000 );
        allowance_public_pump( &pair );
    }
    rocketnet_client_network_simulator( pair.clients[0], 0, 0, 0, 0 );
    for ( int i = 0; i < 500; i++ )
    {
        allowance_public_queue( &pair, 0, 3, 1000 );
        allowance_public_pump( &pair );
    }

    // the server stalled for T-1 seconds while the client sends, then
    // everything waiting in one update
    for ( int i = 0; i < ( ALLOWANCE_TIMEOUT_SECONDS - 1 ) * 100; i++ )
    {
        allowance_public_queue( &pair, 0, 3, 1000 );
        allowance_public_pump( &pair, false, true );
    }
    for ( int i = 0; i < 500; i++ )
    {
        allowance_public_queue( &pair, 0, 3, 1000 );
        allowance_public_pump( &pair );
    }

    check( rocketnet_client_state( pair.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair.server, 0 ) );

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( pair.clients[0], &stats );
    check( stats.bytes_sent > (uint64_t) kbps * 125 * 30 );

    uint64_t packets = 1, bytes = 1;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets == 0 && bytes == 0 );

    allowance_public_destroy( &pair );
}

// Low settings and the shortest timeout: 4, 6 and 8 kbps over one second.
// The client's keep-alives (up to ten a second) and standalone acks are
// outside its send budget. Before the ack credit and the control tokens,
// at 4 and 6 kbps this run had packets refused (measured 2026-11-11, 3 and
// 80 of about 540); now they are paid by those, and nothing is refused at
// any of the three. The capacity's maximum packet is checked in
// test_receive_allowance_rate_and_capacity.

static void allowance_low_setting_run( int kbps );

static void test_receive_allowance_low_setting_correct_client_never_refused()
{
    static const int settings[] = { 4, 6, 8 };
    for ( int s = 0; s < (int) ( sizeof( settings ) / sizeof( settings[0] ) ); s++ )
    {
        allowance_low_setting_run( settings[s] );
    }
}

static void allowance_low_setting_run( int kbps )
{
    struct allowance_public_t pair;
    allowance_public_create( &pair, kbps );
    pair.timeout_seconds = 1;
    allowance_public_connect( &pair, 0, kbps );

    // maximum unreliable messages queued every 10 ms for 20 seconds, then 10
    // seconds idle
    for ( int i = 0; i < 3000; i++ )
    {
        if ( i < 2000 )
        {
            allowance_public_queue( &pair, 0, 1, ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES );
        }
        allowance_public_pump( &pair );
    }

    check( rocketnet_client_state( pair.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair.server, 0 ) );

    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( pair.clients[0], &stats );
    check( stats.bytes_sent > (uint64_t) kbps * 125 * 20 );

    uint64_t packets = 1, bytes = 1;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets == 0 && bytes == 0 );

    allowance_public_destroy( &pair );
}

// A client past twice the setting is refused and counted in its
// conditions, logged once, and not disconnected; the other client, at the
// setting, is untouched. The slot's next connection starts with nothing
// refused.

static void test_receive_allowance_flooding_client_refused_alone()
{
    const int kbps = 256;
    struct allowance_public_t pair;
    allowance_public_create( &pair, kbps );
    allowance_public_connect( &pair, 0, ROCKETNET_SEND_KBPS_UNLIMITED );
    allowance_public_connect( &pair, 1, kbps );

    rocketnet_quiet( 0 );
    enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    allowance_log_lines = 0;
    rocketnet_log_function( allowance_log_count, NULL );

    // client 0 queues 8 KB every 10 ms, 6.4 Mbps, twelve times the allowance
    for ( int i = 0; i < 2000; i++ )
    {
        allowance_public_queue( &pair, 0, 8, 1000 );
        allowance_public_queue( &pair, 1, 3, 1000 );
        allowance_public_pump( &pair );
    }

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
    rocketnet_quiet( 1 );

    uint64_t packets = 0, bytes = 0;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets > 0 );
    check( bytes >= packets * ( ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES ) );
    check( allowance_log_lines == 1 );

    check( rocketnet_server_client_connected( pair.server, 0 ) );
    check( rocketnet_client_state( pair.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    uint64_t other_packets = 1, other_bytes = 1;
    allowance_public_refused( &pair, 1, &other_packets, &other_bytes );
    check( other_packets == 0 && other_bytes == 0 );

    // the slot's next connection: the flooder leaves, the slot frees, a new
    // client takes it with nothing refused
    rn_test_client_stop_and_destroy( pair.clients[0] );
    pair.clients[0] = NULL;
    for ( int i = 0; i < ( ALLOWANCE_TIMEOUT_SECONDS + 2 ) * 100 && rocketnet_server_client_connected( pair.server, 0 ); i++ )
    {
        allowance_public_pump( &pair );
    }
    check( !rocketnet_server_client_connected( pair.server, 0 ) );

    allowance_public_connect( &pair, 0, kbps );
    packets = 1;
    bytes = 1;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets == 0 && bytes == 0 );

    allowance_public_destroy( &pair );
}

// The server streaming to client c at its default per-client rate (10 Mbps,
// 12.5 KB a 10 ms step: thirteen 1000-byte unreliable messages, more than the
// budget carries) with one reliable message a step.
static void allowance_public_server_stream( struct allowance_public_t * pair, int c )
{
    for ( int i = 0; i < 13; i++ )
    {
        uint8_t * out = rocketnet_server_start_message( pair->server, (uint64_t) c, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1000 );
        if ( out )
        {
            memset( out, (uint8_t) i, 1000 );
            rocketnet_server_finish_message( pair->server, 1000 );
        }
    }
    allowance_public_server_queue( pair, c, 1 );
}

// The open finding of the first build, settled: a correct client at setting
// kbps, the server streaming to it at its default 10 Mbps with loss server to
// client, for 60 simulated seconds (30 with the client saturating its own
// budget, 30 with it idle, sending only keep-alives and acks): refused
// nothing, at 0, 1, 10 and 30% loss. Its standalone acks of the lossy stream
// (up to a maximum packet each, at its update rate) run far over a low
// setting; they are paid by the ack credit, its keep-alives by the control
// tokens, so the bytes carry its budget alone.

static void allowance_public_stream_matrix( int kbps )
{
    static const int losses[] = { 0, 1, 10, 30 };
    for ( int l = 0; l < (int) ( sizeof( losses ) / sizeof( losses[0] ) ); l++ )
    {
        struct allowance_public_t pair;
        allowance_public_create( &pair, kbps );
        allowance_public_connect( &pair, 0, kbps );
        rocketnet_server_network_simulator( pair.server, 0, 0, losses[l], 0 );

        uint64_t received_before = 0;
        {
            struct rocketnet_client_stats_t stats;
            rocketnet_client_stats( pair.clients[0], &stats );
            received_before = stats.bytes_received;
        }

        for ( int i = 0; i < 6000; i++ )
        {
            if ( i < 3000 )
            {
                allowance_public_queue( &pair, 0, 2, 1000 );
            }
            allowance_public_server_stream( &pair, 0 );
            allowance_public_pump( &pair );
        }

        check( rocketnet_client_state( pair.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( pair.server, 0 ) );

        // the stream ran: at least 5 Mbps of the 10 reached the client
        struct rocketnet_client_stats_t stats;
        rocketnet_client_stats( pair.clients[0], &stats );
        check( stats.bytes_received - received_before > (uint64_t) 5000 * 125 * 60 );
        check( stats.bytes_sent > (uint64_t) kbps * 125 * 25 );

        uint64_t packets = 1, bytes = 1;
        allowance_public_refused( &pair, 0, &packets, &bytes );
        if ( packets != 0 )
        {
            printf( "receive allowance: %d kbps, %d%% loss: %" PRIu64 " packets, %" PRIu64 " bytes refused\n", kbps, losses[l], packets, bytes );
        }
        check( packets == 0 && bytes == 0 );

        allowance_public_destroy( &pair );
    }
}

static void test_receive_allowance_stream_4_kbps() { allowance_public_stream_matrix( 4 ); }
static void test_receive_allowance_stream_8_kbps() { allowance_public_stream_matrix( 8 ); }
static void test_receive_allowance_stream_64_kbps() { allowance_public_stream_matrix( 64 ); }
static void test_receive_allowance_stream_256_kbps() { allowance_public_stream_matrix( 256 ); }
static void test_receive_allowance_stream_1000_kbps() { allowance_public_stream_matrix( 1000 ); }

// A client sending 2.5 times the setting in payload over a one-second
// timeout for 60 seconds: refused and counted, never disconnected. Of what it
// sent (about 2.75 x the setting with its keep-alives and acks, which the
// control tokens and the ack credit pay) a tenth at least is refused
// (measured: 17%). The exact bound on what is taken is
// test_receive_allowance_flooder_bounded's.

static void test_receive_allowance_two_and_a_half_times_refused()
{
    const int kbps = 64;
    struct allowance_public_t pair;
    allowance_public_create( &pair, kbps );
    pair.timeout_seconds = 1;
    allowance_public_connect( &pair, 0, kbps * 5 / 2 );

    struct rocketnet_client_stats_t before;
    rocketnet_client_stats( pair.clients[0], &before );

    for ( int i = 0; i < 6000; i++ )
    {
        allowance_public_queue( &pair, 0, 2, 1000 );
        allowance_public_pump( &pair );
    }

    struct rocketnet_client_stats_t after;
    rocketnet_client_stats( pair.clients[0], &after );

    uint64_t packets = 0, bytes = 0;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets > 0 );
    check( rocketnet_server_client_connected( pair.server, 0 ) );
    check( rocketnet_client_state( pair.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const double sent = (double) ( after.bytes_sent - before.bytes_sent );
    check( sent > 2.5 * (double) kbps * 125.0 * 60.0 );
    check( (double) bytes > 0.1 * sent );

    allowance_public_destroy( &pair );
}

// The unlimited setting turns the refusal off: the same flood is refused nothing.

static void test_receive_allowance_unlimited_setting_is_off()
{
    struct allowance_public_t pair;
    allowance_public_create( &pair, ROCKETNET_SEND_KBPS_UNLIMITED );
    allowance_public_connect( &pair, 0, ROCKETNET_SEND_KBPS_UNLIMITED );

    for ( int i = 0; i < 1000; i++ )
    {
        allowance_public_queue( &pair, 0, 8, 1000 );
        allowance_public_pump( &pair );
    }

    uint64_t packets = 1, bytes = 1;
    allowance_public_refused( &pair, 0, &packets, &bytes );
    check( packets == 0 && bytes == 0 );

    allowance_public_destroy( &pair );
}

REGISTER_UNIT_TEST( test_receive_allowance_correct_client_never_refused );
REGISTER_UNIT_TEST( test_receive_allowance_low_setting_correct_client_never_refused );
REGISTER_UNIT_TEST( test_receive_allowance_flooding_client_refused_alone );
REGISTER_UNIT_TEST( test_receive_allowance_unlimited_setting_is_off );
REGISTER_UNIT_TEST( test_receive_allowance_stream_4_kbps );
REGISTER_UNIT_TEST( test_receive_allowance_stream_8_kbps );
REGISTER_UNIT_TEST( test_receive_allowance_stream_64_kbps );
REGISTER_UNIT_TEST( test_receive_allowance_stream_256_kbps );
REGISTER_UNIT_TEST( test_receive_allowance_stream_1000_kbps );
REGISTER_UNIT_TEST( test_receive_allowance_two_and_a_half_times_refused );

#endif // #if ROCKETNET_DEV
