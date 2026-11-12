/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The receive side of the message matrix that tests/client_server/client.c
    and tests/client_server/server.c run against each other, shared so both
    ends judge the other side by one rule.

    Each end judges what the other side actually sent, never its own sends:
    the payload carries its own sequence (tests/live_verify.h), the length
    follows from the sequence, the cycle comes from the HELLO, and each sender
    reports the count it sent on every stream when it stops sending (the sent
    report). Against those:

    - every arrival verifies byte for byte (tests/live_verify.h);
    - a sequence at or past the sender's count is never-sent;
    - a sequence that arrives twice is a duplicate;
    - RELIABLE_ORDERED and its receipt form get every sequence in order;
    - the sequenced modes get strictly increasing sequences;
    - every owed sequence (modes 2, 3, 6, 7, and 0 and 5 above 1,411 where
      they travel as a reliable block) arrives by the end of the drain, or it
      is lost.

    A message under 7 bytes carries no sequence. On the ordered modes the
    sequence due is known and checked; on the others the receiver counts
    them per mode and length against the count the sent report implies (the
    stated blind spot of tests/live_verify.h).

    The control messages ride channel 31, which carries no matrix stream of
    the modes that keep a channel: HELLO (client to server, RELIABLE_ORDERED,
    and the server's answer), the progress report (UNRELIABLE_SEQUENCED,
    every 50 ms each way, which releases the sender's pacing ledger) and the
    sent report (RELIABLE_ORDERED, once, at the drain).

    No allocation, no syscall, no global state.
*/

#pragma once

#ifndef ROCKETNET_TESTS_MATRIX_RECEIVE_H
#define ROCKETNET_TESTS_MATRIX_RECEIVE_H

#include "tests/live_verify.h"

#include <stdint.h>
#include <string.h>

#define MATRIX_CONTROL_CHANNEL          31
#define MATRIX_NIBBLE_MASK              0xF0u
#define MATRIX_HELLO_NIBBLE             0xD0u
#define MATRIX_PROGRESS_NIBBLE          0xC0u
#define MATRIX_SENT_NIBBLE              0xE0u
#define MATRIX_HELLO_BYTES              17          // tag, then cap kbps, cycle, seed, max_message_bytes (u32 each); the label follows
#define MATRIX_HELLO_ANSWER_BYTES       5           // tag, then the cycle (u32)
#define MATRIX_PROGRESS_RECORD          10          // mode, channel, the highest owed-contiguous sequence (u32), bytes received (u32)
#define MATRIX_SENT_RECORD              6           // mode, channel, the count sent (u32)
#define MATRIX_PROGRESS_SECONDS         0.05
#define MATRIX_DRAIN_SECONDS            10.0        // the drain bound before the budget's share (reliable_timeout_ms)
#define MATRIX_WINDOW_WORDS             1024
#define MATRIX_WINDOW_BITS              65536u      // the seen-window per stream
#define MATRIX_REPORT_BYTES             ( 1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * MATRIX_PROGRESS_RECORD )

static inline void matrix_put_u32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) ( v >> 8 );
    p[2] = (uint8_t) ( v >> 16 );
    p[3] = (uint8_t) ( v >> 24 );
}

static inline uint32_t matrix_get_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

// codes 1, 3, 4 and 7 arrive on the channel sent, and run on channels 0 to
// 30; the channel-less codes 0, 2, 5 and 6 arrive on channel 0 and name all 32
static inline int matrix_carries_channel( int mode )
{
    return mode == 1 || mode == 3 || mode == 4 || mode == 7;
}

static inline int matrix_channels( int mode )
{
    return matrix_carries_channel( mode ) ? 31 : 32;
}

static inline int matrix_ordered( int mode )
{
    return mode == 3 || mode == 7;
}

static inline int matrix_sequenced( int mode )
{
    return mode == 1 || mode == 4;
}

// every sequence arrives: 2, 3, 6, 7, and 0 and 5 above 1,411 where they
// travel as a reliable block. Modes 1 and 4 may skip (superseded), and
// unreliable sends may be lost or dropped over budget.
static inline int matrix_owed( int mode, uint32_t length )
{
    if ( mode == 0 || mode == 5 )
    {
        return length > ROCKETNET_LIVE_UNRELIABLE_LIMIT;
    }
    return mode == 2 || mode == 3 || mode == 6 || mode == 7;
}

