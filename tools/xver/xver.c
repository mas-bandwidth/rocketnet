/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The cross-version check: one end of a real connection, built against one
    release's library, run against the other end built against another
    (tools/xver/xver.go drives the pairs; .github/workflows/nightly-xver.yml
    runs it every night). It uses only functions the v1.0.0 header already
    had, so the same file builds against every 1.x tree; each build links
    that tree's own rocketnet_dev (tools/xver/CMakeLists.txt).

        xver server <a.b.c.d:port> <max_clients>
        xver client <a.b.c.d:port> <server public key hex> <case> <count>

    Each session sends, in both directions, one stream per reliability mode
    (mode m on channel m), `count` messages each, with lengths from a
    schedule that reaches the configured max_message_bytes on modes 0, 2, 3,
    6 and 7 (mode 0's is promoted to its reliable twin). Control messages go
    reliable-ordered on channel 31. Every message carries mode, channel,
    direction, session, sequence and length, written byte by byte, and a body
    that is a pattern of them.

    The receiver checks every message: header and body intact; the
    reliability the batch reports is the one the standard says (receipt
    flag stripped; an unreliable message above its one-record limit
    reported as its reliable twin); the batch channel is the stream's for
    modes 1, 3, 4 and 7 and zero otherwise; no sequence twice in any mode;
    strictly in order for modes 3 and 7; strictly increasing for modes 1
    and 4. At the end of a session, against the sender's FIN totals: every
    message of modes 2, 3, 6 and 7 delivered, and the last of mode 4. Each
    end, as sender, checks one ACKED receipt per send on modes 6 and 7 and
    one terminal receipt per send on mode 5: the client before it ends the
    session, the server in its verdict ("SRV verify ... receipts=complete",
    which the driver requires of every verified session).

    Cases (the client runs the case; the server reacts to control messages):

        basic        one session, clean client disconnect
        lifecycle    A clean client disconnect; B reconnect, the server
                     disconnects the client; C reconnect, client-to-server
                     silence past T; D reconnect, server-to-client silence
                     past T; E reconnect, clean disconnect. The server runs
                     max_clients 1, so every reconnect takes the freed slot.
        die          connect and run traffic until the driver kills the
                     process; the driver then runs basic into the same slot
        weather      both ends' development link simulators at 40 ms
                     latency, 30 ms jitter, 10% loss and 5% duplicates
        outage-c2s   100% loss for T - 1 seconds, client to server, while the
                     streams are half sent; the connection must survive
        outage-s2c   the same, server to client
        outage-both  the same, both directions at once

    T is the connect token's timeout, 10 s. Every result is one line; a line
    holding " FAIL " is a failure. The server prints "SRV end slot=... reason=
    NAME(code)" for every ended connection and the driver grades the names.

    A functional check over real sockets and the wall clock, not a unit test.
*/

#if !defined( ROCKETNET_DEV )
#define ROCKETNET_DEV 1
#endif // #if !defined( ROCKETNET_DEV )

#include "rocketnet.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XV_PROTOCOL_ID 0x5856455253494f4eULL
#define XV_MODES 8
#define XV_CTRL_CHANNEL 31
#define XV_MAXSEQ 8192
#define XV_HEADER 16
#define XV_MAGIC 0x5A
#define XV_CTRL 0xFF
#define XV_MAX_CLIENTS 4
#define XV_TOKEN_TIMEOUT 10
#define XV_KBPS 20000
#define XV_RECEIPTS 4096
#define XV_UNRELIABLE_LIMIT 1411 // UNRELIABLE and UNRELIABLE_WITH_ACK_RECEIPT one-record limit
#define XV_SEQUENCED_LIMIT 1405  // UNRELIABLE_SEQUENCED one-record limit, channels below 256

enum xv_control_t
{
    CT_HELLO = 1,
    CT_FIN = 2,
    CT_VERDICT = 3,
    CT_KICK = 4,
    CT_OUTAGE = 5,
    CT_WEATHER = 6,
};

static volatile sig_atomic_t g_quit;
static int g_max_message_bytes;
static uint64_t g_fails;

static void on_signal( int signal_number )
{
    (void) signal_number;
    g_quit = 1;
}

static void write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

static uint8_t body_byte( uint32_t i, int mode, int dir, uint32_t session, uint32_t seq )
{
    return (uint8_t) ( i * 131u + seq * 7u + (uint32_t) mode * 29u + (uint32_t) dir * 3u + session + ( i >> 8 ) * 17u );
}

static int stream_length( int mode, uint32_t seq )
{
    static const int schedule[10] = { 16, 17, 100, 500, 1100, 1400, 1411, 3000, 9000, 20000 };
    int length = schedule[seq % 10];
    if ( seq % 200 == 150 && ( mode == 0 || mode == 2 || mode == 3 || mode == 6 || mode == 7 ) )
    {
        return g_max_message_bytes;
    }
    if ( ( mode == 0 || mode == 1 || mode == 5 ) && length > XV_UNRELIABLE_LIMIT )
    {
        length = XV_UNRELIABLE_LIMIT;
    }
    if ( mode == 1 && length > XV_SEQUENCED_LIMIT )
    {
        length = XV_SEQUENCED_LIMIT;
    }
    return length;
}

// the reliability the receiver's batch reports for a send of this mode and length
static int expected_received_reliability( int mode, int length )
{
    int base = mode;
    if ( mode == 5 )
    {
        base = 0;
    }
    else if ( mode == 6 )
    {
        base = 2;
    }
    else if ( mode == 7 )
    {
        base = 3;
    }
    if ( base == 0 && length > XV_UNRELIABLE_LIMIT )
    {
        base = 2;
    }
    if ( base == 1 && length > XV_SEQUENCED_LIMIT )
    {
        base = 4;
    }
    return base;
}

static int channel_reported( int reliability )
{
    return reliability == 1 || reliability == 3 || reliability == 4;
}

static void fill( uint8_t * p, int length, int mode, int dir, uint32_t session, uint32_t seq )
{
    p[0] = XV_MAGIC;
    p[1] = (uint8_t) mode;
    p[2] = (uint8_t) mode;
    p[3] = (uint8_t) dir;
    write_u32( p + 4, session );
    write_u32( p + 8, seq );
    write_u32( p + 12, (uint32_t) length );
    for ( int i = XV_HEADER; i < length; i++ )
    {
        p[i] = body_byte( (uint32_t) i, mode, dir, session, seq );
    }
}

static void fail_line( void )
{
    g_fails++;
}

// ---- receive-side stream state ----

struct xv_rx_t
{
    uint8_t seen[XV_MODES][XV_MAXSEQ];
    uint32_t received[XV_MODES];
    uint32_t expected[XV_MODES];
    int64_t last[XV_MODES];
    uint32_t bad;
};

static void rx_reset( struct xv_rx_t * r )
{
    memset( r, 0, sizeof( *r ) );
    for ( int m = 0; m < XV_MODES; m++ )
    {
        r->last[m] = -1;
    }
}

static void rx_check( const char * who, struct xv_rx_t * r, const uint8_t * p, int bytes, int batch_reliability, int batch_channel, int want_dir, uint32_t session )
{
    if ( bytes < XV_HEADER || p[0] != XV_MAGIC )
    {
        printf( "%s FAIL bad-magic bytes=%d\n", who, bytes );
        r->bad++;
        fail_line();
        return;
    }
    const int mode = p[1];
    const int dir = p[3];
    const uint32_t message_session = read_u32( p + 4 );
    const uint32_t seq = read_u32( p + 8 );
    const uint32_t length = read_u32( p + 12 );
    if ( mode >= XV_MODES || p[2] != mode || dir != want_dir || message_session != session || length != (uint32_t) bytes || seq >= XV_MAXSEQ )
    {
        printf( "%s FAIL bad-header mode=%d dir=%d session=%x/%x seq=%u length=%u bytes=%d\n", who, mode, dir, message_session, session, seq, length, bytes );
        r->bad++;
        fail_line();
        return;
    }
    for ( int i = XV_HEADER; i < bytes; i++ )
    {
        if ( p[i] != body_byte( (uint32_t) i, mode, dir, message_session, seq ) )
        {
            printf( "%s FAIL bad-body mode=%d seq=%u offset=%d bytes=%d\n", who, mode, seq, i, bytes );
            r->bad++;
            fail_line();
            return;
        }
    }
    const int want_reliability = expected_received_reliability( mode, bytes );
    if ( batch_reliability != want_reliability )
    {
        printf( "%s FAIL reliability mode=%d seq=%u bytes=%d reported=%d want=%d\n", who, mode, seq, bytes, batch_reliability, want_reliability );
        fail_line();
    }
    const int want_channel = channel_reported( want_reliability ) ? mode : 0;
    if ( batch_channel != want_channel )
    {
        printf( "%s FAIL channel mode=%d seq=%u reported=%d want=%d\n", who, mode, seq, batch_channel, want_channel );
        fail_line();
    }
    if ( r->seen[mode][seq] )
    {
        printf( "%s FAIL duplicate mode=%d seq=%u\n", who, mode, seq );
        fail_line();
        return;
    }
    r->seen[mode][seq] = 1;
    r->received[mode]++;
    if ( mode == 3 || mode == 7 )
    {
        if ( seq != r->expected[mode] )
        {
            printf( "%s FAIL out-of-order mode=%d seq=%u expected=%u\n", who, mode, seq, r->expected[mode] );
            fail_line();
        }
        r->expected[mode] = seq + 1;
    }
    if ( mode == 1 || mode == 4 )
    {
        if ( (int64_t) seq <= r->last[mode] )
        {
            printf( "%s FAIL not-increasing mode=%d seq=%u last=%lld\n", who, mode, seq, (long long) r->last[mode] );
            fail_line();
        }
        r->last[mode] = seq;
    }
}

// 1 when everything the sender's totals promise has arrived
static int rx_complete( const struct xv_rx_t * r, const uint32_t * totals )
{
    for ( int m = 0; m < XV_MODES; m++ )
    {
        if ( ( m == 2 || m == 3 || m == 6 || m == 7 ) && r->received[m] != totals[m] )
        {
            return 0;
        }
        if ( m == 4 && totals[m] > 0 && r->last[m] != (int64_t) totals[m] - 1 )
        {
            return 0;
        }
    }
    return 1;
}

static void rx_summary( char * out, size_t capacity, const struct xv_rx_t * r, const uint32_t * totals )
{
    size_t used = 0;
    out[0] = '\0';
    for ( int m = 0; m < XV_MODES && used < capacity; m++ )
    {
        const int n = snprintf( out + used, capacity - used, "m%d=%u/%u%s ", m, r->received[m], totals[m], m == 4 ? "(last)" : "" );
        if ( n < 0 )
        {
            break;
        }
        used += (size_t) n;
    }
}

// ---- send-side stream state ----

struct xv_tx_t
{
    uint32_t next[XV_MODES];
    uint32_t count;
    uint8_t serial_mode[65536]; // receipt serial -> mode + 1
    uint32_t acked[XV_MODES];
    uint32_t lost[XV_MODES];
    uint32_t sent_with_receipt[XV_MODES];
    uint64_t refused;
};

static void tx_reset( struct xv_tx_t * t, uint32_t count )
{
    memset( t, 0, sizeof( *t ) );
    t->count = count;
}

static int tx_done( const struct xv_tx_t * t )
{
    for ( int m = 0; m < XV_MODES; m++ )
    {
        if ( t->next[m] < t->count )
        {
            return 0;
        }
    }
    return 1;
}

static int tx_receipts_done( const struct xv_tx_t * t )
{
    return t->acked[6] == t->sent_with_receipt[6] && t->acked[7] == t->sent_with_receipt[7] && t->acked[5] + t->lost[5] == t->sent_with_receipt[5];
}

static void tx_receipt( const char * who, struct xv_tx_t * t, uint32_t serial, int status )
{
    const int mode = t->serial_mode[serial & 0xffff] - 1;
    if ( mode < 5 )
    {
        printf( "%s FAIL receipt-unknown-serial serial=%u status=%d\n", who, serial, status );
        fail_line();
        return;
    }
    t->serial_mode[serial & 0xffff] = 0;
    if ( status == ROCKETNET_RECEIPT_STATUS_ACKED )
    {
        t->acked[mode]++;
    }
    else if ( status == ROCKETNET_RECEIPT_STATUS_LOSS )
    {
        if ( mode != 5 )
        {
            printf( "%s FAIL receipt-loss-on-reliable mode=%d serial=%u\n", who, mode, serial );
            fail_line();
        }
        t->lost[mode]++;
    }
}

static const char * reason_name( int reason )
{
    return rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) reason );
}

