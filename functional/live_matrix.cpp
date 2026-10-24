/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The loopback message matrix (docs/LIVE-XDP.md sections 3 to 5): a real
    message server and a real message client in one process over the UDP
    loopback, every mode at once on every channel the config allows, both
    directions, across the full size schedule of section 4 including each
    mode's exact maximum. Every delivered message is verified by
    tests/live_verify.h; the reliable modes are checked for completeness and
    order, the sequenced modes for strictly increasing sequences, the receipt
    modes for exactly one receipt per serial, and the over-maximum start is
    refused with its named counter. Codes 0 and 1 one past their
    single-record maxima start as their reliable twins (the promotion
    rule); their over-maximum is max_message_bytes.

    One window on one connection, ending in a clean disconnect the server
    sees: every mode at once on every channel the config allows, both
    directions, across the full size schedule of section 4 including each
    mode's exact maximum, every delivered message verified, the reliable
    modes checked for completeness and order, and the over-maximum start
    refused with its named counter. A connect that fails, or any disconnect
    the case did not plan, fails the case.

    Time is an argument: the clock the objects see is 1000.0 plus the wall
    time elapsed. The case prints its finish time as wall-clock duration;
    the target is 60 s and the hard completion bound is 120 s. Timing is
    reported separately from message correctness; this is not a load test.
*/

#include "../tests/native_shutdown.h"
#include "../tests/time_scale.h"
#include "rocketnet.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"

#include "../tests/live_verify.h"
#include "../tests/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// defined in functional/harness.inc: the connect token timestamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

#define LIVE_MATRIX_DIRECTIONS              2
#define LIVE_MATRIX_MODES                   8
#define LIVE_MATRIX_CHANNELS                32
#define LIVE_MATRIX_MAX_SEQUENCES           256
#define LIVE_MATRIX_TINY                    ROCKETNET_LIVE_HEADER_BYTES
#define LIVE_MATRIX_TOKEN_TIMEOUT           10.0            // the development connect token's timeout_seconds
// Use the unlimited create-time reserve policy with this same runtime ceiling.
// The matrix pays its working storage explicitly; it does not need two seconds
// of payload reserve at the ceiling in every size class.
#define LIVE_MATRIX_SEND_KBPS               10000000        // ROCKETNET_SEND_BUDGET_MAX_KBPS: the wire, not the budget, is the limit
// Each send_packets writes at most packets_per_update packets, reliable
// first (sdk/rocketnet_endpoint.cpp rn_endpoint_next_packet_bytes): the
// matrix queues about forty packets of reliable bytes and twenty-four of
// unreliable bytes per update, so the reliable path never starves the
// unreliable queue.
#define LIVE_MATRIX_RELIABLE_RATE           ( 40 * 1400 )
#define LIVE_MATRIX_UNRELIABLE_RATE         ( 24 * 1400 )
#define LIVE_MATRIX_RECEIPT_TABLE           ( 1 << 15 )
// Fixed functional-case budget, independent of build mode.
#define LIVE_MATRIX_WALL_BOUND              ROCKETNET_FUNCTIONAL_CASE_MAX_SECONDS
#define LIVE_MATRIX_TARGET_SECONDS          ROCKETNET_FUNCTIONAL_CASE_TARGET_SECONDS
#define LIVE_MATRIX_MAX_FAIL_LINES          24

// a passing run prints its name only: the per-window lines, the table and
// the summary print under ROCKETNET_TEST_VERBOSE, and the window lines,
// table and summary also print once the run has failed
static int live_matrix_verbose;

static void live_matrix_check_failed( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define live_matrix_check( condition )                                                                          \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
        live_matrix_check_failed( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );   \
} while(0)

// ---------------------------------------------------------------
//                     the per-mode rules (section 5)
// ---------------------------------------------------------------

// modes 1, 3, 4 and 7 run on channels 0 to 30; the channel-less modes 0, 2,
// 5 and 6 name all 32 (section 5, "Every channel, all at once")
static int live_matrix_channels( int mode )
{
    return ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ) ? 31 : 32;
}

// codes 1, 3, 4 and 7 arrive on the channel sent; 0, 2, 5 and 6 on channel 0
static int live_matrix_carries_channel( int mode )
{
    return mode == 1 || mode == 3 || mode == 4 || mode == 7;
}

static int live_matrix_ordered( int mode )
{
    return mode == 3 || mode == 7;
}

static int live_matrix_sequenced( int mode )
{
    return mode == 1 || mode == 4;
}

// The promotion rule: code 0 above 1,411 travels as code 2,
// and code 1 above 1,405 (channels below 256) as code 4
static int live_matrix_promoted( int mode, uint32_t length )
{
    return ( mode == 0 && length > ROCKETNET_LIVE_UNRELIABLE_LIMIT ) || ( mode == 1 && length > 1405u );
}

// every sequence arrives exactly once: 2, 3, 6, 7, and 0 and 5 above 1,411
// where they travel as a reliable block (section 5, the receive checks)
static int live_matrix_complete( int mode, uint32_t length )
{
    if ( mode == 5 || mode == 0 )
        return length > ROCKETNET_LIVE_UNRELIABLE_LIMIT;     // promoted to a reliable block
    return mode == 2 || mode == 3 || mode == 6 || mode == 7;
}

static int live_matrix_has_receipt( int mode )
{
    return mode == 5 || mode == 6 || mode == 7;
}

// ---------------------------------------------------------------
//                     state
// ---------------------------------------------------------------

struct live_matrix_stream_t
{
    int active;
    uint32_t total;                 // matrix sequences this window
    uint32_t closing;               // mode 4: the closing sequence, sent after the channel's last ordered send
    uint32_t next;                  // next sequence to send
    uint32_t sent;                  // messages sent on this stream
    uint64_t seen[LIVE_MATRIX_MAX_SEQUENCES / 64];
    int64_t last;                   // the last sequence received (7 bytes or more), sequenced and ordered modes
    uint32_t expected;              // ordered modes: the next sequence due
    uint32_t received;
    uint32_t lost_sequenced;        // mode 1 and 4: sequences skipped over
};

struct live_matrix_receipt_entry_t
{
    uint32_t serial;
    uint8_t mode;
    uint8_t receipted;
    uint32_t length;
};

struct live_matrix_direction_t
{
    struct live_matrix_stream_t streams[LIVE_MATRIX_MODES][LIVE_MATRIX_CHANNELS];
    uint32_t tiny_sent[LIVE_MATRIX_MODES][LIVE_MATRIX_TINY];
    uint32_t tiny_received[LIVE_MATRIX_MODES][LIVE_MATRIX_TINY];
    int cursor;
    int64_t reliable_tokens;
    int64_t unreliable_tokens;
    uint64_t paced;
    struct live_matrix_receipt_entry_t receipts[LIVE_MATRIX_RECEIPT_TABLE];
    uint32_t receipts_expected;
    uint32_t receipts_seen;
    uint32_t receipts_acked;
    uint32_t receipts_lost;
    uint64_t bytes_sent;
    uint64_t messages_sent;
};