// the sends that wait in the sender's queue until acked: the reliable codes,
// and 0 and 5 above 1,411 and 1 above 1,405 where the promotion rule makes
// them reliable
static inline int matrix_reliable_path( int mode, uint32_t length )
{
    if ( mode == 0 || mode == 5 )
    {
        return length > ROCKETNET_LIVE_UNRELIABLE_LIMIT;
    }
    if ( mode == 1 )
    {
        return length > 1405u;
    }
    return 1;
}

// The pacing ledger (section 6): a sender keeps its reliable bytes not yet
// confirmed by the other side's progress reports under this many seconds of
// its send budget, so the drain stays short; with nothing outstanding one
// message of any size is admitted. A chosen bound, not a measured one.
#define MATRIX_LEDGER_SECONDS           4.0

static inline int matrix_ledger_admits( uint64_t outstanding, uint32_t length, uint32_t kbps )
{
    const double limit = (double) kbps * 125.0 * MATRIX_LEDGER_SECONDS;
    return outstanding == 0 || (double) ( outstanding + length ) <= limit;
}

// the reliable bytes of one stream's sequences [from, to), which a progress
// report confirming up to `to` releases from the ledger
static inline uint64_t matrix_ledger_bytes( const rocketnet_live_config_t * config, int mode, int channel, uint32_t from, uint32_t to )
{
    uint64_t bytes = 0;
    for ( uint32_t q = from; q < to; q++ )
    {
        const uint32_t length = rocketnet_live_length( config, mode, channel, q );
        if ( matrix_reliable_path( mode, length ) )
        {
            bytes += length;
        }
    }
    return bytes;
}

// a message under 7 bytes is counted per mode and length, unless the mode
// is ordered, where the sequence due is known
static inline int matrix_counted_tiny( int mode, uint32_t length )
{
    return length < ROCKETNET_LIVE_HEADER_BYTES && !matrix_ordered( mode );
}

// The receive state of one stream (direction, mode, sender's channel).
struct matrix_rx_stream_t
{
    uint64_t seen[MATRIX_WINDOW_WORDS];     // bit q: sequence q arrived, for q in ( high - 65,536, high ]
    uint32_t high;                          // the highest sequence that arrived
    int any;                                // a sequence arrived
    uint32_t owed;                          // every owed sequence below it arrived
    uint32_t expected;                      // ordered modes: the sequence due
    int64_t last;                           // sequenced modes: the last sequence, -1 for none
    uint32_t sent;                          // the sender's count, from its sent report
    int have_sent;
    uint32_t received;
    uint64_t bytes;
};

// One direction's receive state: every stream, and the tiny counts.
struct matrix_rx_t
{
    struct matrix_rx_stream_t streams[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_NUM_CHANNELS];
    uint32_t tiny_received[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_HEADER_BYTES];
    uint32_t tiny_sent[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_HEADER_BYTES];
    int have_sent_report;
};

static inline void matrix_rx_reset( struct matrix_rx_t * rx )
{
    memset( rx, 0, sizeof( *rx ) );
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
        {
            rx->streams[mode][channel].last = -1;
        }
    }
}

// 1 when sequence q is inside the stream's seen-window
static inline int matrix_rx_in_window( const struct matrix_rx_stream_t * s, uint32_t q )
{
    return s->any && q <= s->high && s->high - q < MATRIX_WINDOW_BITS;
}

static inline int matrix_rx_seen( const struct matrix_rx_stream_t * s, uint32_t q )
{
    if ( !matrix_rx_in_window( s, q ) )
    {
        return 0;
    }
    return ( s->seen[( q >> 6 ) & ( MATRIX_WINDOW_WORDS - 1 )] >> ( q & 63 ) ) & 1u ? 1 : 0;
}

static inline void matrix_rx_mark( struct matrix_rx_stream_t * s, uint32_t q )
{
    if ( !s->any )
    {
        s->any = 1;
        s->high = q;
    }
    else if ( q > s->high )
    {
        if ( q - s->high >= MATRIX_WINDOW_BITS )
        {
            memset( s->seen, 0, sizeof( s->seen ) );
        }
        else
        {
            for ( uint32_t k = s->high + 1; k <= q; k++ )
            {
                s->seen[( k >> 6 ) & ( MATRIX_WINDOW_WORDS - 1 )] &= ~( 1ull << ( k & 63 ) );
            }
        }
        s->high = q;
    }
    s->seen[( q >> 6 ) & ( MATRIX_WINDOW_WORDS - 1 )] |= 1ull << ( q & 63 );
}