static const char * state_name( int state )
{
    return rocketnet_client_state_name( (enum rocketnet_client_state_t) state );
}

// ============================================================ server

struct xv_slot_t
{
    int connected;
    uint64_t client_id;
    uint32_t session;
    int hello;
    struct xv_rx_t rx;
    struct xv_tx_t tx;
    int remote_fin;
    uint32_t remote_totals[XV_MODES];
    int fin_sent;
    int verdict_sent;
    double fin_at;
};

static struct xv_slot_t g_slots[XV_MAX_CLIENTS];
static uint8_t g_buffer[( 1 << 20 ) + 64];

static int server_send( struct rocketnet_server_t * server, int index, int channel, int reliability, const uint8_t * data, int length, uint32_t * serial )
{
    // a v1.0.x server disconnects the client on a start that finds no room, so ask first
    if ( !rocketnet_server_can_send_message( server, index, channel, (enum rocketnet_reliability_t) reliability, length ) )
    {
        return 0;
    }
    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, channel, (enum rocketnet_reliability_t) reliability, length );
    if ( !p )
    {
        return 0;
    }
    memcpy( p, data, (size_t) length );
    const uint32_t s = rocketnet_server_finish_message( server, length );
    if ( serial )
    {
        *serial = s;
    }
    return 1;
}