// the one table, summed over the windows: per (direction, mode, channel,
// size bucket), sent and received
static uint32_t live_matrix_table_sent[LIVE_MATRIX_DIRECTIONS][LIVE_MATRIX_MODES][LIVE_MATRIX_CHANNELS][ROCKETNET_LIVE_NUM_BUCKETS];
static uint32_t live_matrix_table_received[LIVE_MATRIX_DIRECTIONS][LIVE_MATRIX_MODES][LIVE_MATRIX_CHANNELS][ROCKETNET_LIVE_NUM_BUCKETS];

static struct live_matrix_direction_t live_matrix_directions[LIVE_MATRIX_DIRECTIONS];

struct live_matrix_t
{
    rocketnet_live_config_t live;
    struct rocketnet_message_config_t message_config;
    struct rocketnet_message_server_t * server;
    struct rocketnet_message_client_t * client;
    char server_address[64];
    double wall_start;
    double offset;
    uint32_t cycle;
    uint64_t failures;
    uint64_t edge_failures;         // probes that never arrived: named, then the matrix still runs
    uint64_t verified;
    uint64_t not_delivered;              // modes 0, 1 and 4: lost or superseded, counted, not failed
    uint64_t refusals;
    uint64_t iteration;
};

static double live_matrix_now( struct live_matrix_t * m )
{
    return 1000.0 + ( rocketnet_platform_time() - m->wall_start ) + m->offset;
}

// The pass bound is the case's own finish time, measured as the wall-clock
// duration of the case body. Whole-process CPU time (getrusage RUSAGE_SELF)
// sums the parallel transport worker threads and the kernel time servicing
// page faults, so it over-counts the case's work (it can exceed the wall
// clock), scaling with the member's parallelism rather than with how long the
// case takes; it is therefore a broken instrument for the 10 s bound.

static void live_matrix_fail( struct live_matrix_t * m, const char * label, int direction, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    // section 3: fail <label> <cycle> <direction> <mode> <channel> <sequence> <reason> <bytes>
    if ( m->failures < LIVE_MATRIX_MAX_FAIL_LINES )
        printf( "fail %s %u %d %d %d %lld %s %d\n", label, m->cycle, direction, mode, channel, (long long) sequence, reason, bytes );
    m->failures++;
}

static uint64_t live_matrix_counter( const struct rocketnet_counters_t * counters, int index )
{
    return counters ? counters->values[index] : 0;
}

static const struct rocketnet_counters_t * live_matrix_sender_counters( struct live_matrix_t * m, int direction )
{
    return direction == 0 ? rocketnet_message_client_counters( m->client ) : rocketnet_message_server_counters( m->server );
}

// ---------------------------------------------------------------
//                     pacing by the sender's room (docs/LIVE-XDP.md section 6)
// ---------------------------------------------------------------

// A start message the region has no room for ends the connection
// (MESSAGE_MEMORY_EXHAUSTED), and the region frees a message only when the
// peer acks it (or, for an unreliable one, writes it), so the sender asks the
// library for its room before each start message: a send is admitted only
// while it fits and leaves at least half of the pool it takes from free, the
// store class for a message up to one block stride, the send spans above it.
// Every mode reads the same room, whether its messages wait for an ack or not,
// and nothing is released on a timer.

// internal, defined in sdk/rocketnet_message_server.cpp (rn_region_send_fits);
// not in rocketnet.h and not exported
int rn_message_client_send_fits( struct rocketnet_message_client_t * client, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );
int rn_message_server_send_fits( struct rocketnet_message_server_t * server, int client_index, int bytes, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages );

static int live_matrix_unreliable_queue( int mode, uint32_t length )
{
    return ( ( mode == 0 || mode == 1 ) && !live_matrix_promoted( mode, length ) ) || ( mode == 5 && length <= ROCKETNET_LIVE_UNRELIABLE_LIMIT );
}

// 1 when the sender's room admits a send of length now
static int live_matrix_room_admits( struct live_matrix_t * m, int direction, uint32_t length, uint64_t * free_bytes, uint64_t * pool_bytes, uint32_t * free_messages )
{
    const int fits = direction == 0 ? rn_message_client_send_fits( m->client, (int) length, free_bytes, pool_bytes, free_messages )
                                    : rn_message_server_send_fits( m->server, 0, (int) length, free_bytes, pool_bytes, free_messages );
    // Keep half the pool free for ordinary traffic, but admit a message larger
    // than half the pool when it is otherwise empty. A fixed half-pool reserve
    // would permanently starve the new 1 MiB default's largest messages.
    if ( !fits || *free_bytes < (uint64_t) length || *pool_bytes < (uint64_t) length || *free_messages < 1 )
        return 0;
    const uint64_t reserve = (uint64_t) length > *pool_bytes / 2 ? *pool_bytes - length : *pool_bytes / 2;
    return *free_bytes - length >= reserve;
}

// ---------------------------------------------------------------
//                     receipts (section 5, modes 5, 6, 7)
// ---------------------------------------------------------------

static struct live_matrix_receipt_entry_t * live_matrix_receipt_slot( struct live_matrix_direction_t * d, uint32_t serial, int insert )
{
    uint32_t index = ( serial * 2654435761u ) & ( LIVE_MATRIX_RECEIPT_TABLE - 1 );
    for ( int probe = 0; probe < LIVE_MATRIX_RECEIPT_TABLE; probe++ )
    {
        struct live_matrix_receipt_entry_t * e = &d->receipts[index];
        if ( e->serial == serial )
            return e;
        if ( e->serial == 0 )
            return insert ? e : NULL;
        index = ( index + 1 ) & ( LIVE_MATRIX_RECEIPT_TABLE - 1 );
    }
    return NULL;
}

static void live_matrix_take_receipts( struct live_matrix_t * m, int direction )
{
    struct live_matrix_direction_t * d = &live_matrix_directions[direction];
    static struct rocketnet_message_receipt_t receipts[4096];
    const int count = direction == 0 ? rocketnet_message_client_receipts( m->client, receipts, 4096 )
                                     : rocketnet_message_server_receipts( m->server, 0, receipts, 4096 );
    if ( count < 0 || count > 4096 )
    {
        live_matrix_fail( m, "receipt", direction, -1, -1, 0, "receipt-buffer-too-small", count );
        return;
    }
    for ( int i = 0; i < count; i++ )
    {
        struct live_matrix_receipt_entry_t * e = live_matrix_receipt_slot( d, receipts[i].serial, 0 );
        if ( !e )
        {
            live_matrix_fail( m, "receipt", direction, -1, -1, receipts[i].serial, "unknown-serial", 0 );
            continue;
        }
        if ( e->receipted )
        {
            live_matrix_fail( m, "receipt", direction, e->mode, -1, receipts[i].serial, "second-receipt", (int) e->length );
            continue;
        }
        e->receipted = 1;
        d->receipts_seen++;
        if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED )
        {
            d->receipts_acked++;
        }
        else if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_LOSS )
        {
            d->receipts_lost++;
            // modes 6 and 7, and 5 as a reliable block, get exactly one ACKED and no LOSS
            if ( e->mode != 5 || e->length > ROCKETNET_LIVE_UNRELIABLE_LIMIT )
                live_matrix_fail( m, "receipt", direction, e->mode, -1, receipts[i].serial, "loss-on-reliable", (int) e->length );
        }
        else
        {
            live_matrix_fail( m, "receipt", direction, e->mode, -1, receipts[i].serial, "bad-status", (int) e->length );
        }
    }
}