// Move owed past every sequence that arrived or is not owed one by one,
// up to the sender's count once it is known, else up to the highest arrival.
static inline void matrix_rx_advance( const rocketnet_live_config_t * config, struct matrix_rx_stream_t * s, int mode, int channel )
{
    uint32_t limit = 0;
    if ( s->have_sent )
    {
        limit = s->sent;
    }
    else if ( s->any )
    {
        limit = s->high + 1u;
    }

    while ( s->owed < limit )
    {
        const uint32_t length = rocketnet_live_length( config, mode, channel, s->owed );
        if ( matrix_rx_seen( s, s->owed ) || !matrix_owed( mode, length ) || matrix_counted_tiny( mode, length ) )
        {
            s->owed++;
            continue;
        }
        break;
    }
}

// Judge one arrival of 7 bytes or more, or an ordered mode's tiny message,
// that tests/live_verify.h already verified. Returns NULL when it is
// accepted, else the reason; *sequence receives the sequence judged.
static inline const char * matrix_rx_accept( const rocketnet_live_config_t * config, struct matrix_rx_stream_t * s, int mode, int channel,
                                             const uint8_t * data, int bytes, int64_t * sequence )
{
    const uint32_t q = bytes >= ROCKETNET_LIVE_HEADER_BYTES ? rocketnet_live_sequence( data, bytes ) : s->expected;
    *sequence = (int64_t) q;

    if ( s->have_sent && q >= s->sent )
    {
        return "never-sent";
    }

    if ( s->any && q <= s->high && s->high - q >= MATRIX_WINDOW_BITS )
    {
        return "outside-window";        // more than 65,536 behind the newest: it cannot be judged, so it fails
    }

    if ( matrix_rx_seen( s, q ) )
    {
        return "duplicate";
    }

    if ( matrix_ordered( mode ) && q != s->expected )
    {
        return "out-of-order";
    }

    if ( matrix_sequenced( mode ) && s->last >= 0 && (int64_t) q <= s->last )
    {
        return "not-increasing";
    }

    matrix_rx_mark( s, q );
    s->received++;
    s->bytes += (uint64_t) bytes;
    if ( matrix_ordered( mode ) )
    {
        s->expected = q + 1u;
    }
    if ( matrix_sequenced( mode ) )
    {
        s->last = (int64_t) q;
    }
    matrix_rx_advance( config, s, mode, channel );
    return NULL;
}

// Count one tiny arrival of a mode that is not ordered. Returns NULL, or
// "duplicate-tiny" when more of that length arrived than the sender sent.
static inline const char * matrix_rx_tiny( struct matrix_rx_t * rx, int mode, int bytes )
{
    rx->tiny_received[mode][bytes]++;
    if ( rx->have_sent_report && rx->tiny_received[mode][bytes] > rx->tiny_sent[mode][bytes] )
    {
        return "duplicate-tiny";
    }
    return NULL;
}

// The tiny messages of each length a count of sends on one stream implies.
static inline void matrix_rx_count_tiny( const rocketnet_live_config_t * config, struct matrix_rx_t * rx, int mode, int channel, uint32_t count )
{
    if ( matrix_ordered( mode ) )
    {
        return;
    }

    // only the boundary slot r = 0 of the length rule holds a tiny length
    uint32_t q = ( ROCKETNET_LIVE_SLOTS - ( (uint32_t) channel % ROCKETNET_LIVE_SLOTS ) ) % ROCKETNET_LIVE_SLOTS;
    for ( ; q < count; q += ROCKETNET_LIVE_SLOTS )
    {
        const uint32_t length = rocketnet_live_length( config, mode, channel, q );
        if ( length < ROCKETNET_LIVE_HEADER_BYTES )
        {
            rx->tiny_sent[mode][length]++;
        }
    }
}

// Apply the sender's report of the counts it sent; a stream it does not
// name sent nothing. Calls fail( context, mode, channel, sequence, reason )
// for every arrival the report shows was never sent.
typedef void ( *matrix_rx_fail_fn )( void * context, int mode, int channel, int64_t sequence, const char * reason, int bytes );

