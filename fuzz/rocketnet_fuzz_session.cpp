/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The scripted session target: yojimbo's fuzz_connection_structured, on a
// real message server and a real message client over the loopback transport.
// The input is a script of operations -- send a message (fuzz picks the side,
// channel and size), tick, drop, duplicate, hold back, reorder, corrupt,
// inject -- and every packet on the wire comes from the real write paths, so
// the receivers never bail out early: the reliable window, block
// fragmentation and reassembly, resend, ack reconstruction, the sequenced
// and ordered delivery state and the receipts all run under a fuzz-chosen
// message stream on a fuzz-chosen hostile wire.
//
// THE INVARIANTS ARE THE DST'S (dst_checks.h, each mirroring a TLA+ module in
// tla/): channel c carries reliability type c, and every delivery is checked
// against what was sent -- ordered types in order and exactly once, reliable
// types exactly once, sequenced types never backwards, unreliable at most
// once, every message whole. After the script a quiet window runs with a
// clean wire while both sides keep sending, and every reliable message sent
// in it must arrive by the end (liveness). On top: nothing allocates after
// create, and every counter block is conserved.
//
// Unauthenticated damage (drops, duplicates, reordering, corruption, garbage
// from anywhere) can never excuse a broken guarantee, so it never switches a
// check off. The one operation that does is the authenticated injection,
// which seals fuzz plaintext under the live keys: that is the remote endpoint itself
// misbehaving, and a forged ack may legitimately end a resend. An input that
// uses it keeps the safety checks on deliveries it can attribute and drops
// the liveness check. A connection the script itself tears down (a sealed
// disconnect, or a long enough silence) also skips liveness.
//
// INPUT: [selector][op]...
//   selector bit 4: start the script during the handshake
//   each op is one byte (op % 12) and its arguments:
//    0 tick: one clean step
//    1 client sends: [channel][u16 size]
//    2 server sends: [channel][u16 size]
//    3 faulty tick: per captured datagram, one byte: deliver, drop,
//      duplicate or hold back to a later step
//    4 drop every captured client -> server datagram
//    5 drop every captured server -> client datagram
//    6 duplicate every captured datagram, both ways
//    7 reverse both captured queues, then a clean step
//    8 corrupt: [direction][u16 position][xor]
//    9 inject garbage: [where][u16 length][bytes]
//   10 authenticated injection: [direction][type][skip][u16 length][plaintext]
//   11 silence: [ticks] steps with nothing delivered

#include "rocketnet_fuzz_pair.h"
#include "../tests/dst_checks.h"

#include <filesystem>
#include <string>
#include <vector>

#define ROCKETNET_FUZZ_SESSION_MAGIC       0xF7
#define ROCKETNET_FUZZ_SESSION_HEADER      8
#define ROCKETNET_FUZZ_SESSION_SERVER      0       // the dst session the server's slot 0 receives on
#define ROCKETNET_FUZZ_SESSION_CLIENT      16      // the dst session client 0 receives on
#define ROCKETNET_FUZZ_SESSION_QUIET_STEPS 300

static struct rocketnet_fuzz_pair_t rocketnet_fuzz_session_pair;
static struct rocketnet_dst_check_state_t rocketnet_fuzz_session_checks;
static uint32_t rocketnet_fuzz_session_next[2][8];     // [direction][channel] next sequence
static bool rocketnet_fuzz_session_tainted;
static uint64_t rocketnet_fuzz_session_step;
static uint64_t rocketnet_fuzz_session_delivered_tally[2][ROCKETNET_FUZZ_NUM_CHANNELS];   // the generator's every-type check

static int rocketnet_fuzz_session_stream_index( int session, int channel )
{
    return rocketnet_dst_check_stream_index( session, channel, channel );
}