// ---------------------------------------------------------------
//                     receive (sections 3 and 5)
// ---------------------------------------------------------------

static void live_matrix_arrival( struct live_matrix_t * m, int direction, int receive_channel, const uint8_t * data, int bytes )
{
    struct live_matrix_direction_t * d = &live_matrix_directions[direction];

    if ( bytes < 1 || ( data[0] & ROCKETNET_LIVE_TAG_MASK ) != ROCKETNET_LIVE_TAG_BASE || ( ( data[0] >> 3 ) & 1 ) != direction )
    {
        live_matrix_fail( m, "arrival", direction, -1, receive_channel, -1, "bad-header", bytes );
        return;
    }
    const int mode = data[0] & 7;
    const int channel = bytes >= 2 ? data[1] : receive_channel;

    // section 5: codes 1, 3, 4 and 7 on the channel sent, 0, 2, 5 and 6 on channel 0
    const int want_channel = live_matrix_carries_channel( mode ) ? channel : 0;
    if ( receive_channel != want_channel || channel >= live_matrix_channels( mode ) )
    {
        live_matrix_fail( m, "arrival", direction, mode, channel, -1, "receive-channel", bytes );
        return;
    }

    struct live_matrix_stream_t * s = &d->streams[mode][channel];
    const int64_t expected = live_matrix_ordered( mode ) ? (int64_t) s->expected : -1;
    int offset = -1;
    const int result = rocketnet_live_check( &m->live, data, bytes, direction, mode, channel, m->cycle, expected, &offset );
    if ( result != ROCKETNET_LIVE_OK )
    {
        live_matrix_fail( m, "verify", direction, mode, channel, bytes >= LIVE_MATRIX_TINY ? (int64_t) rocketnet_live_sequence( data, bytes ) : expected,
                          rocketnet_live_result_name( result ), bytes );
        return;
    }

    const uint32_t length = (uint32_t) bytes;
    const int bucket = rocketnet_live_bucket( &m->live, mode, length );

    if ( bytes < LIVE_MATRIX_TINY && !live_matrix_ordered( mode ) )
    {
        // the stated blind spot: a tiny message carries no sequence, so it is
        // counted by length; more arrivals than sends is a duplicate
        d->tiny_received[mode][bytes]++;
        if ( d->tiny_received[mode][bytes] > d->tiny_sent[mode][bytes] )
            live_matrix_fail( m, "arrival", direction, mode, channel, -1, "duplicate-tiny", bytes );
        s->received++;
        live_matrix_table_received[direction][mode][channel][bucket]++;
        m->verified++;
        return;
    }

    const uint32_t q = bytes >= LIVE_MATRIX_TINY ? rocketnet_live_sequence( data, bytes ) : s->expected;
    if ( q >= s->next || q >= LIVE_MATRIX_MAX_SEQUENCES )
    {
        live_matrix_fail( m, "arrival", direction, mode, channel, q, "never-sent", bytes );
        return;
    }
    if ( s->seen[q / 64] & ( 1ull << ( q % 64 ) ) )
    {
        live_matrix_fail( m, "arrival", direction, mode, channel, q, "duplicate", bytes );
        return;
    }
    if ( live_matrix_ordered( mode ) && q != s->expected )
    {
        live_matrix_fail( m, "arrival", direction, mode, channel, q, "out-of-order", bytes );
        return;
    }
    if ( live_matrix_sequenced( mode ) && s->last >= 0 && (int64_t) q <= s->last )
    {
        live_matrix_fail( m, "arrival", direction, mode, channel, q, "not-increasing", bytes );
        return;
    }

    if ( live_matrix_sequenced( mode ) && s->last >= 0 )
        s->lost_sequenced += (uint32_t) ( (int64_t) q - s->last - 1 );

    s->seen[q / 64] |= 1ull << ( q % 64 );
    s->last = q;
    s->received++;
    if ( live_matrix_ordered( mode ) )
        s->expected++;
    live_matrix_table_received[direction][mode][channel][bucket]++;
    m->verified++;
}

static void live_matrix_receive( struct live_matrix_t * m )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    int channel = 0;
    {
        while ( rocketnet_message_server_receive( m->server, NULL, &channel, &data, &bytes ) )
        {
            live_matrix_arrival( m, 0, channel, data, bytes );
            rocketnet_message_server_release( m->server, data );
        }
        while ( rocketnet_message_client_receive( m->client, &channel, &data, &bytes ) )
        {
            live_matrix_arrival( m, 1, channel, data, bytes );
            rocketnet_message_client_release( m->client, data );
        }
    }
}

// ---------------------------------------------------------------
//                     send (section 4 sizes, section 5 streams)
// ---------------------------------------------------------------

static uint32_t live_matrix_start_finish( struct live_matrix_t * m, int direction, int mode, int channel, uint32_t sequence, uint32_t length )
{
    uint8_t * p = direction == 0 ? rocketnet_message_client_start( m->client, channel, mode, (int) length )
                                 : rocketnet_message_server_start( m->server, 0, channel, mode, (int) length );
    if ( !p )
        return 0;
    const int written = rocketnet_live_generate( &m->live, direction, mode, channel, m->cycle, sequence, p, (int) length );
    if ( written != (int) length )
    {
        if ( direction == 0 ) rocketnet_message_client_abort( m->client ); else rocketnet_message_server_abort( m->server, 0 );
        return 0;
    }
    return direction == 0 ? rocketnet_message_client_finish( m->client, written )
                          : rocketnet_message_server_finish( m->server, 0, written );
}

static int live_matrix_ordered_done( struct live_matrix_direction_t * d, int channel )
{
    const struct live_matrix_stream_t * a = &d->streams[3][channel];
    const struct live_matrix_stream_t * b = &d->streams[7][channel];
    return a->next >= a->total && b->next >= b->total;
}