static int server_control( struct rocketnet_server_t * server, int index, int type, uint32_t session, const uint32_t * words, int num_words )
{
    uint8_t m[8 + 4 * 16];
    m[0] = XV_MAGIC;
    m[1] = XV_CTRL;
    m[2] = (uint8_t) type;
    m[3] = 1;
    write_u32( m + 4, session );
    for ( int i = 0; i < num_words; i++ )
    {
        write_u32( m + 8 + 4 * i, words[i] );
    }
    return server_send( server, index, XV_CTRL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, m, 8 + 4 * num_words, NULL );
}

static void server_control_message( struct rocketnet_server_t * server, int index, struct xv_slot_t * s, const uint8_t * p, int bytes, double now, double * outage_until, int * weather )
{
    const int type = p[2];
    if ( type == CT_HELLO && bytes >= 12 )
    {
        s->hello = 1;
        tx_reset( &s->tx, read_u32( p + 8 ) );
        printf( "SRV hello slot=%d session=%x count=%u\n", index, s->session, s->tx.count );
    }
    else if ( type == CT_FIN && bytes >= 8 + 4 * XV_MODES )
    {
        s->remote_fin = 1;
        s->fin_at = now;
        for ( int m = 0; m < XV_MODES; m++ )
        {
            s->remote_totals[m] = read_u32( p + 8 + 4 * m );
        }
    }
    else if ( type == CT_KICK )
    {
        printf( "SRV kick slot=%d session=%x\n", index, s->session );
        rocketnet_server_disconnect_client( server, index );
    }
    else if ( type == CT_OUTAGE && bytes >= 12 )
    {
        const uint32_t ms = read_u32( p + 8 );
        rocketnet_server_network_simulator( server, 0, 0, 100, 0 );
        *outage_until = now + ms / 1000.0;
        printf( "SRV outage-start ms=%u t=%.3f\n", ms, now );
    }
    else if ( type == CT_WEATHER && bytes >= 24 )
    {
        for ( int w = 0; w < 4; w++ )
        {
            weather[w] = (int) read_u32( p + 8 + 4 * w );
        }
        rocketnet_server_network_simulator( server, weather[0], weather[1], weather[2], weather[3] );
        printf( "SRV weather %d %d %d %d\n", weather[0], weather[1], weather[2], weather[3] );
    }
    fflush( stdout );
}