// the checks' state, reset for the streams this target uses only: the full
// table is tens of megabytes, and two sessions of eight streams are in play
static void rocketnet_fuzz_session_checks_reset()
{
    struct rocketnet_dst_check_state_t * st = &rocketnet_fuzz_session_checks;
    memset( st, 0, offsetof( struct rocketnet_dst_check_state_t, stream ) );
    const size_t tail = offsetof( struct rocketnet_dst_check_state_t, slot_client_id );
    memset( (uint8_t*) st + tail, 0, sizeof( *st ) - tail );
    for ( int channel = 0; channel < ROCKETNET_FUZZ_NUM_CHANNELS; channel++ )
    {
        memset( &st->stream[rocketnet_fuzz_session_stream_index( ROCKETNET_FUZZ_SESSION_SERVER, channel )], 0, sizeof( st->stream[0] ) );
        memset( &st->stream[rocketnet_fuzz_session_stream_index( ROCKETNET_FUZZ_SESSION_CLIENT, channel )], 0, sizeof( st->stream[0] ) );
    }
    memset( rocketnet_fuzz_session_next, 0, sizeof( rocketnet_fuzz_session_next ) );
    rocketnet_fuzz_session_tainted = false;
    rocketnet_fuzz_session_step = 0;
}

static void rocketnet_fuzz_session_fill( uint8_t * out, int bytes, int direction, int channel, uint32_t sequence )
{
    out[0] = ROCKETNET_FUZZ_SESSION_MAGIC;
    out[1] = (uint8_t) direction;
    out[2] = (uint8_t) channel;
    out[3] = (uint8_t) channel;         // the type: channel c carries reliability c
    memcpy( out + 4, &sequence, 4 );
    for ( int i = ROCKETNET_FUZZ_SESSION_HEADER; i < bytes; i++ )
        out[i] = (uint8_t) ( sequence * 31u + (uint32_t) i );
}

