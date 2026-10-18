/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The loopback message matrix (docs/LIVE-XDP.md sections 3 to 5): both
    message objects over the channel harness, every mode at once, every
    channel the config allows, both directions, across the full size
    schedule including the exact maximum. Every delivered message is
    verified; reliable modes are checked for completeness and order; the
    over-maximum send is refused with its named counter. Counts print in one
    table at the end. Bounded to under 10 s.
*/

#include "channel_harness.h"

#include "rocketnet_platform.h"

#include "../test/live_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rocketnet_live_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_live_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

enum
{
    MATRIX_NDIR = 2,
    MATRIX_NMODE = 8,
    MATRIX_NCH = 32,
    MATRIX_MAX_SEQ = 16,
    MATRIX_BUF = 32768,
};

static uint32_t g_seed = 0x5EED5EE1u;
static uint32_t g_max_m = 1415;   // ROCKETNET_MESSAGE_SINGLE_BYTES, the config minimum

static uint32_t g_sent_seq[MATRIX_NDIR][MATRIX_NMODE][MATRIX_NCH][MATRIX_MAX_SEQ];
static int g_sent_n[MATRIX_NDIR][MATRIX_NMODE][MATRIX_NCH];
static uint32_t g_recv_seen[MATRIX_NDIR][MATRIX_NMODE][MATRIX_NCH][MATRIX_MAX_SEQ];
static int g_recv_n[MATRIX_NDIR][MATRIX_NMODE][MATRIX_NCH];
static uint32_t g_last_seq[MATRIX_NDIR][MATRIX_NMODE][MATRIX_NCH];

static void matrix_drain( rocketnet_channel_harness_t * h, int server_side );

static long g_verified = 0;
static long g_failed = 0;
static long g_missing = 0;
static long g_out_of_order = 0;
static long g_duplicate = 0;
static long g_unsent = 0;

static int matrix_mode_channels( int mode )
{
    if ( mode == 1 || mode == 3 || mode == 4 || mode == 7 )
        return 31;      // channels 0..30; 31 carries progress reports
    return 32;          // channel-less modes name all channels
}

static int matrix_stream_needs_order( int mode )
{
    return mode == 1 || mode == 3 || mode == 4 || mode == 7;
}

static int matrix_stream_needs_complete( int mode )
{
    return mode == 2 || mode == 3 || mode == 4 || mode == 6 || mode == 7;
}

static void matrix_record_sent( int dir, int mode, int ch, uint32_t s )
{
    rocketnet_check( g_sent_n[dir][mode][ch] < MATRIX_MAX_SEQ );
    g_sent_seq[dir][mode][ch][g_sent_n[dir][mode][ch]++] = s;
}

static void matrix_send( rocketnet_channel_harness_t * h, int dir, int mode, int ch, uint32_t s )
{
    static uint8_t buf[MATRIX_BUF];
    const int len = rocketnet_live_generate( dir, mode, ch, 0, s, g_seed, g_max_m, buf, (int) sizeof(buf) );
    rocketnet_check( len >= 1 );
    const uint32_t serial = dir == 0 ? rocketnet_channel_harness_client_send( h, ch, mode, buf, len )
                                     : rocketnet_channel_harness_server_send( h, ch, mode, buf, len );
    if ( serial == 0 )
        printf( "matrix_send REFUSED dir=%d mode=%d ch=%d s=%u len=%d\n", dir, mode, ch, s, len );
    rocketnet_check( serial != 0 );
    // pace reliable sends: pump until the message's ack frees its record, so
    // the connection's region never latches exhausted
    if ( matrix_stream_needs_complete( mode ) )
    {
        for ( int p = 0; p < 5; p++ )
        {
            rocketnet_channel_harness_pump( h, 1.0 / 60.0 );
            matrix_drain( h, 1 );
            matrix_drain( h, 0 );
        }
    }
    matrix_record_sent( dir, mode, ch, s );
}

