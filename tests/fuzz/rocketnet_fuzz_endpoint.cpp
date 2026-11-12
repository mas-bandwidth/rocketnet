/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message layer's receive side after decrypt, direct: rn_endpoint_on_payload
// (layers 2 to 6: record decode, the reliable window, the channel dispatch,
// block and sequenced fragment reassembly, the receive ring, CONFIG) on one
// receiving endpoint B, with a real sending endpoint A beside it. The pair
// targets reach this code only through the AEAD at tens of inputs a second;
// this one has no transport, so it runs at thousands.
//
// INPUT: [config selector][op]...
//   config selector: picks one of the message configs below (both ends use it,
//   so the CONFIG exchange agrees)
//   op & 7:
//     0 raw:     [u16 len][len bytes] handed to B as one opened payload
//     1 send:    [reliability][channel][u16 size] A sends a message of that size
//                (scaled into 1 .. max_message_bytes), filled with a pattern
//     2 write:   A's write pass: its packets join the in-flight list (at most 64)
//     3 deliver: [index][mutate][u16 pos][value] one in-flight A packet to B;
//                mutate bit 0 XORs value at pos, bit 1 truncates at pos, bit 2
//                keeps the packet in flight (a duplicate), bit 3 drops it instead
//     4 update:  [dt] one update of both ends: B's begin_update, tick and
//                config tick, B's acks fed back to A (lossy by bit), A's tick
//     5 ack:     [u64 id] B is told the transport acked packet id (B's own sends)
//     6 bsend:   [reliability][u16 size] B sends, and B writes its packets
//                (delivered cleanly to A), so B has sent state for acks
//     7 time:    [u16 ms] time jumps
//
// ORACLES:
//   - every delivery is within the config: bytes 0 .. max_message_bytes,
//     channel below num_channels, code a valid reliability;
//   - the copy count is within the capacity handed in;
//   - CLEAN STREAM: while every payload B opened was an unmodified A packet,
//     each opened at most once (the transport's replay window), in any order
//     and with any loss, B delivers only what A sent, byte for byte, reliable
//     messages at most once, and B never ends the connection (a local sender's
//     loss, reordering and delay never disconnect: the owner's rule).
//   - unbounded work is caught by libFuzzer's -timeout, memory by ASan and the
//     rss limit.
//
// THE SEED GENERATOR: ROCKETNET_FUZZ_SEED_DIR=<dir> writes one script per
// config, reliability and message size (tools/fuzz_seedgen wrote the
// committed set the same way): A sends three messages, four rounds of a
// write pass, twelve clean deliveries and an update, then a reordered round
// (last packet first), then B sends and is acked. Each is replayed through
// the target, which must keep the stream clean (no oracle fails, nothing
// tainted, B never ends the connection), and every reliability type must be
// delivered to B under every config by some seed. It exits 1 printing
// INCOMPLETE otherwise.

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_channel.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_message.h"
#include "rocketnet_send_budget.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "../portable_crt.h"

static void fz_require( bool condition, const char * property )
{
    if ( condition )
    {
        return;
    }
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

struct fz_reader_t
{
    const uint8_t * data;
    size_t size;
    size_t at;
};

static bool fz_more( const fz_reader_t * r )
{
    return r->at < r->size;
}

static uint8_t fz_u8( fz_reader_t * r )
{
    return r->at < r->size ? r->data[r->at++] : 0;
}

static uint16_t fz_u16( fz_reader_t * r )
{
    const uint16_t lo = fz_u8( r );
    const uint16_t hi = fz_u8( r );
    return (uint16_t) ( lo | ( hi << 8 ) );
}

static uint64_t fz_u64( fz_reader_t * r )
{
    uint64_t v = 0;
    for ( int i = 0; i < 8; i++ )
    {
        v |= (uint64_t) fz_u8( r ) << ( 8 * i );
    }
    return v;
}

#define FZ_PACKET_BYTES     ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 64 )
#define FZ_MAX_INFLIGHT     64
#define FZ_MAX_MESSAGES     4096
#define FZ_MAX_OPS          2048

struct fz_packet_t
{
    uint64_t id;
    int bytes;
    uint8_t data[FZ_PACKET_BYTES];
};

struct fz_side_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_endpoint_t endpoint;
    uint64_t next_packet_id;
};