// direction 0: client -> server; 1: server -> client
static void rocketnet_fuzz_session_send( struct rocketnet_fuzz_pair_t * pair, int direction, int channel, int bytes )
{
    channel %= ROCKETNET_FUZZ_NUM_CHANNELS;
    if ( bytes < ROCKETNET_FUZZ_SESSION_HEADER )
        bytes = ROCKETNET_FUZZ_SESSION_HEADER;
    if ( bytes > ROCKETNET_FUZZ_MAX_MESSAGE_BYTES )
        bytes = ROCKETNET_FUZZ_MAX_MESSAGE_BYTES;
    if ( direction == 0 && rocketnet_message_client_state( pair->client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return;
    if ( direction == 1 && !rocketnet_message_server_client_connected( pair->server, 0 ) )
        return;

    const uint32_t sequence = rocketnet_fuzz_session_next[direction][channel];
    uint8_t * out = direction == 0 ? rocketnet_message_client_start( pair->client, channel, channel, bytes )
                                   : rocketnet_message_server_start( pair->server, 0, channel, channel, bytes );
    if ( !out )
        return;     // a full send queue refuses; nothing was sent
    rocketnet_fuzz_session_fill( out, bytes, direction, channel, sequence );
    const uint32_t id = direction == 0 ? rocketnet_message_client_finish( pair->client, bytes )
                                       : rocketnet_message_server_finish( pair->server, 0, bytes );
    if ( id == 0 )
        return;
    rocketnet_fuzz_session_next[direction][channel] = sequence + 1;
    const int session = direction == 0 ? ROCKETNET_FUZZ_SESSION_SERVER : ROCKETNET_FUZZ_SESSION_CLIENT;
    rocketnet_dst_check_sent( &rocketnet_fuzz_session_checks, pair->time, session, channel, channel, sequence, 1, 1, 0, bytes );
}

// ordered and sequenced types are delivered on their send's channel; the
// others ignore the channel and are delivered on channel 0 (rocketnet.h)
static int rocketnet_fuzz_session_delivery_channel( int type )
{
    const bool channelled = type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
                            type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
    return channelled ? type : 0;
}

static void rocketnet_fuzz_session_delivered( struct rocketnet_fuzz_pair_t * pair, int direction, int delivered_channel, const uint8_t * data, int bytes )
{
    struct rocketnet_dst_check_state_t * st = &rocketnet_fuzz_session_checks;
    rocketnet_fuzz_pair_require( delivered_channel >= 0 && delivered_channel < ROCKETNET_FUZZ_NUM_CHANNELS, "a delivery names a configured channel" );
    rocketnet_fuzz_pair_require( bytes >= 0 && bytes <= ROCKETNET_FUZZ_MAX_MESSAGE_BYTES, "a delivery is within max_message_bytes" );
    const int channel = bytes >= ROCKETNET_FUZZ_SESSION_HEADER ? data[2] : -1;
    const bool ours = bytes >= ROCKETNET_FUZZ_SESSION_HEADER && data[0] == ROCKETNET_FUZZ_SESSION_MAGIC && data[1] == direction &&
                      channel < ROCKETNET_FUZZ_NUM_CHANNELS && data[3] == channel &&
                      delivered_channel == rocketnet_fuzz_session_delivery_channel( channel );
    if ( !ours )
    {
        // only an authenticated injection can put a message on the wire that
        // the script did not send
        rocketnet_fuzz_pair_require( rocketnet_fuzz_session_tainted, "every delivery was sent" );
        return;
    }
    uint32_t sequence = 0;
    memcpy( &sequence, data + 4, 4 );
    bool intact = sequence < rocketnet_fuzz_session_next[direction][channel];
    for ( int i = ROCKETNET_FUZZ_SESSION_HEADER; i < bytes && intact; i++ )
        intact = data[i] == (uint8_t) ( sequence * 31u + (uint32_t) i );
    if ( !intact )
    {
        rocketnet_fuzz_pair_require( rocketnet_fuzz_session_tainted, "every delivery is a message that was sent, byte for byte" );
        return;
    }
    if ( rocketnet_fuzz_session_tainted )
        return;     // a forged sender can repeat a real message's bytes; the safety rows assume an honest one
    const int session = direction == 0 ? ROCKETNET_FUZZ_SESSION_SERVER : ROCKETNET_FUZZ_SESSION_CLIENT;
    st->step = rocketnet_fuzz_session_step;
    rocketnet_fuzz_session_delivered_tally[direction][channel]++;
    rocketnet_dst_check_delivery( st, pair->time, session, channel, channel, sequence, 1, 0, 1, 0, bytes, 1, 0 );
    if ( st->failed )
    {
        fprintf( stderr, "fuzz oracle failed: dst check %s: %s\n", st->check, st->detail );
        abort();
    }
}

static void rocketnet_fuzz_session_receive( struct rocketnet_fuzz_pair_t * pair )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    int channel = 0;
    int client_index = 0;
    while ( rocketnet_message_server_receive( pair->server, &client_index, &channel, &data, &bytes ) )
    {
        rocketnet_fuzz_pair_require( client_index == 0, "a delivery names the one client slot" );
        rocketnet_fuzz_session_delivered( pair, 0, channel, data, bytes );
        rocketnet_message_server_release( pair->server, data );
    }
    while ( rocketnet_message_client_receive( pair->client, &channel, &data, &bytes ) )
    {
        rocketnet_fuzz_session_delivered( pair, 1, channel, data, bytes );
        rocketnet_message_client_release( pair->client, data );
    }
}

// one step without the wire delivering: the caller decides what moves
static void rocketnet_fuzz_session_update( struct rocketnet_fuzz_pair_t * pair, double dt )
{
    rocketnet_fuzz_session_step++;
    pair->time += dt;
    rocketnet_fuzz_pair_server_update( pair );
    rocketnet_fuzz_pair_client_update( pair );
    rocketnet_fuzz_session_receive( pair );
    rocketnet_fuzz_pair_send( pair );
    rocketnet_fuzz_pair_check_no_allocation( pair );
}

static void rocketnet_fuzz_session_clean_step( struct rocketnet_fuzz_pair_t * pair )
{
    rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
    rocketnet_fuzz_pair_deliver_all( pair );
}

static void rocketnet_fuzz_session_faulty_step( struct rocketnet_fuzz_pair_t * pair, struct rocketnet_fuzz_cursor_t * cursor )
{
    rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
    // client -> server, then server -> client: each datagram takes one decision
    for ( int pass = 0; pass < 2; pass++ )
    {
        struct rocketnet_fuzz_ring_t * ring = pass == 0 ? &pair->c2s : &pair->s2c;
        int i = 0;
        const int count = ring->count;
        for ( int k = 0; k < count && i < ring->count; k++ )
        {
            const uint8_t decision = rocketnet_fuzz_u8( cursor ) & 3;
            if ( decision == 0 )            // deliver
                pass == 0 ? rocketnet_fuzz_pair_deliver_c2s( pair, i ) : rocketnet_fuzz_pair_deliver_s2c( pair, i );
            else if ( decision == 1 )       // drop
                rocketnet_fuzz_ring_remove( ring, i );
            else if ( decision == 2 )       // duplicate: one copy now, the other stays for a later step
            {
                struct rocketnet_fuzz_datagram_t copy = ring->packet[i];
                pass == 0 ? rocketnet_fuzz_pair_deliver_c2s( pair, i ) : rocketnet_fuzz_pair_deliver_s2c( pair, i );
                rocketnet_fuzz_ring_push( ring, &copy.from, copy.data, copy.bytes );
            }
            else                            // hold back: it is delivered after what comes later
                i++;
        }
    }
}

static void rocketnet_fuzz_session_duplicate_all( struct rocketnet_fuzz_pair_t * pair )
{
    for ( int pass = 0; pass < 2; pass++ )
    {
        struct rocketnet_fuzz_ring_t * ring = pass == 0 ? &pair->c2s : &pair->s2c;
        const int count = ring->count;
        for ( int i = 0; i < count; i++ )
        {
            struct rocketnet_fuzz_datagram_t copy = ring->packet[i];
            rocketnet_fuzz_ring_push( ring, &copy.from, copy.data, copy.bytes );
        }
    }
}

static void rocketnet_fuzz_session_reverse( struct rocketnet_fuzz_ring_t * ring )
{
    for ( int i = 0, j = ring->count - 1; i < j; i++, j-- )
    {
        struct rocketnet_fuzz_datagram_t t = ring->packet[i];
        ring->packet[i] = ring->packet[j];
        ring->packet[j] = t;
    }
}

static const uint8_t rocketnet_fuzz_session_sealed_types[] =
{
    ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
    ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET
};

static uint64_t rocketnet_fuzz_session_run( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;

    struct rocketnet_fuzz_cursor_t cursor = { data, size, 0 };
    const uint8_t selector = rocketnet_fuzz_u8( &cursor );

    struct rocketnet_fuzz_pair_t * pair = &rocketnet_fuzz_session_pair;
    rocketnet_fuzz_pair_acquire( pair, ( selector & 0x10 ) == 0 );
    rocketnet_fuzz_session_checks_reset();

    static uint8_t buffer[ROCKETNET_RECEIVE_CEILING];
    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int ops = 0; ops < 1024 && rocketnet_fuzz_more( &cursor ); ops++ )
    {
        const int op = rocketnet_fuzz_u8( &cursor ) % 12;
        switch ( op )
        {
            case 0:
                rocketnet_fuzz_session_clean_step( pair );
                break;

            case 1:
            case 2:
            {
                const int direction = op == 1 ? 0 : 1;
                const int channel = rocketnet_fuzz_u8( &cursor );
                const int bytes = rocketnet_fuzz_u16( &cursor ) % ( ROCKETNET_FUZZ_MAX_MESSAGE_BYTES + 1 );
                rocketnet_fuzz_session_send( pair, direction, channel, bytes );
                break;
            }

            case 3:
                rocketnet_fuzz_session_faulty_step( pair, &cursor );
                break;

            case 4:
                rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                pair->c2s.count = 0;
                rocketnet_fuzz_pair_deliver_all( pair );
                break;

            case 5:
                rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                pair->s2c.count = 0;
                rocketnet_fuzz_pair_deliver_all( pair );
                break;

            case 6:
                rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                rocketnet_fuzz_session_duplicate_all( pair );
                rocketnet_fuzz_pair_deliver_all( pair );
                break;

            case 7:
                rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                rocketnet_fuzz_session_reverse( &pair->c2s );
                rocketnet_fuzz_session_reverse( &pair->s2c );
                rocketnet_fuzz_pair_deliver_all( pair );
                break;

            case 8:
            {
                const uint8_t direction = rocketnet_fuzz_u8( &cursor );
                const uint16_t position = rocketnet_fuzz_u16( &cursor );
                const uint8_t mask = rocketnet_fuzz_u8( &cursor );
                rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                struct rocketnet_fuzz_ring_t * ring = ( direction & 1 ) ? &pair->s2c : &pair->c2s;
                if ( ring->count > 0 )
                {
                    struct rocketnet_fuzz_datagram_t * d = &ring->packet[( direction >> 1 ) % ring->count];
                    d->data[position % d->bytes] ^= mask;
                }
                rocketnet_fuzz_pair_deliver_all( pair );
                break;
            }

            case 9:
            {
                const uint8_t where = rocketnet_fuzz_u8( &cursor );
                int length = rocketnet_fuzz_u16( &cursor ) % ( ROCKETNET_RECEIVE_CEILING + 1 );
                for ( int i = 0; i < length; i++ )
                    buffer[i] = rocketnet_fuzz_u8( &cursor );
                if ( length < 1 )
                    length = 1;
                switch ( where % 3 )
                {
                    case 0: rocketnet_fuzz_pair_inject_c2s( pair, false, buffer, length ); break;
                    case 1: rocketnet_fuzz_pair_inject_c2s( pair, true, buffer, length ); break;
                    case 2: rocketnet_fuzz_pair_inject_s2c( pair, buffer, length ); break;
                }
                rocketnet_fuzz_session_clean_step( pair );
                break;
            }

            case 10:
            {
                const uint8_t direction = rocketnet_fuzz_u8( &cursor );
                const uint8_t type = rocketnet_fuzz_session_sealed_types[rocketnet_fuzz_u8( &cursor ) % sizeof( rocketnet_fuzz_session_sealed_types )];
                const int skip = rocketnet_fuzz_u8( &cursor ) % 64;
                int length = rocketnet_fuzz_u16( &cursor ) % ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 );
                for ( int i = 0; i < length; i++ )
                    buffer[i] = rocketnet_fuzz_u8( &cursor );
                rocketnet_fuzz_session_tainted = true;
                const int bytes = ( direction & 1 ) ? rocketnet_fuzz_pair_seal_s2c( pair, type, skip, buffer, length, packet )
                                                    : rocketnet_fuzz_pair_seal_c2s( pair, type, skip, buffer, length, packet );
                if ( bytes > 0 )
                {
                    if ( direction & 1 )
                        rocketnet_fuzz_pair_inject_s2c( pair, packet, bytes );
                    else
                        rocketnet_fuzz_pair_inject_c2s( pair, true, packet, bytes );
                }
                rocketnet_fuzz_session_clean_step( pair );
                break;
            }

            case 11:
            {
                const int ticks = 1 + rocketnet_fuzz_u8( &cursor ) % 64;
                for ( int t = 0; t < ticks; t++ )
                {
                    rocketnet_fuzz_session_update( pair, ROCKETNET_FUZZ_DT );
                    pair->c2s.count = 0;
                    pair->s2c.count = 0;
                }
                break;
            }
        }
    }

    // the quiet window: a clean wire, both sides still sending, and every
    // reliable message sent in it due by the end
    rocketnet_dst_check_settle( &rocketnet_fuzz_session_checks );
    for ( int step = 0; step < ROCKETNET_FUZZ_SESSION_QUIET_STEPS; step++ )
    {
        if ( step < ROCKETNET_FUZZ_SESSION_QUIET_STEPS - 100 )
        {
            rocketnet_fuzz_session_send( pair, 0, step % ROCKETNET_FUZZ_NUM_CHANNELS, 16 + step % 64 );
            rocketnet_fuzz_session_send( pair, 1, ( step + 3 ) % ROCKETNET_FUZZ_NUM_CHANNELS, 16 + step % 64 );
        }
        rocketnet_fuzz_session_clean_step( pair );
    }

    const bool live = rocketnet_fuzz_pair_connected( pair );
    if ( live && !rocketnet_fuzz_session_tainted )
    {
        int session_live[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        uint64_t live_identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        memset( session_live, 0, sizeof( session_live ) );
        memset( live_identity, 0, sizeof( live_identity ) );
        session_live[ROCKETNET_FUZZ_SESSION_SERVER] = 1;
        session_live[ROCKETNET_FUZZ_SESSION_CLIENT] = 1;
        live_identity[ROCKETNET_FUZZ_SESSION_SERVER] = 1;
        live_identity[ROCKETNET_FUZZ_SESSION_CLIENT] = 1;
        rocketnet_dst_check_end( &rocketnet_fuzz_session_checks, pair->time, session_live, live_identity );
        if ( rocketnet_fuzz_session_checks.failed )
        {
            fprintf( stderr, "fuzz oracle failed: dst check %s: %s\n", rocketnet_fuzz_session_checks.check, rocketnet_fuzz_session_checks.detail );
            abort();
        }
    }

    char report[240];
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( &pair->transport->counters, report, sizeof( report ) ) != 0, "the server transport's counters are conserved" );
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( rocketnet_message_server_counters( pair->server ), report, sizeof( report ) ) != 0, "the message server's counters are conserved" );
    rocketnet_fuzz_pair_require( rocketnet_counters_conserved( rocketnet_message_client_counters( pair->client ), report, sizeof( report ) ) != 0, "the message client's counters are conserved" );
    rocketnet_fuzz_pair_check_no_allocation( pair );
    return rocketnet_fuzz_pair_fingerprint( pair );
}