static void server_streams( struct rocketnet_server_t * server, int index, struct xv_slot_t * s, double now )
{
    for ( int m = 0; m < XV_MODES; m++ )
    {
        if ( s->tx.next[m] >= s->tx.count )
        {
            continue;
        }
        const uint32_t seq = s->tx.next[m];
        const int length = stream_length( m, seq );
        fill( g_buffer, length, m, 1, s->session, seq );
        uint32_t serial = 0;
        if ( server_send( server, index, m, m, g_buffer, length, &serial ) )
        {
            if ( m >= 5 )
            {
                s->tx.serial_mode[serial & 0xffff] = (uint8_t) ( m + 1 );
                s->tx.sent_with_receipt[m]++;
            }
            s->tx.next[m]++;
        }
        else
        {
            s->tx.refused++;
        }
    }
    if ( tx_done( &s->tx ) && !s->fin_sent && server_control( server, index, CT_FIN, s->session, s->tx.next, XV_MODES ) )
    {
        s->fin_sent = 1;
    }
    if ( !s->remote_fin || s->verdict_sent )
    {
        return;
    }
    const int complete = rx_complete( &s->rx, s->remote_totals );
    // the server's own sends are graded as the client's are: every stream sent
    // (its FIN out) and one receipt per send on modes 5, 6 and 7
    const int receipts = s->fin_sent && tx_receipts_done( &s->tx );
    if ( ( !complete || !receipts ) && now - s->fin_at <= 120.0 )
    {
        return;
    }
    char summary[512];
    rx_summary( summary, sizeof( summary ), &s->rx, s->remote_totals );
    const uint32_t ok = ( complete && receipts && s->rx.bad == 0 ) ? 1u : 0u;
    if ( !ok )
    {
        printf( "SRV FAIL verify session=%x receipts=%s fin_sent=%d m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s\n", s->session, receipts ? "complete" : "open", s->fin_sent, s->tx.acked[5],
                s->tx.lost[5], s->tx.sent_with_receipt[5], s->tx.acked[6], s->tx.sent_with_receipt[6], s->tx.acked[7], s->tx.sent_with_receipt[7], summary );
        fail_line();
    }
    else
    {
        struct rocketnet_server_client_stats_t stats;
        memset( &stats, 0, sizeof( stats ) );
        rocketnet_server_client_stats( server, index, &stats );
        printf( "SRV verify session=%x ok receipts=complete m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s stats sent=%llu received=%llu rtt=%.2f loss=%.3f\n", s->session, s->tx.acked[5],
                s->tx.lost[5], s->tx.sent_with_receipt[5], s->tx.acked[6], s->tx.sent_with_receipt[6], s->tx.acked[7], s->tx.sent_with_receipt[7], summary,
                (unsigned long long) stats.packets_sent, (unsigned long long) stats.packets_received, stats.rtt_ms, stats.packet_loss_percent );
    }
    uint32_t words[1 + XV_MODES];
    words[0] = ok;
    for ( int m = 0; m < XV_MODES; m++ )
    {
        words[1 + m] = s->rx.received[m];
    }
    if ( server_control( server, index, CT_VERDICT, s->session, words, 1 + XV_MODES ) )
    {
        s->verdict_sent = 1;
    }
    fflush( stdout );
}

static int run_server( const char * address, int max_clients )
{
    if ( max_clients < 1 || max_clients > XV_MAX_CLIENTS )
    {
        printf( "SRV FAIL max_clients %d is outside 1 .. %d\n", max_clients, XV_MAX_CLIENTS );
        return 2;
    }
    struct rocketnet_server_config_t config;
    static const uint8_t backend_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    rocketnet_server_default_config( &config );
    config.protocol_id = XV_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_key, sizeof( backend_key ) );
    config.max_clients = max_clients;
    config.per_client_send_kbps = XV_KBPS;
    g_max_message_bytes = config.max_message_bytes;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, address, NULL, &config, &status );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    if ( !server )
    {
        printf( "SRV FAIL create %s\n", rocketnet_status_name( status ) );
        return 1;
    }
    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, key );
    printf( "SRV key " );
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        printf( "%02x", key[i] );
    }
    printf( "\nSRV started %s transport=%s max_message_bytes=%d\n", address, rocketnet_server_transport_name( server ), g_max_message_bytes );
    fflush( stdout );

    double outage_until = 0.0;
    int weather[4] = { 0, 0, 0, 0 };
    static struct rocketnet_receipt_t receipts[XV_RECEIPTS];

    while ( !g_quit )
    {
        const double now = rocketnet_time();
        rocketnet_server_update( server, now );

        if ( outage_until > 0.0 && now >= outage_until )
        {
            rocketnet_server_network_simulator( server, weather[0], weather[1], weather[2], weather[3] );
            printf( "SRV outage-end t=%.3f\n", now );
            fflush( stdout );
            outage_until = 0.0;
        }

        for ( int i = 0; i < max_clients; i++ )
        {
            struct xv_slot_t * s = &g_slots[i];
            const int connected = rocketnet_server_client_connected( server, i ) ? 1 : 0;
            if ( connected && !s->connected )
            {
                memset( s, 0, sizeof( *s ) );
                s->connected = 1;
                s->client_id = rocketnet_server_client_id( server, i );
                s->session = (uint32_t) s->client_id;
                rx_reset( &s->rx );
                printf( "SRV connect slot=%d client_id=%llx t=%.3f\n", i, (unsigned long long) s->client_id, now );
                fflush( stdout );
            }
            else if ( !connected && s->connected )
            {
                const int reason = (int) rocketnet_server_disconnect_reason( server, i );
                printf( "SRV end slot=%d session=%x reason=%s(%d) verdict_sent=%d t=%.3f\n", i, s->session, reason_name( reason ), reason, s->verdict_sent, now );
                fflush( stdout );
                s->connected = 0;
            }
            if ( connected )
            {
                const int n = rocketnet_server_receipts( server, i, receipts, XV_RECEIPTS );
                for ( int k = 0; k < n && k < XV_RECEIPTS; k++ )
                {
                    tx_receipt( "SRV", &s->tx, receipts[k].serial, (int) receipts[k].status );
                }
            }
        }

        // drain the whole receive queue before the next update (MESSAGE_NOT_RECEIVED otherwise)
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int k = 0; k < batch->num_messages; k++ )
            {
                const int i = batch->client_index[k];
                const uint8_t * p = batch->data[k];
                const int bytes = batch->bytes[k];
                if ( i < 0 || i >= max_clients )
                {
                    printf( "SRV FAIL client-index %d\n", i );
                    fail_line();
                    continue;
                }
                if ( bytes >= 8 && p[0] == XV_MAGIC && p[1] == XV_CTRL )
                {
                    server_control_message( server, i, &g_slots[i], p, bytes, now, &outage_until, weather );
                    continue;
                }
                rx_check( "SRV", &g_slots[i].rx, p, bytes, (int) batch->reliability[k], batch->channel[k], 0, g_slots[i].session );
            }
        }

        for ( int i = 0; i < max_clients; i++ )
        {
            if ( g_slots[i].connected && g_slots[i].hello )
            {
                server_streams( server, i, &g_slots[i], now );
            }
        }

        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.002 );
    }

    for ( int i = 0; i < max_clients; i++ )
    {
        const struct xv_slot_t * s = &g_slots[i];
        if ( s->connected && s->hello && !tx_receipts_done( &s->tx ) )
        {
            printf( "SRV note receipts-open slot=%d m5=%u+%u/%u m6=%u/%u m7=%u/%u\n", i, s->tx.acked[5], s->tx.lost[5], s->tx.sent_with_receipt[5], s->tx.acked[6],
                    s->tx.sent_with_receipt[6], s->tx.acked[7], s->tx.sent_with_receipt[7] );
        }
    }
    rocketnet_server_stop( server );
    for ( int k = 0; k < 2000 && !rocketnet_server_stopped( server ); k++ )
    {
        rocketnet_server_update( server, rocketnet_time() );
        rocketnet_sleep( 0.005 );
    }
    rocketnet_server_destroy( server );
    printf( "SRV exit fails=%llu\n", (unsigned long long) g_fails );
    fflush( stdout );
    return g_fails ? 1 : 0;
}