static void fz_config( int selector, rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->reliable_message_timeout_ms = 0;
    switch ( selector % 4 )
    {
        case 0:     // the unit suite's endpoint config: 64 KB ring, W = 64
            config->reliable_messages_in_flight = 64;
            config->send_queue_messages = 128;
            config->packets_in_flight = 64;
            config->receive_message_buffer_bytes = 65536;
            config->max_message_bytes = 65536;
            config->send_message_buffer_bytes = 262144;
            config->packets_per_update = 4;
            break;
        case 1:     // the smallest ring and window: bounds close enough to cross
            config->num_channels = 2;
            config->reliable_messages_in_flight = 64;
            config->send_queue_messages = 16;
            config->packets_in_flight = 64;
            config->receive_message_buffer_bytes = 4096;
            config->max_message_bytes = 4096;
            config->send_message_buffer_bytes = 65536;
            config->packets_per_update = 2;
            config->unreliable_messages_per_update = 1;
            config->sequenced_messages_held = 1;
            config->max_message_receipts = 16;
            break;
        case 2:     // many channels, held sequenced, a mid ring
            config->num_channels = 1024 < ROCKETNET_MAX_CHANNELS ? 1024 : ROCKETNET_MAX_CHANNELS;
            config->reliable_messages_in_flight = 128;
            config->send_queue_messages = 256;
            config->packets_in_flight = 256;
            config->receive_message_buffer_bytes = 16384;
            config->max_message_bytes = 9 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
            config->send_message_buffer_bytes = 262144;
            config->packets_per_update = 8;
            config->sequenced_messages_held = 4096;
            break;
        default:    // the defaults (1 MB ring, W = 1024, max message 1 MB)
            config->packets_per_update = 16;
            break;
    }
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_max_ms = 250;
}

static bool fz_side_create( fz_side_t * side, const rocketnet_message_config_t * config )
{
    memset( &side->counters, 0, sizeof( side->counters ) );
    side->arena = rn_arena_create( NULL, config, 1, &side->counters );
    if ( side->arena == NULL )
    {
        return false;
    }
    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = config;
    endpoint_config.send_kbps = 0;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    rn_endpoint_init( &side->endpoint, rn_arena_region( side->arena, 0 ), &endpoint_config );
    side->next_packet_id = 1;
    return true;
}

// the pattern A writes: message index in the first 4 bytes (when it fits),
// then bytes derived from the index and position

static uint8_t fz_pattern( uint32_t index, int i )
{
    return (uint8_t) ( index * 131u + (uint32_t) i * 7u + 1u );
}

struct fz_state_t
{
    rocketnet_message_config_t config;
    fz_side_t * a;
    fz_side_t * b;
    double now;
    bool tainted;                       // B opened a payload that was not an unmodified A packet, once
    std::vector<fz_packet_t> inflight;  // A's packets on the wire
    std::vector<uint64_t> opened;       // A packet ids B opened (the replay window)
    std::vector<uint64_t> b_acks;       // ids of A packets B opened, to ack back to A
    uint32_t sent_count;
    int sent_reliability[FZ_MAX_MESSAGES];
    int sent_bytes[FZ_MAX_MESSAGES];
    uint8_t delivered[FZ_MAX_MESSAGES];
    int since_update;
};