// -----------------------------------------------------------------------
//  the seed generator (ROCKETNET_FUZZ_SEED_DIR=<dir>, rocketnet_fuzz_pair.h)
//
// Scripts, not packets: every op of the script language at least once, each
// with legitimate traffic around it -- every reliability type on its channel
// both ways, small and block-fragmented sizes, each wire fault, a silence,
// and garbage injected from each of the three places, the garbage being the
// committed wire corpus (ROCKETNET_FUZZ_SEED_FROM=fuzz/corpus) rather than
// anything written here. Every script must replay clean through this target,
// and together they must deliver every reliability type in both directions.
// -----------------------------------------------------------------------

static void rocketnet_fuzz_session_op_send( std::vector<uint8_t> & s, int direction, int channel, int bytes )
{
    s.push_back( (uint8_t) ( direction == 0 ? 1 : 2 ) );
    s.push_back( (uint8_t) channel );
    s.push_back( (uint8_t) ( bytes & 0xFF ) );
    s.push_back( (uint8_t) ( bytes >> 8 ) );
}

static void rocketnet_fuzz_session_traffic( std::vector<uint8_t> & s, int bytes )
{
    for ( int channel = 0; channel < ROCKETNET_FUZZ_NUM_CHANNELS; channel++ )
    {
        rocketnet_fuzz_session_op_send( s, 0, channel, bytes + channel );
        rocketnet_fuzz_session_op_send( s, 1, channel, bytes + 2 * channel );
        s.push_back( 0 );
    }
}