// the next sequence this stream sends, or -1 when it has none now
static int64_t live_matrix_next_sequence( struct live_matrix_direction_t * d, int mode, int channel )
{
    const struct live_matrix_stream_t * s = &d->streams[mode][channel];
    if ( !s->active )
        return -1;
    if ( s->next < s->total )
        return s->next;
    // section 5: mode 4's closing message follows the channel's last ordered send
    if ( mode == 4 && s->next <= s->closing && live_matrix_ordered_done( d, channel ) )
        return s->closing;
    return -1;
}

static void live_matrix_send( struct live_matrix_t * m, int direction )
{
    struct live_matrix_direction_t * d = &live_matrix_directions[direction];
    const int streams = LIVE_MATRIX_MODES * LIVE_MATRIX_CHANNELS;
    d->reliable_tokens += LIVE_MATRIX_RELIABLE_RATE;
    if ( d->reliable_tokens > LIVE_MATRIX_RELIABLE_RATE )
        d->reliable_tokens = LIVE_MATRIX_RELIABLE_RATE;
    d->unreliable_tokens += LIVE_MATRIX_UNRELIABLE_RATE;
    if ( d->unreliable_tokens > LIVE_MATRIX_UNRELIABLE_RATE )
        d->unreliable_tokens = LIVE_MATRIX_UNRELIABLE_RATE;
    int idle = 0;
    while ( idle < streams && ( d->reliable_tokens > 0 || d->unreliable_tokens > 0 ) )
    {
        // round robin across every stream, so every mode and channel is in flight at once
        const int mode = d->cursor % LIVE_MATRIX_MODES;
        const int channel = d->cursor / LIVE_MATRIX_MODES;
        const int64_t q = live_matrix_next_sequence( d, mode, channel );
        if ( q < 0 )
        {
            d->cursor = ( d->cursor + 1 ) % streams;
            idle++;
            continue;
        }
        const uint32_t sequence = (uint32_t) q;
        const uint32_t length = rocketnet_live_length( &m->live, mode, channel, sequence );
        const int unreliable_queue = live_matrix_unreliable_queue( mode, length );
        uint64_t free_bytes = 0, pool_bytes = 0;
        uint32_t free_messages = 0;
        if ( ( unreliable_queue ? d->unreliable_tokens : d->reliable_tokens ) <= 0 || !live_matrix_room_admits( m, direction, length, &free_bytes, &pool_bytes, &free_messages ) )
        {
            // held back: the stream waits, the others go on
            d->paced++;
            d->cursor = ( d->cursor + 1 ) % streams;
            idle++;
            continue;
        }

        const uint32_t serial = live_matrix_start_finish( m, direction, mode, channel, sequence, length );
        if ( serial == 0 )
        {
            live_matrix_fail( m, "send", direction, mode, channel, sequence, "refused", (int) length );
            printf( "room at the refusal: iteration %llu, pool %llu of %llu bytes free, %u messages free\n",
                    (unsigned long long) m->iteration, (unsigned long long) free_bytes, (unsigned long long) pool_bytes, free_messages );
            printf( "sender counters: store_full %llu queue_full %llu memory_exhausted %llu\n",
                    (unsigned long long) live_matrix_counter( live_matrix_sender_counters( m, direction ), ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ),
                    (unsigned long long) live_matrix_counter( live_matrix_sender_counters( m, direction ), ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ),
                    (unsigned long long) live_matrix_counter( live_matrix_sender_counters( m, direction ), ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ) );
            return;
        }
        struct live_matrix_stream_t * s = &d->streams[mode][channel];
        s->next = sequence + 1;
        s->sent++;
        if ( length < LIVE_MATRIX_TINY )
            d->tiny_sent[mode][length]++;
        if ( live_matrix_has_receipt( mode ) )
        {
            struct live_matrix_receipt_entry_t * e = live_matrix_receipt_slot( d, serial, 1 );
            live_matrix_check( e && e->serial == 0 );
            e->serial = serial;
            e->mode = (uint8_t) mode;
            e->length = length;
            e->receipted = 0;
            d->receipts_expected++;
        }
        live_matrix_table_sent[direction][mode][channel][rocketnet_live_bucket( &m->live, mode, length )]++;
        d->bytes_sent += length;
        d->messages_sent++;
        if ( unreliable_queue )
            d->unreliable_tokens -= length;
        else
            d->reliable_tokens -= length;
        idle = 0;
        d->cursor = ( d->cursor + 1 ) % streams;
    }
}

// ---------------------------------------------------------------
//                     completeness (section 5)
// ---------------------------------------------------------------

static int live_matrix_sends_done( struct live_matrix_direction_t * d )
{
    for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
        for ( int channel = 0; channel < LIVE_MATRIX_CHANNELS; channel++ )
            if ( live_matrix_next_sequence( d, mode, channel ) >= 0 )
                return 0;
    return 1;
}

// the count of sequences a direction still owes: every complete-mode
// sequence and tiny length, mode 4's closing message, and every receipt
static uint64_t live_matrix_owed( struct live_matrix_t * m, int direction, int report )
{
    struct live_matrix_direction_t * d = &live_matrix_directions[direction];
    uint64_t owed = 0;
    for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
    {
        for ( int channel = 0; channel < LIVE_MATRIX_CHANNELS; channel++ )
        {
            const struct live_matrix_stream_t * s = &d->streams[mode][channel];
            if ( !s->active )
                continue;
            for ( uint32_t q = 0; q < s->next && q < LIVE_MATRIX_MAX_SEQUENCES; q++ )
            {
                const uint32_t length = rocketnet_live_length( &m->live, mode, channel, q );
                const int tiny_counted = length < LIVE_MATRIX_TINY && !live_matrix_ordered( mode );
                const int due = ( live_matrix_complete( mode, length ) && !tiny_counted ) || ( mode == 4 && q == s->closing );
                if ( due && !( s->seen[q / 64] & ( 1ull << ( q % 64 ) ) ) )
                {
                    owed++;
                    if ( report )
                        live_matrix_fail( m, "complete", direction, mode, channel, q, "missing", (int) length );
                }
            }
        }
        for ( int length = 1; length < LIVE_MATRIX_TINY; length++ )
        {
            if ( !live_matrix_complete( mode, (uint32_t) length ) || live_matrix_ordered( mode ) )
                continue;           // the ordered modes check a tiny message against the sequence due
            if ( d->tiny_received[mode][length] < d->tiny_sent[mode][length] )
            {
                owed += d->tiny_sent[mode][length] - d->tiny_received[mode][length];
                if ( report )
                    live_matrix_fail( m, "complete", direction, mode, -1, -1, "missing-tiny", length );
            }
        }
    }
    if ( d->receipts_seen < d->receipts_expected )
    {
        owed += d->receipts_expected - d->receipts_seen;
        if ( report )
            live_matrix_fail( m, "complete", direction, -1, -1, -1, "missing-receipts", (int) ( d->receipts_expected - d->receipts_seen ) );
    }
    return owed;
}

// ---------------------------------------------------------------
//                     the connection
// ---------------------------------------------------------------