static bool fz_reliable( int reliability )
{
    return reliability == ROCKETNET_RELIABILITY_RELIABLE || reliability == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           reliability == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || reliability == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           reliability == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static void fz_check_delivery( fz_state_t * s, const rn_delivery_t * d )
{
    fz_require( d->bytes >= 0 && d->bytes <= s->config.max_message_bytes, "a delivery is within max_message_bytes" );
    fz_require( (int) d->channel < s->config.num_channels, "a delivery's channel is below num_channels" );
    fz_require( d->bytes == 0 || d->data != NULL, "a nonempty delivery has data" );

    // touch every byte, so a delivery pointing outside live memory is an ASan report

    volatile uint8_t sink = 0;
    for ( int i = 0; i < d->bytes; i++ )
    {
        sink = (uint8_t) ( sink ^ d->data[i] );
    }
    (void) sink;

    if ( s->tainted )
    {
        return;
    }

    // clean stream: B delivers only what A sent

    fz_require( d->bytes >= 4, "clean stream: a delivery carries A's index" );
    uint32_t index;
    memcpy( &index, d->data, 4 );
    fz_require( index < s->sent_count, "clean stream: a delivery is a message A sent" );
    fz_require( d->bytes == s->sent_bytes[index], "clean stream: a delivery has the size A sent" );
    for ( int i = 4; i < d->bytes; i++ )
    {
        if ( d->data[i] != fz_pattern( index, i ) )
        {
            fprintf( stderr, "index %u byte %d of %d: got %02x want %02x\n", index, i, d->bytes, d->data[i], fz_pattern( index, i ) );
            fz_require( false, "clean stream: a delivery is byte for byte what A sent" );
        }
    }
    if ( s->delivered[index] < 255 )
    {
        s->delivered[index]++;
    }
    if ( fz_reliable( s->sent_reliability[index] ) )
    {
        fz_require( s->delivered[index] == 1, "clean stream: a reliable message is delivered at most once" );
    }
}

static void fz_b_payload( fz_state_t * s, const uint8_t * data, int bytes )
{
    // the payload lives in its own allocation of exactly its size: a read past it is ASan's

    uint8_t * copy = (uint8_t *) malloc( bytes > 0 ? (size_t) bytes : 1 );
    if ( bytes > 0 )
    {
        memcpy( copy, data, (size_t) bytes );
    }
    rn_delivery_t out[64];
    int count = -1;
    rn_endpoint_on_payload( &s->b->endpoint, copy, bytes, out, 64, &count, s->now );
    fz_require( count >= 0 && count <= 64, "the copied delivery count is within the capacity" );

    // zero-copy deliveries point into the payload: check them before it goes

    for ( int i = 0; i < count; i++ )
    {
        fz_check_delivery( s, &out[i] );
    }
    free( copy );

    s->since_update++;
}

static void fz_update( fz_state_t * s, uint8_t dt, uint8_t ack_mask )
{
    s->now += 0.001 * ( 1 + dt );

    // B: the update's start, ticks

    rn_endpoint_begin_update( &s->b->endpoint );
    rn_endpoint_tick( &s->b->endpoint, s->now );
    rn_endpoint_config_tick( &s->b->endpoint, 1, s->now );

    // B's opened ids acked back to A over a lossy path (bit k of ack_mask drops ack k)

    for ( size_t k = 0; k < s->b_acks.size(); k++ )
    {
        if ( ( ack_mask >> ( k & 7 ) ) & 1 )
        {
            continue;
        }
        rn_endpoint_on_acked( &s->a->endpoint, s->b_acks[k], s->now );
    }
    s->b_acks.clear();

    rn_endpoint_begin_update( &s->a->endpoint );
    rn_endpoint_tick( &s->a->endpoint, s->now );
    rn_endpoint_config_tick( &s->a->endpoint, 1, s->now );

    s->since_update = 0;
}

// one write pass of a side: returns its packets through `sink`, or delivers
// them cleanly to the other side when sink is NULL (B -> A)

static void fz_write_pass( fz_state_t * s, fz_side_t * side, fz_side_t * remote, std::vector<fz_packet_t> * sink )
{
    rn_endpoint_begin_flush( &side->endpoint );
    for ( int guard = 0; guard < 256; guard++ )
    {
        const int request = rn_endpoint_next_packet_bytes( &side->endpoint, s->now );
        if ( request == 0 )
        {
            return;
        }
        fz_require( request > 0 && request <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, "a planned packet is within the payload bound" );

        fz_packet_t packet;
        packet.id = side->next_packet_id++;
        const int bytes = rn_endpoint_write_packet( &side->endpoint, packet.data, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, packet.id, s->now );
        if ( bytes == 0 )
        {
            rn_endpoint_packet_failed( &side->endpoint, packet.id, s->now );
            continue;
        }
        fz_require( bytes > 0 && bytes <= request, "a written packet is within its plan" );
        packet.bytes = bytes;
        rn_endpoint_packet_sent( &side->endpoint, packet.id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), s->now );

        if ( sink != NULL )
        {
            if ( sink->size() < FZ_MAX_INFLIGHT )
            {
                sink->push_back( packet );
            }
        }
        else
        {
            rn_delivery_t out[64];
            int count = 0;
            rn_endpoint_on_payload( &remote->endpoint, packet.data, packet.bytes, out, 64, &count, s->now );
            rn_endpoint_on_acked( &side->endpoint, packet.id, s->now );
        }
    }
}

static int fz_reliability( uint8_t v )
{
    return (int) ( v % 8 );
}

static bool fz_env_set( const char * name )
{
    char value[16];
    return rocketnet_test_getenv( name, value, sizeof( value ) ) != 0;
}

static fz_state_t fz_state;             // large: the in-flight packets; the seed generator reads it after a run