static bool rocketnet_fuzz_session_generate( const char * dir )
{
    struct named_t { std::string name; std::vector<uint8_t> script; };
    std::vector<named_t> seeds;

    { named_t n = { "every_type_small", { 0x00 } }; rocketnet_fuzz_session_traffic( n.script, 16 ); seeds.push_back( n ); }
    { named_t n = { "every_type_blocks", { 0x00 } }; rocketnet_fuzz_session_traffic( n.script, 6000 ); seeds.push_back( n ); }
    { named_t n = { "handshake_start", { 0x10 } }; for ( int i = 0; i < 40; i++ ) n.script.push_back( 0 ); rocketnet_fuzz_session_traffic( n.script, 64 ); seeds.push_back( n ); }
    {
        // the faulty tick, each decision in turn: deliver, drop, duplicate, hold
        named_t n = { "faulty_ticks", { 0x00 } };
        for ( int round = 0; round < 8; round++ )
        {
            rocketnet_fuzz_session_traffic( n.script, 200 * round + 16 );
            n.script.push_back( 3 );
            for ( int d = 0; d < 32; d++ )
                n.script.push_back( (uint8_t) ( ( d + round ) & 3 ) );
        }
        seeds.push_back( n );
    }
    {
        const uint8_t ops[] = { 4, 5, 6, 7 };
        const char * names[] = { "drop_c2s", "drop_s2c", "duplicate", "reverse" };
        for ( int k = 0; k < 4; k++ )
        {
            named_t n = { names[k], { 0x00 } };
            for ( int round = 0; round < 6; round++ )
            {
                rocketnet_fuzz_session_traffic( n.script, 300 * round + 16 );
                n.script.push_back( ops[k] );
            }
            seeds.push_back( n );
        }
    }
    {
        named_t n = { "corrupt", { 0x00 } };
        for ( int round = 0; round < 6; round++ )
        {
            rocketnet_fuzz_session_traffic( n.script, 100 * round + 16 );
            const uint8_t corrupt[] = { 8, (uint8_t) round, (uint8_t) ( 7 + round ), 0, 0x5a };
            n.script.insert( n.script.end(), corrupt, corrupt + sizeof( corrupt ) );
        }
        seeds.push_back( n );
    }
    {
        named_t n = { "silence", { 0x00 } };
        rocketnet_fuzz_session_traffic( n.script, 64 );
        n.script.push_back( 11 );
        n.script.push_back( 40 );
        rocketnet_fuzz_session_traffic( n.script, 64 );
        seeds.push_back( n );
    }

    // garbage from each of the three places: the committed wire corpus
    char from[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_FROM", from, sizeof( from ) ) )
    {
        int count = 0;
        for ( const auto & entry : std::filesystem::directory_iterator( from ) )
        {
            const std::string file = entry.path().filename().string();
            if ( file.rfind( "prefilter_", 0 ) != 0 || !entry.is_regular_file() || count >= 12 )
                continue;
            static uint8_t body[ROCKETNET_RECEIVE_CEILING + 2];
            FILE * f = rocketnet_test_fopen( entry.path().string().c_str(), "rb" );
            if ( !f )
                continue;
            const int got = (int) fread( body, 1, sizeof( body ), f );
            fclose( f );
            if ( got < 2 )
                continue;
            const int datagram = got - 1 > ROCKETNET_RECEIVE_CEILING ? ROCKETNET_RECEIVE_CEILING : got - 1;
            named_t n = { "garbage_" + file.substr( strlen( "prefilter_" ) ), { 0x00 } };
            rocketnet_fuzz_session_traffic( n.script, 32 );
            n.script.push_back( 9 );
            n.script.push_back( (uint8_t) ( count % 3 ) );
            n.script.push_back( (uint8_t) ( datagram & 0xFF ) );
            n.script.push_back( (uint8_t) ( datagram >> 8 ) );
            n.script.insert( n.script.end(), body + 1, body + 1 + datagram );
            rocketnet_fuzz_session_traffic( n.script, 32 );
            seeds.push_back( n );
            count++;
        }
    }

    memset( rocketnet_fuzz_session_delivered_tally, 0, sizeof( rocketnet_fuzz_session_delivered_tally ) );
    bool complete = true;
    for ( const named_t & n : seeds )
    {
        rocketnet_fuzz_session_run( n.script.data(), n.script.size() );
        complete = rocketnet_fuzz_seed_write( dir, n.name.c_str(), n.script.data(), (int) n.script.size() ) && complete;
    }
    for ( int direction = 0; direction < 2; direction++ )
        for ( int channel = 0; channel < ROCKETNET_FUZZ_NUM_CHANNELS; channel++ )
            if ( rocketnet_fuzz_session_delivered_tally[direction][channel] == 0 )
            {
                fprintf( stderr, "seed generator: no seed delivers reliability %d %s\n", channel, direction == 0 ? "client -> server" : "server -> client" );
                complete = false;
            }
    printf( "seed generator: %d scripts; %s\n", (int) seeds.size(),
            complete ? "every reliability type is delivered both ways" : "INCOMPLETE" );
    return complete;
}

ROCKETNET_FUZZ_PAIR_TARGET( rocketnet_fuzz_session_run, rocketnet_fuzz_session_generate )