// ============================================================ client

struct xv_client_t
{
    struct rocketnet_client_t * client;
    uint32_t session;
    struct xv_rx_t rx;
    struct xv_tx_t tx;
    int remote_fin;
    uint32_t remote_totals[XV_MODES];
    int verdict;
    uint32_t verdict_ok;
    int fin_sent;
    int last_state;
};

static struct xv_client_t g_client;
static uint8_t g_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
static const char * g_address;
static uint32_t g_session_counter;

static int client_send( struct xv_client_t * c, int channel, int reliability, const uint8_t * data, int length, uint32_t * serial )
{
    // a v1.0.x client disconnects on a start that finds no room, so ask first
    if ( !rocketnet_client_can_send_message( c->client, channel, (enum rocketnet_reliability_t) reliability, length ) )
    {
        return 0;
    }
    uint8_t * p = rocketnet_client_start_message( c->client, channel, (enum rocketnet_reliability_t) reliability, length );
    if ( !p )
    {
        return 0;
    }
    memcpy( p, data, (size_t) length );
    const uint32_t s = rocketnet_client_finish_message( c->client, length );
    if ( serial )
    {
        *serial = s;
    }
    return 1;
}

static int client_control( struct xv_client_t * c, int type, const uint32_t * words, int num_words )
{
    uint8_t m[8 + 4 * 16];
    m[0] = XV_MAGIC;
    m[1] = XV_CTRL;
    m[2] = (uint8_t) type;
    m[3] = 0;
    write_u32( m + 4, c->session );
    for ( int i = 0; i < num_words; i++ )
    {
        write_u32( m + 8 + 4 * i, words[i] );
    }
    return client_send( c, XV_CTRL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, m, 8 + 4 * num_words, NULL );
}

static void client_streams( struct xv_client_t * c )
{
    for ( int m = 0; m < XV_MODES; m++ )
    {
        if ( c->tx.next[m] >= c->tx.count )
        {
            continue;
        }
        const uint32_t seq = c->tx.next[m];
        const int length = stream_length( m, seq );
        fill( g_buffer, length, m, 0, c->session, seq );
        uint32_t serial = 0;
        if ( client_send( c, m, m, g_buffer, length, &serial ) )
        {
            if ( m >= 5 )
            {
                c->tx.serial_mode[serial & 0xffff] = (uint8_t) ( m + 1 );
                c->tx.sent_with_receipt[m]++;
            }
            c->tx.next[m]++;
        }
        else
        {
            c->tx.refused++;
        }
    }
    if ( tx_done( &c->tx ) && !c->fin_sent && client_control( c, CT_FIN, c->tx.next, XV_MODES ) )
    {
        c->fin_sent = 1;
    }
}