static void fz_run( const uint8_t * data, size_t size )
{
    if ( size < 1 )
    {
        return;
    }

    fz_reader_t r = { data, size, 0 };

    fz_state_t * s = &fz_state;
    s->inflight.clear();
    s->opened.clear();
    s->b_acks.clear();
    s->tainted = false;
    s->sent_count = 0;
    s->since_update = 0;
    memset( s->delivered, 0, sizeof( s->delivered ) );

    // One pair of arenas per config, made once and reused: rn_endpoint_reset
    // carves the region again for every input, as a server slot is reused for
    // a new connection, so an input that replays differently after another is
    // state a reset left behind. FZ_EP_FRESH=1 makes fresh arenas every input.

    static rocketnet_message_config_t configs[4];
    static fz_side_t sides_a[4];
    static fz_side_t sides_b[4];
    static bool made[4];
    static const bool fresh = fz_env_set( "FZ_EP_FRESH" );

    const int selector = fz_u8( &r ) % 4;
    if ( fresh || !made[selector] )
    {
        if ( made[selector] )
        {
            rn_arena_destroy( sides_a[selector].arena );
            rn_arena_destroy( sides_b[selector].arena );
        }
        fz_config( selector, &configs[selector] );
        fz_require( rocketnet_message_validate_config( &configs[selector] ) == 1, "the harness config validates" );
        if ( !fz_side_create( &sides_a[selector], &configs[selector] ) || !fz_side_create( &sides_b[selector], &configs[selector] ) )
        {
            fz_require( false, "the arenas create" );
        }
        made[selector] = true;
    }
    s->config = configs[selector];
    s->a = &sides_a[selector];
    s->b = &sides_b[selector];
    s->a->next_packet_id = 1;
    s->b->next_packet_id = 1;

    s->now = 1000.0;
    rn_endpoint_reset( &s->a->endpoint, s->now );
    rn_endpoint_reset( &s->b->endpoint, s->now );
    rn_endpoint_announce_config( &s->a->endpoint, s->now );
    rn_endpoint_announce_config( &s->b->endpoint, s->now );

    static uint8_t message[1 << 20];
    static uint8_t raw[FZ_PACKET_BYTES];

    for ( int ops = 0; ops < FZ_MAX_OPS && fz_more( &r ); ops++ )
    {
        if ( rn_endpoint_error( &s->b->endpoint ) != ROCKETNET_DISCONNECT_REASON_NONE )
        {
            fz_require( s->tainted, "clean stream: B never ends the connection" );
            break;                      // the owner disconnects here
        }

        // a real owner runs an update every so often: at most 32 payloads between

        if ( s->since_update >= 32 )
        {
            fz_update( s, 0, 0 );
        }

        const uint8_t op = fz_u8( &r );
        switch ( op & 7 )
        {
            case 0:
            {
                int len = fz_u16( &r ) % ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 );
                if ( (size_t) len > r.size - r.at )
                {
                    len = (int) ( r.size - r.at );
                }
                memcpy( raw, r.data + r.at, (size_t) len );
                r.at += (size_t) len;
                s->tainted = true;
                fz_b_payload( s, raw, len );
                break;
            }

            case 1:
            {
                const int reliability = fz_reliability( fz_u8( &r ) );
                const int channel = fz_u8( &r ) % s->config.num_channels;
                const uint16_t size_bits = fz_u16( &r );
                int bytes = 4 + (int) ( (uint64_t) size_bits * (uint64_t) ( s->config.max_message_bytes - 4 ) / 65535u );
                if ( size_bits & 1 )
                {
                    bytes = 4 + ( size_bits >> 1 ) % 2048;      // small ones are the common case
                }
                if ( bytes > s->config.max_message_bytes )
                {
                    bytes = s->config.max_message_bytes;
                }
                if ( s->sent_count >= FZ_MAX_MESSAGES )
                {
                    break;
                }
                const uint32_t index = s->sent_count;
                memcpy( message, &index, 4 );
                for ( int i = 4; i < bytes; i++ )
                {
                    message[i] = fz_pattern( index, i );
                }
                const uint32_t serial = rn_endpoint_send( &s->a->endpoint, message, bytes, reliability, ROCKETNET_PRIORITY_MEDIUM, channel, 0 );
                if ( serial != 0 )
                {
                    s->sent_reliability[index] = reliability;
                    s->sent_bytes[index] = bytes;
                    s->sent_count++;
                }
                fz_require( rn_endpoint_error( &s->a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE || s->tainted,
                            "clean stream: A's own sends never end A's connection" );
                break;
            }

            case 2:
                fz_write_pass( s, s->a, s->b, &s->inflight );
                break;

            case 3:
            {
                const uint8_t index_bits = fz_u8( &r );
                const uint8_t mutate = fz_u8( &r );
                const uint16_t pos = fz_u16( &r );
                const uint8_t value = fz_u8( &r );
                if ( s->inflight.empty() )
                {
                    break;
                }
                const size_t k = index_bits % s->inflight.size();
                fz_packet_t packet = s->inflight[k];
                if ( ( mutate & 4 ) == 0 )
                {
                    s->inflight.erase( s->inflight.begin() + (long) k );
                }
                if ( mutate & 8 )
                {
                    break;      // lost
                }
                bool modified = false;
                if ( ( mutate & 1 ) && value != 0 && packet.bytes > 0 )
                {
                    packet.data[pos % packet.bytes] ^= value;
                    modified = true;
                }
                if ( ( mutate & 2 ) && packet.bytes > 0 )
                {
                    const int cut = pos % packet.bytes;
                    if ( cut != packet.bytes )
                    {
                        packet.bytes = cut;
                        modified = true;
                    }
                }

                // the transport's replay window: an unmodified packet opened twice is dropped there

                bool seen = false;
                for ( size_t j = 0; j < s->opened.size(); j++ )
                {
                    if ( s->opened[j] == packet.id )
                    {
                        seen = true;
                    }
                }
                if ( seen )
                {
                    if ( !( mutate & 16 ) )
                    {
                        break;
                    }
                    modified = true;    // a duplicate past the replay window: not what the transport hands up
                }
                if ( modified )
                {
                    s->tainted = true;
                }
                else
                {
                    s->opened.push_back( packet.id );
                    s->b_acks.push_back( packet.id );
                }
                fz_b_payload( s, packet.data, packet.bytes );
                break;
            }

            case 4:
            {
                const uint8_t dt = fz_u8( &r );
                const uint8_t mask = fz_u8( &r );
                fz_update( s, dt, mask );
                break;
            }

            case 5:
                rn_endpoint_on_acked( &s->b->endpoint, fz_u64( &r ), s->now );
                break;

            case 6:
            {
                const int reliability = fz_reliability( fz_u8( &r ) );
                const int bytes = 4 + fz_u16( &r ) % 3000;
                memset( message, 0xb5, (size_t) bytes );
                rn_endpoint_send( &s->b->endpoint, message, bytes, reliability, ROCKETNET_PRIORITY_HIGH, 0, 0 );
                fz_write_pass( s, s->b, s->a, NULL );
                break;
            }

            default:
                s->now += 0.001 * (double) fz_u16( &r );
                break;
        }
    }

    // receipts read back: the ring is within its bound

    rocketnet_message_receipt_t receipts[64];
    const int n = rn_endpoint_take_receipts( &s->b->endpoint, receipts, 64 );
    fz_require( n >= 0 && n <= 64, "receipts are within the capacity" );

    static const bool debug = fz_env_set( "FZ_EP_DEBUG" );
    if ( debug )
    {
        int delivered = 0;
        for ( uint32_t i = 0; i < s->sent_count; i++ )
        {
            delivered += s->delivered[i] ? 1 : 0;
        }
        fprintf( stderr, "sent %u delivered %d tainted %d inflight %zu opened %zu a_err %d b_err %d\n", s->sent_count, delivered,
                 s->tainted ? 1 : 0, s->inflight.size(), s->opened.size(), rn_endpoint_error( &s->a->endpoint ), rn_endpoint_error( &s->b->endpoint ) );
    }
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir>

#define FZ_SEED_MAX 1024

static int fz_seed_put( uint8_t * seed, int at, const uint8_t * bytes, int count )
{
    if ( at + count > FZ_SEED_MAX )
    {
        return FZ_SEED_MAX + 1;
    }
    memcpy( seed + at, bytes, (size_t) count );
    return at + count;
}

static int fz_seed_send( uint8_t * seed, int at, uint8_t reliability, uint8_t channel, int size )
{
    const uint8_t op[5] = { 1, reliability, channel, (uint8_t) size, (uint8_t) ( size >> 8 ) };
    return fz_seed_put( seed, at, op, 5 );
}

static int fz_seed_deliver( uint8_t * seed, int at, uint8_t index )
{
    const uint8_t op[6] = { 3, index, 0, 0, 0, 0 };
    return fz_seed_put( seed, at, op, 6 );
}

static int fz_seed_round( uint8_t * seed, int at, bool reordered )
{
    static const uint8_t write_pass[1] = { 2 };
    static const uint8_t update[3] = { 4, 10, 0 };
    at = fz_seed_put( seed, at, write_pass, 1 );
    for ( int j = 0; j < 12; j++ )
    {
        at = fz_seed_deliver( seed, at, (uint8_t) ( reordered ? 200 - j : j % 3 ) );
    }
    return fz_seed_put( seed, at, update, 3 );
}

static int fz_seed_build( int config, int reliability, int size, uint8_t * seed )
{
    int at = 0;
    const uint8_t selector[1] = { (uint8_t) config };
    at = fz_seed_put( seed, at, selector, 1 );
    for ( int k = 0; k < 3; k++ )
    {
        at = fz_seed_send( seed, at, (uint8_t) reliability, (uint8_t) k, size );
    }
    for ( int k = 0; k < 4; k++ )
    {
        at = fz_seed_round( seed, at, false );
    }
    at = fz_seed_send( seed, at, (uint8_t) reliability, 1, size );
    at = fz_seed_round( seed, at, true );
    const uint8_t b_send[4] = { 6, (uint8_t) reliability, 100, 0 };
    at = fz_seed_put( seed, at, b_send, 4 );
    const uint8_t b_acked[9] = { 5, 1, 0, 0, 0, 0, 0, 0, 0 };
    at = fz_seed_put( seed, at, b_acked, 9 );
    static const uint8_t update[3] = { 4, 10, 0 };
    return fz_seed_put( seed, at, update, 3 );
}

static bool fz_seed_write( const char * dir, const char * name, const uint8_t * data, int bytes )
{
    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
    {
        fprintf( stderr, "seed generator: the seed path for %s does not fit\n", name );
        return false;
    }

    FILE * file = rocketnet_test_fopen( path, "wb" );
    if ( !file )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
        return false;
    }

    const bool ok = fwrite( data, 1, (size_t) bytes, file ) == (size_t) bytes;

    fclose( file );
    if ( !ok )
    {
        fprintf( stderr, "seed generator: cannot write %s\n", path );
    }

    return ok;
}