static int live_matrix_up( struct live_matrix_t * m )
{
    return rocketnet_message_client_state( m->client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
           rocketnet_message_server_client_connected( m->server, 0 ) &&
           rocketnet_message_client_disconnect_reason( m->client ) == ROCKETNET_DISCONNECT_REASON_NONE &&
           rocketnet_message_server_disconnect_reason( m->server, 0 ) == ROCKETNET_DISCONNECT_REASON_NONE;
}

static void live_matrix_pump( struct live_matrix_t * m )
{
    const double now = live_matrix_now( m );
    live_matrix_check( rocketnet_message_client_update( m->client, now ) == ROCKETNET_UPDATE_OK );
    live_matrix_check( rocketnet_message_server_update( m->server, now ) == ROCKETNET_UPDATE_OK );
}

static void live_matrix_flush( struct live_matrix_t * m )
{
    rocketnet_message_client_send_packets( m->client );
    rocketnet_message_server_send_packets( m->server );
}

// a connect that fails is a failure of the case, never a skip
static void live_matrix_connect( struct live_matrix_t * m )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    live_matrix_check( rocketnet_write_test_connect_token( m->server_address, rocketnet_random_uint64(), rocketnet_functional_token_timestamp( 0, live_matrix_now( m ) ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    m->client = rocketnet_message_client_create_ex( NULL, token, NULL, &m->message_config, NULL, 0, LIVE_MATRIX_SEND_KBPS, NULL, 1 );
    live_matrix_check( m->client );

    const double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        live_matrix_pump( m );
        live_matrix_flush( m );
        if ( live_matrix_up( m ) )
            break;
        if ( rocketnet_message_client_state( m->client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;
        rocketnet_platform_sleep( 0.0005 );
    }
    if ( !live_matrix_up( m ) )
    {
        printf( "fail connect %u client_state=%d server_connected=%d\n", m->cycle, rocketnet_message_client_state( m->client ),
                rocketnet_message_server_client_connected( m->server, 0 ) );
        live_matrix_check_failed( "connect failed", (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );
    }
}

// ---------------------------------------------------------------
//                     the over-maximum start (section 4)
// ---------------------------------------------------------------

static void live_matrix_over_maximum( struct live_matrix_t * m, int direction )
{
    for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
    {
        const int channels[2] = { 0, LIVE_MATRIX_CHANNELS - 1 };
        for ( int c = 0; c < 2; c++ )
        {
            const int channel = channels[c];
            uint32_t over = rocketnet_live_mode_max( &m->live, mode ) + 1;
            if ( mode <= 1 )
            {
                // The promotion rule: one past the single-record
                // maximum of codes 0 and 1 starts as its reliable twin, never a
                // refusal; abort gives the room back, so nothing is sent
                const uint64_t failed_before = live_matrix_counter( live_matrix_sender_counters( m, direction ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                uint8_t * q = direction == 0 ? rocketnet_message_client_start( m->client, channel, mode, (int) over )
                                             : rocketnet_message_server_start( m->server, 0, channel, mode, (int) over );
                const uint64_t failed_delta = live_matrix_counter( live_matrix_sender_counters( m, direction ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) - failed_before;
                if ( live_matrix_verbose )
                    printf( "promoted %d %d %u start=%d\n", mode, channel, over, q != NULL );
                if ( q == NULL || failed_delta != 0 )
                    live_matrix_fail( m, "over", direction, mode, channel, -1, "promotion-refused", (int) over );
                if ( q )
                {
                    if ( direction == 0 ) rocketnet_message_client_abort( m->client ); else rocketnet_message_server_abort( m->server, 0 );
                }
                // the refusal boundary left: max_message_bytes
                over = m->live.max_message_bytes + 1;
            }
            const int unreliable = mode <= 1 || mode == 5;
            const int counter = unreliable ? ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED : ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED;
            const int other = unreliable ? ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED : ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED;
            const uint64_t before = live_matrix_counter( live_matrix_sender_counters( m, direction ), counter );
            const uint64_t before_other = live_matrix_counter( live_matrix_sender_counters( m, direction ), other );
            uint8_t * p = direction == 0 ? rocketnet_message_client_start( m->client, channel, mode, (int) over )
                                         : rocketnet_message_server_start( m->server, 0, channel, mode, (int) over );
            const uint64_t delta = live_matrix_counter( live_matrix_sender_counters( m, direction ), counter ) - before;
            const uint64_t delta_other = live_matrix_counter( live_matrix_sender_counters( m, direction ), other ) - before_other;
            if ( live_matrix_verbose )
                printf( "refused %d %d %u %s %llu\n", mode, channel, over, rocketnet_counter_name( counter ), (unsigned long long) delta );
            if ( p != NULL || delta != 1 || delta_other != 0 )
            {
                live_matrix_fail( m, "over", direction, mode, channel, -1, p ? "start-succeeded" : "wrong-counter", (int) over );
                if ( p )
                {
                    if ( direction == 0 ) rocketnet_message_client_abort( m->client ); else rocketnet_message_server_abort( m->server, 0 );
                }
            }
            live_matrix_table_sent[direction][mode][channel][ROCKETNET_LIVE_BUCKET_OVER]++;
            if ( p == NULL && delta == 1 )
                live_matrix_table_received[direction][mode][channel][ROCKETNET_LIVE_BUCKET_OVER]++;
            m->refusals++;
        }
    }
}

// ---------------------------------------------------------------
//                     the edge probes (sections 4 and 5)
// ---------------------------------------------------------------

#define LIVE_MATRIX_PROBE_BASE              ( 1u << 20 )    // probe sequences, far past any matrix sequence

// a sequence of stream (mode, channel) whose length is length, in the
// boundary slots past LIVE_MATRIX_PROBE_BASE; 0 when there is none
static uint32_t live_matrix_probe_sequence( struct live_matrix_t * m, int mode, int channel, uint32_t length, uint32_t after )
{
    for ( uint32_t q = after; q < after + 32u * 2u * ROCKETNET_LIVE_MAX_BOUNDARIES; q++ )
    {
        if ( ( q + (uint32_t) channel ) % 32u == 0 && rocketnet_live_length( &m->live, mode, channel, q ) == length )
            return q;
    }
    return 0;
}

// One message, alone on the connection, must arrive verified on the channel
// section 5 names. Returns 1 when it did.
static int live_matrix_probe_one( struct live_matrix_t * m, int direction, int mode, int channel, uint32_t sequence, uint32_t length )
{
    const uint32_t serial = live_matrix_start_finish( m, direction, mode, channel, sequence, length );
    if ( serial == 0 )
    {
        live_matrix_fail( m, "edge", direction, mode, channel, sequence, "refused", (int) length );
        return 0;
    }
    if ( live_matrix_has_receipt( mode ) )
    {
        struct live_matrix_direction_t * d = &live_matrix_directions[direction];
        struct live_matrix_receipt_entry_t * e = live_matrix_receipt_slot( d, serial, 1 );
        live_matrix_check( e && e->serial == 0 );
        e->serial = serial;
        e->mode = (uint8_t) mode;
        e->length = length;
        e->receipted = 0;
        d->receipts_expected++;
    }
    const int want_channel = live_matrix_carries_channel( mode ) ? channel : 0;
    const double start = rocketnet_platform_time();
    int arrived = 0;
    while ( !arrived && rocketnet_platform_time() - start < 0.25 )
    {
        live_matrix_flush( m );
        live_matrix_pump( m );
        live_matrix_check( live_matrix_up( m ) );
        {
            const uint8_t * data = NULL;
            int bytes = 0;
            int c = 0;
            while ( direction == 0 ? rocketnet_message_server_receive( m->server, NULL, &c, &data, &bytes ) : rocketnet_message_client_receive( m->client, &c, &data, &bytes ) )
            {
                const int result = rocketnet_live_check( &m->live, data, bytes, direction, mode, channel, m->cycle, -1, NULL );
                if ( result != ROCKETNET_LIVE_OK || rocketnet_live_sequence( data, bytes ) != sequence )
                    live_matrix_fail( m, "edge", direction, mode, channel, sequence, result != ROCKETNET_LIVE_OK ? rocketnet_live_result_name( result ) : "wrong-sequence", bytes );
                else if ( c != want_channel )
                    live_matrix_fail( m, "edge", direction, mode, channel, sequence, "receive-channel", bytes );
                else
                    arrived = 1;
                if ( direction == 0 ) rocketnet_message_server_release( m->server, data ); else rocketnet_message_client_release( m->client, data );
            }
        }
        live_matrix_take_receipts( m, 0 );
        live_matrix_take_receipts( m, 1 );
    }
    return arrived;
}

// Section 4's single-record edges E - 1, E and E + 1 of every mode, and code
// 1's transport payload edges 1,404 to 1,406, one message at a time in both
// directions, each arriving on the channel section 5 names (codes 1, 3, 4
// and 7 on the channel sent, the others on channel 0). An unreliable probe
// gets three tries, so one loss is not a finding; a probe that never arrives
// is named by its length.
static void live_matrix_edge_probes( struct live_matrix_t * m )
{
    const int channel = 1;
    for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
    {
        for ( int mode = 0; mode < LIVE_MATRIX_MODES && m->failures == 0; mode++ )
        {
            uint32_t lengths[8];
            int n = 0;
            const uint32_t e = rocketnet_live_edge( &m->live, mode );
            const uint32_t max_m = rocketnet_live_mode_max( &m->live, mode );
            const uint32_t candidates[6] = { e - 1, e, e + 1, 1404, 1405, 1406 };
            for ( int i = 0; i < ( mode == 1 ? 6 : 3 ); i++ )
            {
                const uint32_t l = candidates[i] > max_m ? max_m : candidates[i];
                int seen = 0;
                for ( int j = 0; j < n; j++ )
                    seen |= lengths[j] == l;
                if ( !seen )
                    lengths[n++] = l;
            }
            for ( int i = 0; i < n && m->failures == 0; i++ )
            {
                const int tries = live_matrix_complete( mode, lengths[i] ) ? 1 : 3;
                uint32_t sequence = LIVE_MATRIX_PROBE_BASE;
                int arrived = 0;
                for ( int t = 0; t < tries && !arrived && m->failures == 0; t++ )
                {
                    sequence = live_matrix_probe_sequence( m, mode, channel, lengths[i], sequence + 1 );
                    live_matrix_check( sequence != 0 );
                    arrived = live_matrix_probe_one( m, direction, mode, channel, sequence, lengths[i] );
                }
                if ( !arrived && m->failures == 0 )
                {
                    live_matrix_fail( m, "edge", direction, mode, channel, sequence, "never-arrived", (int) lengths[i] );
                    m->failures--;
                    m->edge_failures++;
                }
                else if ( arrived && live_matrix_verbose )
                    printf( "edge %d %d %u arrived on channel %d\n", direction, mode, lengths[i], live_matrix_carries_channel( mode ) ? channel : 0 );
            }
        }
    }

    // every probe's receipt is in before the matrix starts its own sends
    const double start = rocketnet_platform_time();
    while ( m->failures == 0 && rocketnet_platform_time() - start < 2.0 &&
            ( live_matrix_directions[0].receipts_seen < live_matrix_directions[0].receipts_expected ||
              live_matrix_directions[1].receipts_seen < live_matrix_directions[1].receipts_expected ) )
    {
        live_matrix_flush( m );
        live_matrix_pump( m );
        live_matrix_check( live_matrix_up( m ) );
        live_matrix_take_receipts( m, 0 );
        live_matrix_take_receipts( m, 1 );
    }
    for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
    {
        const struct live_matrix_direction_t * d = &live_matrix_directions[direction];
        if ( m->failures == 0 && d->receipts_seen != d->receipts_expected )
            live_matrix_fail( m, "edge", direction, -1, -1, -1, "missing-receipts", (int) ( d->receipts_expected - d->receipts_seen ) );
    }
    fflush( stdout );
}

// ---------------------------------------------------------------
//                     one window
// ---------------------------------------------------------------

static void live_matrix_window_reset( struct live_matrix_t * m )
{
    for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
    {
        struct live_matrix_direction_t * d = &live_matrix_directions[direction];
        memset( d, 0, sizeof( *d ) );
        for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
        {
            const uint32_t total = rocketnet_live_sequences_per_stream( &m->live, mode );
            live_matrix_check( total + 64 <= LIVE_MATRIX_MAX_SEQUENCES );
            for ( int channel = 0; channel < live_matrix_channels( mode ); channel++ )
            {
                struct live_matrix_stream_t * s = &d->streams[mode][channel];
                s->active = 1;
                s->total = total;
                s->last = -1;
                // mode 4's closing message must be identified by its sequence, so
                // it is the first sequence past the matrix that is 7 bytes or more
                s->closing = total;
                while ( mode == 4 && rocketnet_live_length( &m->live, 4, channel, s->closing ) < LIVE_MATRIX_TINY )
                    s->closing++;
                if ( mode != 4 )
                    s->closing = 0;
            }
        }
    }
}

static const int live_matrix_window_counters[] =
{
    ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED,
    ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL,
    ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED,
    ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED,
    ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION,
    ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL,
    ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK,
    ROCKETNET_COUNTER_MESSAGE_BAD_RING,
};

static void live_matrix_window( struct live_matrix_t * m, double deadline )
{
    live_matrix_window_reset( m );

    uint64_t server_before[sizeof( live_matrix_window_counters ) / sizeof( int )];
    for ( size_t i = 0; i < sizeof( live_matrix_window_counters ) / sizeof( int ); i++ )
        server_before[i] = live_matrix_counter( rocketnet_message_server_counters( m->server ), live_matrix_window_counters[i] );

    live_matrix_over_maximum( m, 0 );
    live_matrix_over_maximum( m, 1 );

    const double window_start = rocketnet_platform_time();
    uint64_t iterations = 0;
    int tail = -1;
    while ( 1 )
    {
        live_matrix_pump( m );
        live_matrix_receive( m );
        live_matrix_take_receipts( m, 0 );
        live_matrix_take_receipts( m, 1 );
        if ( !live_matrix_up( m ) )
        {
            printf( "fail disconnect %u client_state=%d client_reason=%d server_connected=%d server_reason=%d\n", m->cycle,
                    rocketnet_message_client_state( m->client ), rocketnet_message_client_disconnect_reason( m->client ),
                    rocketnet_message_server_client_connected( m->server, 0 ), rocketnet_message_server_disconnect_reason( m->server, 0 ) );
            for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
            {
                uint64_t c0_free = 0, c0_pool = 0, c1_free = 0, c1_pool = 0, span_free = 0, span_pool = 0;
                uint32_t free_messages = 0;
                if ( direction == 0 )
                {
                    rn_message_client_send_fits( m->client, 1, &c0_free, &c0_pool, &free_messages );
                    rn_message_client_send_fits( m->client, 256, &c1_free, &c1_pool, &free_messages );
                    rn_message_client_send_fits( m->client, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES, &span_free, &span_pool, &free_messages );
                }
                else
                {
                    rn_message_server_send_fits( m->server, 0, 1, &c0_free, &c0_pool, &free_messages );
                    rn_message_server_send_fits( m->server, 0, 256, &c1_free, &c1_pool, &free_messages );
                    rn_message_server_send_fits( m->server, 0, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES, &span_free, &span_pool, &free_messages );
                }
                const struct rocketnet_counters_t * c = live_matrix_sender_counters( m, direction );
                printf( "disconnect %s region: store_c0=%llu/%llu store_c1=%llu/%llu spans=%llu/%llu free_messages=%u "
                        "counters: store_full=%llu queue_full=%llu memory_exhausted=%llu bad_ring=%llu bad_block=%llu protocol_violation=%llu\n",
                        direction == 0 ? "client" : "server",
                        (unsigned long long) c0_free, (unsigned long long) c0_pool,
                        (unsigned long long) c1_free, (unsigned long long) c1_pool,
                        (unsigned long long) span_free, (unsigned long long) span_pool,
                        free_messages,
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ),
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ),
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED ),
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_BAD_RING ),
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK ),
                        (unsigned long long) live_matrix_counter( c, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION ) );
            }
            live_matrix_check_failed( "unplanned disconnect", (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ );
        }
        if ( m->failures > 0 )
            break;
        live_matrix_send( m, 0 );
        live_matrix_send( m, 1 );
        live_matrix_flush( m );
        iterations++;
        m->iteration++;

        if ( tail < 0 && live_matrix_sends_done( &live_matrix_directions[0] ) && live_matrix_sends_done( &live_matrix_directions[1] ) &&
             live_matrix_owed( m, 0, 0 ) == 0 && live_matrix_owed( m, 1, 0 ) == 0 )
            tail = 50;          // a short tail, so a late duplicate is still seen
        if ( tail > 0 && --tail == 0 )
            break;
        if ( rocketnet_platform_time() > deadline )
        {
            live_matrix_fail( m, "window", -1, -1, -1, -1, "wall-bound", 0 );
            break;
        }
    }

    // count what modes 0, 1 and 4 did not deliver (summed per mode: a 1-byte
    // message of a channel-less mode is credited to channel 0), then name
    // what is missing
    for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
    {
        struct live_matrix_direction_t * d = &live_matrix_directions[direction];
        for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
        {
            if ( mode != 0 && mode != 1 && mode != 4 )
                continue;
            int64_t sent = 0;
            int64_t received = 0;
            for ( int channel = 0; channel < LIVE_MATRIX_CHANNELS; channel++ )
            {
                sent += d->streams[mode][channel].sent;
                received += d->streams[mode][channel].received;
            }
            if ( sent > received )
                m->not_delivered += (uint64_t) ( sent - received );
        }
        // the modes without completeness still deliver on the loopback: each
        // (direction, mode) that sent must have received
        for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
        {
            int64_t sent = 0;
            int64_t received = 0;
            for ( int channel = 0; channel < LIVE_MATRIX_CHANNELS; channel++ )
            {
                sent += d->streams[mode][channel].sent;
                received += d->streams[mode][channel].received;
            }
            if ( live_matrix_verbose )
                printf( "delivery %u %d %d %lld/%lld\n", m->cycle, direction, mode, (long long) received, (long long) sent );
            if ( sent > 0 && received == 0 )
                live_matrix_fail( m, "delivery", direction, mode, -1, -1, "nothing-delivered", 0 );
        }
        live_matrix_owed( m, direction, 1 );
    }

    for ( size_t i = 0; i < sizeof( live_matrix_window_counters ) / sizeof( int ); i++ )
    {
        const int index = live_matrix_window_counters[i];
        const uint64_t client = live_matrix_counter( rocketnet_message_client_counters( m->client ), index );
        const uint64_t server = live_matrix_counter( rocketnet_message_server_counters( m->server ), index ) - server_before[i];
        if ( client != 0 || server != 0 )
        {
            printf( "fail counter %u %s client=%llu server=%llu\n", m->cycle, rocketnet_counter_name( index ),
                    (unsigned long long) client, (unsigned long long) server );
            m->failures++;
        }
    }

    if ( !live_matrix_verbose && m->failures == 0 && m->edge_failures == 0 )
        return;

    printf( "window %u: %.3f s, %llu iterations, c2s %llu messages %llu bytes, s2c %llu messages %llu bytes, receipts c2s %u acked %u lost, s2c %u acked %u lost, sequenced stale %llu superseded %llu, sends held back for room c2s %llu s2c %llu\n",
            m->cycle, rocketnet_platform_time() - window_start, (unsigned long long) iterations,
            (unsigned long long) live_matrix_directions[0].messages_sent, (unsigned long long) live_matrix_directions[0].bytes_sent,
            (unsigned long long) live_matrix_directions[1].messages_sent, (unsigned long long) live_matrix_directions[1].bytes_sent,
            live_matrix_directions[0].receipts_acked, live_matrix_directions[0].receipts_lost,
            live_matrix_directions[1].receipts_acked, live_matrix_directions[1].receipts_lost,
            (unsigned long long) ( live_matrix_counter( rocketnet_message_server_counters( m->server ), ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) +
                                   live_matrix_counter( rocketnet_message_client_counters( m->client ), ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE ) ),
            (unsigned long long) ( live_matrix_counter( rocketnet_message_server_counters( m->server ), ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) +
                                   live_matrix_counter( rocketnet_message_client_counters( m->client ), ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED ) ),
            (unsigned long long) live_matrix_directions[0].paced, (unsigned long long) live_matrix_directions[1].paced );
    printf( "window %u counters: receipt acked client %llu server %llu, receipt loss client %llu server %llu, resent client %llu server %llu\n", m->cycle,
            (unsigned long long) live_matrix_counter( rocketnet_message_client_counters( m->client ), ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ),
            (unsigned long long) live_matrix_counter( rocketnet_message_server_counters( m->server ), ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ),
            (unsigned long long) live_matrix_counter( rocketnet_message_client_counters( m->client ), ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ),
            (unsigned long long) live_matrix_counter( rocketnet_message_server_counters( m->server ), ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ),
            (unsigned long long) live_matrix_counter( rocketnet_message_client_counters( m->client ), ROCKETNET_COUNTER_MESSAGE_RESENT ),
            (unsigned long long) live_matrix_counter( rocketnet_message_server_counters( m->server ), ROCKETNET_COUNTER_MESSAGE_RESENT ) );
    fflush( stdout );
}

// ---------------------------------------------------------------
//                     the table
// ---------------------------------------------------------------

static void live_matrix_print_table()
{
    printf( "test_loopback_transport_live_message_matrix table: received/sent per (direction, mode, channel, size bucket); over is refused/attempted\n" );
    printf( "dir mode ch" );
    for ( int b = 0; b < ROCKETNET_LIVE_NUM_BUCKETS; b++ )
        printf( " %12s", rocketnet_live_bucket_name( b ) );
    printf( "\n" );
    for ( int direction = 0; direction < LIVE_MATRIX_DIRECTIONS; direction++ )
        for ( int mode = 0; mode < LIVE_MATRIX_MODES; mode++ )
            for ( int channel = 0; channel < LIVE_MATRIX_CHANNELS; channel++ )
            {
                uint32_t any = 0;
                for ( int b = 0; b < ROCKETNET_LIVE_NUM_BUCKETS; b++ )
                    any += live_matrix_table_sent[direction][mode][channel][b];
                if ( !any )
                    continue;
                printf( "%3d %4d %2d", direction, mode, channel );
                for ( int b = 0; b < ROCKETNET_LIVE_NUM_BUCKETS; b++ )
                {
                    char cell[32];
                    snprintf( cell, sizeof( cell ), "%u/%u", live_matrix_table_received[direction][mode][channel][b], live_matrix_table_sent[direction][mode][channel][b] );
                    printf( " %12s", cell );
                }
                printf( "\n" );
            }
}

// ---------------------------------------------------------------
//                     the case
// ---------------------------------------------------------------

void live_message_matrix_loopback()
{
    const double case_start = rocketnet_platform_time();
    static struct live_matrix_t matrix;
    struct live_matrix_t * m = &matrix;
    memset( m, 0, sizeof( *m ) );
    char verbose[256];
    live_matrix_verbose = rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) );
    memset( live_matrix_table_sent, 0, sizeof( live_matrix_table_sent ) );
    memset( live_matrix_table_received, 0, sizeof( live_matrix_table_received ) );

    // the run's config (section 4): the message defaults, so the reliable
    // maximum is 1 MiB, with bounded room shared by every stream: a deeper
    // send region and queue, more packets per update, and receive lists
    // sized so nothing is dropped for want of room
    rocketnet_message_default_config( &m->message_config );
    m->message_config.num_channels = LIVE_MATRIX_CHANNELS;
    m->message_config.send_message_buffer_bytes = 128 * 1024 * 1024;
    m->message_config.send_queue_messages = 16384;
    m->message_config.packets_in_flight = 2048;
    m->message_config.packets_per_update = 64;
    m->message_config.unreliable_messages_per_update = 8192;
    m->message_config.max_message_receipts = 4096;
    m->message_config.reliable_messages_in_flight = 4096;
    m->message_config.receive_message_buffer_bytes = 1 << 20;

    m->live.seed = 0x5EED0001u;
    m->live.max_message_bytes = (uint32_t) m->message_config.max_message_bytes;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;

    m->server = rocketnet_message_server_create_ex( NULL, "127.0.0.1", &server_config, &m->message_config, NULL, 0, LIVE_MATRIX_SEND_KBPS, NULL, 1 );
    live_matrix_check( m->server );
    snprintf( m->server_address, sizeof( m->server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( rocketnet_message_server_transport( m->server ) ) );

    m->wall_start = rocketnet_platform_time();
    const double deadline = case_start + LIVE_MATRIX_WALL_BOUND;

    // the matrix window, ending in a clean disconnect the server sees
    m->cycle = 0;
    live_matrix_connect( m );
    memset( live_matrix_directions, 0, sizeof( live_matrix_directions ) );
    live_matrix_edge_probes( m );
    live_matrix_window( m, deadline );
    live_matrix_check( m->failures == 0 );
    rocketnet_client_transport_disconnect( rocketnet_message_client_transport( m->client ) );
    {
        const double start = rocketnet_platform_time();
        while ( rocketnet_message_server_client_connected( m->server, 0 ) && rocketnet_platform_time() - start < 2.0 )
        {
            live_matrix_check( rocketnet_message_server_update( m->server, live_matrix_now( m ) ) == ROCKETNET_UPDATE_OK );
            rocketnet_platform_sleep( 0.0005 );
        }
        live_matrix_check( !rocketnet_message_server_client_connected( m->server, 0 ) );
    }
    rn_native_message_client_stop_and_destroy( m->client );
    m->client = NULL;

    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( m->server );
    rocketnet_server_transport_stop( server_transport );
    const double stop_start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server_transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && rocketnet_platform_time() - stop_start < 5.0 )
    {
        rocketnet_platform_sleep( 0.001 );
        rocketnet_message_server_update( m->server, live_matrix_now( m ) );
    }
    live_matrix_check( rocketnet_server_transport_state( server_transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rn_native_message_server_stop_and_destroy( m->server );

    live_matrix_check( m->failures == 0 );
    const double elapsed = rocketnet_platform_time() - case_start;

    if ( live_matrix_verbose || m->failures != 0 || m->edge_failures != 0 || elapsed >= LIVE_MATRIX_TARGET_SECONDS )
    {
        live_matrix_print_table();
        printf( "test_loopback_transport_live_message_matrix: verified=%llu failures=%llu edge_failures=%llu not_delivered_0_1_4=%llu refusals=%llu elapsed=%.3f s (target %.0f s, max %.0f s)\n",
                (unsigned long long) m->verified, (unsigned long long) m->failures, (unsigned long long) m->edge_failures, (unsigned long long) m->not_delivered,
                (unsigned long long) m->refusals, elapsed, LIVE_MATRIX_TARGET_SECONDS, LIVE_MATRIX_WALL_BOUND );
        fflush( stdout );
    }

    live_matrix_check( m->edge_failures == 0 );
    live_matrix_check( elapsed < LIVE_MATRIX_WALL_BOUND );
}