static inline void matrix_rx_sent_report( const rocketnet_live_config_t * config, struct matrix_rx_t * rx, const uint8_t * data, int bytes,
                                          matrix_rx_fail_fn fail, void * context )
{
    if ( rx->have_sent_report )
    {
        fail( context, -1, MATRIX_CONTROL_CHANNEL, -1, "second-sent-report", bytes );
        return;
    }

    if ( bytes < 1 || ( bytes - 1 ) % MATRIX_SENT_RECORD != 0 )
    {
        fail( context, -1, MATRIX_CONTROL_CHANNEL, -1, "bad-sent-report", bytes );
        return;
    }

    rx->have_sent_report = 1;
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
        {
            rx->streams[mode][channel].have_sent = 1;
            rx->streams[mode][channel].sent = 0;
        }
    }

    for ( int off = 1; off + MATRIX_SENT_RECORD <= bytes; off += MATRIX_SENT_RECORD )
    {
        const int mode = data[off];
        const int channel = data[off + 1];
        if ( mode >= ROCKETNET_LIVE_NUM_MODES || channel >= matrix_channels( mode ) )
        {
            fail( context, mode, channel, -1, "bad-sent-record", bytes );
            continue;
        }
        rx->streams[mode][channel].sent = matrix_get_u32( data + off + 2 );
        matrix_rx_count_tiny( config, rx, mode, channel, rx->streams[mode][channel].sent );
    }

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int length = 1; length < ROCKETNET_LIVE_HEADER_BYTES; length++ )
        {
            if ( rx->tiny_received[mode][length] > rx->tiny_sent[mode][length] )
            {
                fail( context, mode, -1, -1, "duplicate-tiny", length );
            }
        }

        for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
        {
            struct matrix_rx_stream_t * s = &rx->streams[mode][channel];
            if ( s->any && s->high >= s->sent )
            {
                fail( context, mode, channel, (int64_t) s->high, "never-sent", 0 );
            }
            matrix_rx_advance( config, s, mode, channel );
        }
    }
}

// 1 when every owed sequence and tiny message of the sent report arrived
static inline int matrix_rx_complete( const struct matrix_rx_t * rx )
{
    if ( !rx->have_sent_report )
    {
        return 0;
    }

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int length = 1; length < ROCKETNET_LIVE_HEADER_BYTES; length++ )
        {
            if ( matrix_owed( mode, (uint32_t) length ) && rx->tiny_received[mode][length] < rx->tiny_sent[mode][length] )
            {
                return 0;
            }
        }

        for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
        {
            if ( rx->streams[mode][channel].owed < rx->streams[mode][channel].sent )
            {
                return 0;
            }
        }
    }

    return 1;
}

// At the end of a window: every owed sequence below the sender's count that
// never arrived is lost. Returns how many are missing on the stream and adds
// each to by_bucket, the size bucket of its length.
static inline uint32_t matrix_rx_missing( const rocketnet_live_config_t * config, const struct matrix_rx_stream_t * s, int mode, int channel,
                                          uint32_t * by_bucket )
{
    uint32_t missing = 0;
    for ( uint32_t q = s->owed; q < s->sent; q++ )
    {
        const uint32_t length = rocketnet_live_length( config, mode, channel, q );
        if ( matrix_owed( mode, length ) && !matrix_counted_tiny( mode, length ) && !matrix_rx_seen( s, q ) )
        {
            by_bucket[rocketnet_live_bucket( config, mode, length )]++;
            missing++;
        }
    }
    return missing;
}

// The progress report: tag 0xC0, then per stream whose owed is past 0 a
// record of mode, channel, owed - 1 and the bytes received. Returns its length.
static inline int matrix_rx_progress( const struct matrix_rx_t * rx, uint8_t * buffer, int capacity )
{
    int off = 0;
    if ( capacity < 1 )
    {
        return 0;
    }
    buffer[off++] = MATRIX_PROGRESS_NIBBLE;

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            const struct matrix_rx_stream_t * s = &rx->streams[mode][channel];
            if ( s->owed == 0 )
            {
                continue;
            }
            if ( off + MATRIX_PROGRESS_RECORD > capacity )
            {
                return off;
            }
            buffer[off++] = (uint8_t) mode;
            buffer[off++] = (uint8_t) channel;
            matrix_put_u32( buffer + off, s->owed - 1u );
            off += 4;
            matrix_put_u32( buffer + off, s->bytes > 0xffffffffu ? 0xffffffffu : (uint32_t) s->bytes );
            off += 4;
        }
    }

    return off;
}

#endif // ROCKETNET_TESTS_MATRIX_RECEIVE_H