// one frame: update, state, receipts, receive, (optionally) the streams, send. Returns the state after update.
static int client_frame( struct xv_client_t * c, int send_streams )
{
    rocketnet_client_update( c->client, rocketnet_time() );
    const int state = (int) rocketnet_client_state( c->client );
    if ( state != c->last_state )
    {
        printf( "CLI state %s(%d) t=%.3f\n", state_name( state ), state, rocketnet_time() );
        fflush( stdout );
        c->last_state = state;
    }
    static struct rocketnet_receipt_t receipts[XV_RECEIPTS];
    const int n = rocketnet_client_receipts( c->client, receipts, XV_RECEIPTS );
    for ( int k = 0; k < n && k < XV_RECEIPTS; k++ )
    {
        tx_receipt( "CLI", &c->tx, receipts[k].serial, (int) receipts[k].status );
    }
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( c->client ) ) != NULL )
    {
        for ( int k = 0; k < batch->num_messages; k++ )
        {
            const uint8_t * p = batch->data[k];
            const int bytes = batch->bytes[k];
            if ( bytes >= 8 && p[0] == XV_MAGIC && p[1] == XV_CTRL )
            {
                if ( p[2] == CT_FIN && bytes >= 8 + 4 * XV_MODES )
                {
                    c->remote_fin = 1;
                    for ( int m = 0; m < XV_MODES; m++ )
                    {
                        c->remote_totals[m] = read_u32( p + 8 + 4 * m );
                    }
                }
                else if ( p[2] == CT_VERDICT && bytes >= 12 )
                {
                    c->verdict = 1;
                    c->verdict_ok = read_u32( p + 8 );
                }
                continue;
            }
            rx_check( "CLI", &c->rx, p, bytes, (int) batch->reliability[k], batch->channel[k], 1, c->session );
        }
    }
    if ( state == ROCKETNET_CLIENT_STATE_CONNECTED && send_streams )
    {
        client_streams( c );
    }
    rocketnet_client_send_packets( c->client );
    return state;
}

static void client_destroy( struct xv_client_t * c )
{
    if ( c->client )
    {
        rocketnet_client_destroy( c->client );
        c->client = NULL;
    }
}

// send one control message, pumping frames until the send side takes it
static void client_control_now( struct xv_client_t * c, int type, const uint32_t * words, int num_words, int send_streams )
{
    while ( !client_control( c, type, words, num_words ) && !g_quit )
    {
        if ( client_frame( c, send_streams ) < 0 )
        {
            return;
        }
        rocketnet_sleep( 0.002 );
    }
}

// connect, retrying denials and timeouts until the deadline; 1 when connected
static int client_connect( struct xv_client_t * c, uint32_t count, double deadline_seconds )
{
    const double t0 = rocketnet_time();
    int attempts = 0;
    while ( rocketnet_time() - t0 < deadline_seconds && !g_quit )
    {
        attempts++;
        client_destroy( c );
        memset( c, 0, sizeof( *c ) );
        g_session_counter++;
        const uint64_t client_id = ( (uint64_t) getpid() << 20 ) ^ ( (uint64_t) g_session_counter << 8 ) ^ (uint64_t) attempts;
        c->session = (uint32_t) client_id;
        rx_reset( &c->rx );
        tx_reset( &c->tx, count );
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        if ( rocketnet_create_dev_connect_token( g_address, NULL, g_key, 300, XV_TOKEN_TIMEOUT, client_id, XV_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, token ) != ROCKETNET_STATUS_OK )
        {
            printf( "CLI FAIL token\n" );
            fail_line();
            return 0;
        }
        struct rocketnet_client_config_t config;
        rocketnet_client_default_config( &config );
        config.send_kbps = XV_KBPS;
        g_max_message_bytes = config.max_message_bytes;
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        c->client = rocketnet_client_create( NULL, &config, &status );
        if ( !c->client || rocketnet_client_connect( c->client, token ) != ROCKETNET_STATUS_OK )
        {
            printf( "CLI FAIL create %s\n", rocketnet_status_name( status ) );
            fail_line();
            return 0;
        }
        while ( !g_quit )
        {
            const int state = client_frame( c, 0 );
            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                printf( "CLI connected session=%x attempts=%d after=%.3fs transport=%s\n", c->session, attempts, rocketnet_time() - t0, rocketnet_client_transport_name( c->client ) );
                fflush( stdout );
                const uint32_t hello[1] = { count };
                if ( !client_control( c, CT_HELLO, hello, 1 ) )
                {
                    printf( "CLI FAIL hello-refused\n" );
                    fail_line();
                }
                return 1;
            }
            if ( state < 0 )
            {
                printf( "CLI connect-attempt %d ended %s(%d)\n", attempts, state_name( state ), state );
                fflush( stdout );
                break;
            }
            rocketnet_sleep( 0.002 );
        }
        rocketnet_sleep( 0.25 );
    }
    printf( "CLI FAIL connect deadline attempts=%d\n", attempts );
    fail_line();
    return 0;
}

// pump frames for seconds, sending the streams; 0 when the connection ended
static int client_pump( struct xv_client_t * c, double seconds )
{
    const double t0 = rocketnet_time();
    while ( rocketnet_time() - t0 < seconds && !g_quit )
    {
        if ( client_frame( c, 1 ) < 0 )
        {
            return 0;
        }
        rocketnet_sleep( 0.002 );
    }
    return 1;
}