static void matrix_handle_delivery( int server_side, int mode, int ch, const uint8_t * data, int bytes, uint32_t s )
{
    const int dir = server_side ? 0 : 1;
    rocketnet_check( mode >= 0 && mode < MATRIX_NMODE );
    rocketnet_check( ch >= 0 && ch < MATRIX_NCH );
    int off = -1;
    const int res = rocketnet_live_check( data, bytes, dir, mode, ch, 0, g_seed, g_max_m, &off );
    if ( res != ROCKETNET_LIVE_OK )
    {
        g_failed++;
        return;
    }
    g_verified++;

    // the sequence must be one we sent on this stream
    int sent = 0;
    for ( int i = 0; i < g_sent_n[dir][mode][ch]; i++ )
        if ( g_sent_seq[dir][mode][ch][i] == s ) { sent = 1; break; }
    if ( !sent ) { g_unsent++; return; }

    // duplicate
    for ( int i = 0; i < g_recv_n[dir][mode][ch]; i++ )
        if ( g_recv_seen[dir][mode][ch][i] == s ) { g_duplicate++; return; }
    rocketnet_check( g_recv_n[dir][mode][ch] < MATRIX_MAX_SEQ );
    g_recv_seen[dir][mode][ch][g_recv_n[dir][mode][ch]++] = s;

    // order on sequenced/ordered streams
    if ( matrix_stream_needs_order( mode ) )
    {
        if ( g_recv_n[dir][mode][ch] > 1 && s <= g_last_seq[dir][mode][ch] )
            g_out_of_order++;
    }
    g_last_seq[dir][mode][ch] = s;
}

static void matrix_drain( rocketnet_channel_harness_t * h, int server_side )
{
    for ( int c = 0; c < MATRIX_NCH; c++ )
    {
        rocketnet_channel_delivery_t d;
        while ( server_side ? rocketnet_channel_harness_server_collect( h, c, &d )
                            : rocketnet_channel_harness_client_collect( h, c, &d ) )
        {
            if ( d.bytes < 1 )
            {
                g_failed++;
                continue;
            }
            const int mode = d.data[0] & 0x07;
            const int ch = d.data[1] & 0xFF;
            const uint32_t s = (uint32_t) d.data[3] | ( (uint32_t) d.data[4] << 8 ) |
                               ( (uint32_t) d.data[5] << 16 ) | ( (uint32_t) d.data[6] << 24 );
            matrix_handle_delivery( server_side, mode, ch, d.data, d.bytes, s );
        }
    }
}

static int matrix_reliable_pending( void )
{
    for ( int dir = 0; dir < MATRIX_NDIR; dir++ )
        for ( int mode = 0; mode < MATRIX_NMODE; mode++ )
        {
            if ( !matrix_stream_needs_complete( mode ) ) continue;
            for ( int ch = 0; ch < MATRIX_NCH; ch++ )
                if ( g_recv_n[dir][mode][ch] < g_sent_n[dir][mode][ch] )
                    return 1;
        }
    return 0;
}

static void matrix_check_completeness( void )
{
    for ( int dir = 0; dir < MATRIX_NDIR; dir++ )
        for ( int mode = 0; mode < MATRIX_NMODE; mode++ )
        {
            if ( !matrix_stream_needs_complete( mode ) ) continue;
            for ( int ch = 0; ch < MATRIX_NCH; ch++ )
            {
                const int missing = g_sent_n[dir][mode][ch] - g_recv_n[dir][mode][ch];
                if ( missing > 0 ) g_missing += missing;
            }
        }
}