static bool fz_generate( const char * dir )
{
    static const int sizes[5] = { 2 * 20 + 1, 2 * 1000 + 1, 0x4000, 0x8000, 0xffff & ~1 };
    static uint8_t seed[FZ_SEED_MAX];
    bool complete = true;

    for ( int config = 0; config < 4; config++ )
    {
        for ( int reliability = 0; reliability < 8; reliability++ )
        {
            bool sent = false;
            bool delivered = false;
            for ( int k = 0; k < 5; k++ )
            {
                char name[64];
                snprintf( name, sizeof( name ), "ep_cfg%d_rel%d_size%d", config, reliability, sizes[k] );
                const int bytes = fz_seed_build( config, reliability, sizes[k], seed );
                if ( bytes > FZ_SEED_MAX )
                {
                    fprintf( stderr, "seed generator: the %s seed does not fit\n", name );
                    complete = false;
                    continue;
                }

                fz_run( seed, (size_t) bytes );

                const fz_state_t * s = &fz_state;
                if ( s->tainted || rn_endpoint_error( &s->b->endpoint ) != ROCKETNET_DISCONNECT_REASON_NONE )
                {
                    fprintf( stderr, "seed generator: the %s seed does not keep the stream clean\n", name );
                    complete = false;
                    continue;
                }
                for ( uint32_t i = 0; i < s->sent_count; i++ )
                {
                    sent = true;
                    if ( s->delivered[i] != 0 && s->sent_reliability[i] == reliability )
                    {
                        delivered = true;
                    }
                }

                if ( !fz_seed_write( dir, name, seed, bytes ) )
                {
                    complete = false;
                }
            }
            if ( !sent || !delivered )
            {
                fprintf( stderr, "seed generator: config %d delivers no reliability %d message to B\n", config, reliability );
                complete = false;
            }
        }
    }

    printf( "seed generator: %s\n", complete ? "every seed keeps the stream clean, and every reliability is delivered under every config"
                                             : "INCOMPLETE" );
    return complete;
}

// -----------------------------------------------------------------------

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        exit( fz_generate( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    fz_run( data, size );
    return 0;
}