// run the session's traffic to the end and verify both directions
static int client_traffic_verify( struct xv_client_t * c, const char * label, double deadline )
{
    const double t0 = rocketnet_time();
    char summary[512];
    while ( !g_quit )
    {
        const int state = client_frame( c, 1 );
        if ( state < 0 )
        {
            printf( "CLI FAIL %s connection-ended %s(%d) during traffic\n", label, state_name( state ), state );
            fail_line();
            return 0;
        }
        if ( c->fin_sent && c->remote_fin && c->verdict && rx_complete( &c->rx, c->remote_totals ) && tx_receipts_done( &c->tx ) )
        {
            break;
        }
        if ( rocketnet_time() - t0 > deadline )
        {
            rx_summary( summary, sizeof( summary ), &c->rx, c->remote_totals );
            printf( "CLI FAIL %s verify-deadline fin_sent=%d remote_fin=%d verdict=%d receipts m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s\n", label, c->fin_sent, c->remote_fin, c->verdict,
                    c->tx.acked[5], c->tx.lost[5], c->tx.sent_with_receipt[5], c->tx.acked[6], c->tx.sent_with_receipt[6], c->tx.acked[7], c->tx.sent_with_receipt[7], summary );
            fail_line();
            return 0;
        }
        rocketnet_sleep( 0.002 );
    }
    rx_summary( summary, sizeof( summary ), &c->rx, c->remote_totals );
    if ( !c->verdict_ok || c->rx.bad )
    {
        printf( "CLI FAIL %s verify server_ok=%u client_bad=%u rx %s\n", label, c->verdict_ok, c->rx.bad, summary );
        fail_line();
        return 0;
    }
    struct rocketnet_client_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_stats( c->client, &stats );
    printf( "CLI verify %s ok in %.2fs refused_starts=%llu receipts m5=%u+%u/%u m6=%u/%u m7=%u/%u rx %s stats sent=%llu received=%llu resent=%llu rtt=%.2f loss=%.3f\n", label,
            rocketnet_time() - t0, (unsigned long long) c->tx.refused, c->tx.acked[5], c->tx.lost[5], c->tx.sent_with_receipt[5], c->tx.acked[6], c->tx.sent_with_receipt[6], c->tx.acked[7],
            c->tx.sent_with_receipt[7], summary, (unsigned long long) stats.packets_sent, (unsigned long long) stats.packets_received, (unsigned long long) stats.messages_resent,
            stats.rtt_ms, stats.packet_loss_percent );
    fflush( stdout );
    return 1;
}

// wait for the connection to end; the final state, or 1 when still up at the deadline
static int client_wait_end( struct xv_client_t * c, double seconds )
{
    const double t0 = rocketnet_time();
    while ( rocketnet_time() - t0 < seconds && !g_quit )
    {
        const int state = client_frame( c, 0 );
        if ( state <= 0 )
        {
            return state;
        }
        rocketnet_sleep( 0.002 );
    }
    return 1;
}

static void client_clean_disconnect( struct xv_client_t * c, const char * label )
{
    rocketnet_client_disconnect( c->client );
    const double t0 = rocketnet_time();
    while ( !rocketnet_client_disconnected( c->client ) && rocketnet_time() - t0 < 5.0 )
    {
        rocketnet_client_update( c->client, rocketnet_time() );
        rocketnet_sleep( 0.002 );
    }
    const int state = (int) rocketnet_client_state( c->client );
    const int disconnected = rocketnet_client_disconnected( c->client ) ? 1 : 0;
    if ( state != ROCKETNET_CLIENT_STATE_DISCONNECTED || !disconnected )
    {
        printf( "CLI FAIL %s clean-disconnect state=%s(%d) disconnected=%d\n", label, state_name( state ), state, disconnected );
        fail_line();
    }
    else
    {
        printf( "CLI end %s clean-disconnect state=%s(%d)\n", label, state_name( state ), state );
    }
    fflush( stdout );
    client_destroy( c );
}

static void expect_state( const char * label, int state, int want, int also )
{
    if ( state != want && state != also )
    {
        printf( "CLI FAIL %s end-state %s(%d) want %s(%d)\n", label, state_name( state ), state, state_name( want ), want );
        fail_line();
    }
    else
    {
        printf( "CLI end %s state=%s(%d)\n", label, state_name( state ), state );
    }
    fflush( stdout );
}

static void set_weather( struct xv_client_t * c, int latency, int jitter, int loss, int duplicates )
{
    rocketnet_client_network_simulator( c->client, latency, jitter, loss, duplicates );
    const uint32_t w[4] = { (uint32_t) latency, (uint32_t) jitter, (uint32_t) loss, (uint32_t) duplicates };
    client_control_now( c, CT_WEATHER, w, 4, 0 );
}

// an outage of ms while traffic runs: dir 0 client to server, 1 server to client, 2 both
static int outage_during_traffic( struct xv_client_t * c, const char * label, int dir, uint32_t ms )
{
    if ( !client_pump( c, 0.3 ) )
    {
        printf( "CLI FAIL %s ended before the outage\n", label );
        fail_line();
        return 0;
    }
    if ( dir == 1 || dir == 2 )
    {
        const uint32_t w[1] = { ms };
        client_control_now( c, CT_OUTAGE, w, 1, 1 );
        client_pump( c, 0.1 );
    }
    if ( dir == 0 || dir == 2 )
    {
        rocketnet_client_network_simulator( c->client, 0, 0, 100, 0 );
    }
    printf( "CLI outage-start %s dir=%d ms=%u t=%.3f sent=%u/%u/%u/%u\n", label, dir, ms, rocketnet_time(), c->tx.next[2], c->tx.next[3], c->tx.next[6], c->tx.next[7] );
    fflush( stdout );
    const int alive = client_pump( c, ms / 1000.0 );
    rocketnet_client_network_simulator( c->client, 0, 0, 0, 0 );
    printf( "CLI outage-end %s alive=%d t=%.3f\n", label, alive, rocketnet_time() );
    fflush( stdout );
    if ( !alive )
    {
        const int state = (int) rocketnet_client_state( c->client );
        printf( "CLI FAIL %s connection-ended-in-outage %s(%d)\n", label, state_name( state ), state );
        fail_line();
        return 0;
    }
    return 1;
}