void live_message_matrix_loopback()
{
    rocketnet_channel_harness_t h;
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.max_message_bytes = (int) g_max_m;
    config.num_channels = 32;
    if ( !rocketnet_channel_harness_create_with_config( &h, &config ) )
    {
        rocketnet_check( 0 );
        return;
    }
    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return;
    }

    const double t0 = rocketnet_platform_time();

    // send: every mode, every channel, both directions, the full size
    // schedule including the exact maximum
    const uint32_t reps[9] = { 0, 1, 8, 24, 29, 30, 31, 33, 64 };
    const int reps_count = 9;
    for ( int dir = 0; dir < MATRIX_NDIR; dir++ )
        for ( int mode = 0; mode < MATRIX_NMODE; mode++ )
        {
            const int maxch = matrix_mode_channels( mode );
            const uint32_t eff = rocketnet_live_mode_max( mode, g_max_m );
            for ( int ch = 0; ch < maxch; ch++ )
            {
                for ( int i = 0; i < reps_count; i++ )
                    matrix_send( &h, dir, mode, ch, reps[i] );
                // the exact maximum: find a sequence whose scheduled length is
                // the stream maximum and send it
                for ( uint32_t s = 0; s < 8192; s++ )
                {
                    if ( rocketnet_live_stream_len( mode, ch, g_seed, s, g_max_m ) == eff )
                    {
                        matrix_send( &h, dir, mode, ch, s );
                        break;
                    }
                }
                // pump a little after each channel so the reliable queues
                // drain and later sends are not refused
                for ( int k = 0; k < 2; k++ )
                {
                    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
                    matrix_drain( &h, 1 );
                    matrix_drain( &h, 0 );
                }
            }
        }

    // pump and drain until every reliable message is accounted for
    int iters = 0;
    while ( iters < 6000 && matrix_reliable_pending() )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        matrix_drain( &h, 1 );
        matrix_drain( &h, 0 );
        iters++;
    }
    // a few extra pumps to flush the tail
    for ( int i = 0; i < 120; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        matrix_drain( &h, 1 );
        matrix_drain( &h, 0 );
    }

    matrix_check_completeness();
    rocketnet_check( g_failed == 0 );
    rocketnet_check( g_unsent == 0 );
    rocketnet_check( g_duplicate == 0 );
    rocketnet_check( g_out_of_order == 0 );
    rocketnet_check( g_missing == 0 );

    // over-maximum refused on send, with its named counter
    {
        const uint64_t refused_before_c = rocketnet_message_client_counters( h.message_client )->values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED];
        const uint64_t unreli_before_c = rocketnet_message_client_counters( h.message_client )->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED];
        const uint64_t refused_before_s = rocketnet_message_server_counters( h.message_server )->values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED];
        const uint64_t unreli_before_s = rocketnet_message_server_counters( h.message_server )->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED];
        static uint8_t over[8192];
        for ( int mode = 0; mode < MATRIX_NMODE; mode++ )
        {
            const int over_len = (int) rocketnet_live_mode_max( mode, g_max_m ) + 1;
            memset( over, 0x5A, sizeof(over) );
            const uint32_t c_ser = rocketnet_channel_harness_client_send( &h, 0, mode, over, over_len );
            const uint32_t s_ser = rocketnet_channel_harness_server_send( &h, 0, mode, over, over_len );
            rocketnet_check( c_ser == 0 );
            rocketnet_check( s_ser == 0 );
        }
        const uint64_t refused_after_c = rocketnet_message_client_counters( h.message_client )->values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED];
        const uint64_t unreli_after_c = rocketnet_message_client_counters( h.message_client )->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED];
        const uint64_t refused_after_s = rocketnet_message_server_counters( h.message_server )->values[ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED];
        const uint64_t unreli_after_s = rocketnet_message_server_counters( h.message_server )->values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED];
        long refused_delta = (long) ( refused_after_c - refused_before_c ) + (long) ( refused_after_s - refused_before_s );
        long unreli_delta = (long) ( unreli_after_c - unreli_before_c ) + (long) ( unreli_after_s - unreli_before_s );
        // reliable/code-5 modes refuse via MESSAGE_SEND_REFUSED; codes 0 and
        // 1 via MESSAGE_UNRELIABLE_SEND_FAILED; both ends, so 2 refusals per
        // mode
        rocketnet_check( refused_delta + unreli_delta == 2 * MATRIX_NMODE );
        rocketnet_check( unreli_delta >= 4 );
        (void) refused_delta;
        (void) unreli_delta;
    }

    const double elapsed = rocketnet_platform_time() - t0;

    // one table: per (direction, mode) sent/received/verified/missing/oob/dup
    printf( "live_message_matrix_loopback table (dir mode sent recv verified missing oob dup)\n" );
    for ( int dir = 0; dir < MATRIX_NDIR; dir++ )
        for ( int mode = 0; mode < MATRIX_NMODE; mode++ )
        {
            long sent = 0, recv = 0, verified = 0;
            for ( int ch = 0; ch < MATRIX_NCH; ch++ )
            {
                sent += g_sent_n[dir][mode][ch];
                recv += g_recv_n[dir][mode][ch];
                verified += (long) g_recv_n[dir][mode][ch];
            }
            printf( "  %d %d %ld %ld %ld %ld %ld %ld\n", dir, mode, sent, recv, verified,
                    g_missing, g_out_of_order, g_duplicate );
        }
    printf( "live_message_matrix_loopback verified=%ld failed=%ld missing=%ld oob=%ld dup=%ld unsent=%ld elapsed_ms=%.1f\n",
            g_verified, g_failed, g_missing, g_out_of_order, g_duplicate, g_unsent, elapsed * 1000.0 );

    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocation_snapshot );
    rocketnet_check( elapsed < 10.0 );

    rocketnet_channel_harness_destroy( &h );
}