static void case_lifecycle( struct xv_client_t * c, uint32_t count )
{
    const uint32_t past_timeout_ms = ( XV_TOKEN_TIMEOUT + 3 ) * 1000;

    // A: traffic, then the client disconnects
    if ( client_connect( c, count, 30.0 ) && client_traffic_verify( c, "A", 120.0 ) )
    {
        client_clean_disconnect( c, "A" );
    }
    // B: reconnect into the freed slot, traffic, then the server disconnects the client
    if ( client_connect( c, count, 30.0 ) && client_traffic_verify( c, "B", 120.0 ) )
    {
        client_control_now( c, CT_KICK, NULL, 0, 0 );
        expect_state( "B-server-disconnect", client_wait_end( c, 5.0 ), ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED, ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
        client_destroy( c );
    }
    // C: reconnect, traffic, then client-to-server silence past T
    if ( client_connect( c, count, 30.0 ) && client_traffic_verify( c, "C", 120.0 ) )
    {
        rocketnet_client_network_simulator( c->client, 0, 0, 100, 0 );
        printf( "CLI silence-c2s-start t=%.3f\n", rocketnet_time() );
        const int state = client_wait_end( c, 40.0 );
        rocketnet_client_network_simulator( c->client, 0, 0, 0, 0 );
        expect_state( "C-silence-c2s", state, ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT, ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
        client_destroy( c );
    }
    // D: reconnect (retried until the server frees the slot by its timeout), then server-to-client silence past T
    if ( client_connect( c, count, 40.0 ) && client_traffic_verify( c, "D", 120.0 ) )
    {
        const uint32_t w[1] = { past_timeout_ms };
        client_control_now( c, CT_OUTAGE, w, 1, 0 );
        printf( "CLI silence-s2c-start t=%.3f\n", rocketnet_time() );
        expect_state( "D-silence-s2c", client_wait_end( c, 40.0 ), ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT, ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
        client_destroy( c );
    }
    // E: reconnect into the slot the server timed out, traffic, clean disconnect
    if ( client_connect( c, count, 40.0 ) && client_traffic_verify( c, "E", 120.0 ) )
    {
        client_clean_disconnect( c, "E" );
    }
}

static int run_client( const char * address, const char * key_hex, const char * name, uint32_t count )
{
    g_address = address;
    if ( strlen( key_hex ) != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES )
    {
        printf( "CLI FAIL the server key wants %d hex characters\n", 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES );
        return 2;
    }
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        unsigned value = 0;
        if ( sscanf( key_hex + 2 * i, "%2x", &value ) != 1 )
        {
            printf( "CLI FAIL the server key is not hex\n" );
            return 2;
        }
        g_key[i] = (uint8_t) value;
    }
    struct xv_client_t * c = &g_client;
    const uint32_t outage_ms = ( XV_TOKEN_TIMEOUT - 1 ) * 1000;

    if ( strcmp( name, "lifecycle" ) == 0 )
    {
        case_lifecycle( c, count );
    }
    else if ( strcmp( name, "weather" ) == 0 )
    {
        if ( client_connect( c, count, 30.0 ) )
        {
            set_weather( c, 40, 30, 10, 5 );
            if ( client_traffic_verify( c, "weather", 240.0 ) )
            {
                set_weather( c, 0, 0, 0, 0 );
                client_pump( c, 0.5 );
                client_clean_disconnect( c, "weather" );
            }
        }
    }
    else if ( strcmp( name, "outage-c2s" ) == 0 || strcmp( name, "outage-s2c" ) == 0 || strcmp( name, "outage-both" ) == 0 )
    {
        const int dir = strcmp( name, "outage-c2s" ) == 0 ? 0 : ( strcmp( name, "outage-s2c" ) == 0 ? 1 : 2 );
        if ( client_connect( c, count, 30.0 ) && outage_during_traffic( c, name, dir, outage_ms ) && client_traffic_verify( c, name, 120.0 ) )
        {
            client_clean_disconnect( c, name );
        }
    }
    else if ( strcmp( name, "die" ) == 0 )
    {
        if ( client_connect( c, count, 30.0 ) )
        {
            client_pump( c, 1.0 );
            printf( "CLI ready-to-die\n" );
            fflush( stdout );
            client_pump( c, 600.0 );
        }
    }
    else if ( strcmp( name, "basic" ) == 0 )
    {
        if ( client_connect( c, count, 40.0 ) && client_traffic_verify( c, "basic", 120.0 ) )
        {
            client_clean_disconnect( c, "basic" );
        }
    }
    else
    {
        printf( "CLI FAIL unknown case %s\n", name );
        return 2;
    }
    client_destroy( c );
    printf( "CLI exit fails=%llu\n", (unsigned long long) g_fails );
    fflush( stdout );
    return g_fails ? 1 : 0;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, on_signal );
    signal( SIGTERM, on_signal );
    setvbuf( stdout, NULL, _IOLBF, 0 );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL rocketnet_init\n" );
        return 1;
    }
    int result = 2;
    if ( argc == 4 && strcmp( argv[1], "server" ) == 0 )
    {
        result = run_server( argv[2], atoi( argv[3] ) );
    }
    else if ( argc == 6 && strcmp( argv[1], "client" ) == 0 )
    {
        // the count is every stream's length: a sequence at or above XV_MAXSEQ is a bad header
        char * end = NULL;
        const unsigned long count = strtoul( argv[5], &end, 10 );
        if ( argv[5][0] < '0' || argv[5][0] > '9' || !end || *end != '\0' || count < 1 || count > XV_MAXSEQ )
        {
            printf( "CLI FAIL count %s is not a number in 1 .. %d\n", argv[5], XV_MAXSEQ );
        }
        else
        {
            result = run_client( argv[2], argv[3], argv[4], (uint32_t) count );
        }
    }
    else
    {
        printf( "usage: xver server <a.b.c.d:port> <max_clients>\n" );
        printf( "       xver client <a.b.c.d:port> <server public key hex> <case> <count>\n" );
    }
    rocketnet_term();
    return result;
}
