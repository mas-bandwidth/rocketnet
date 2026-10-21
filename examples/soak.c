/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
    Soak: 1000 clients against one server over a lossy link for N seconds,
    checking every message's integrity and order, and measuring server tick
    and client update timings, packet/byte rates, and allocator activity.
    This is a stress demo and profiling harness, not a load claim.
    Usage: ./bin/example_soak [--clients 1000] [--seconds 3] [--seed 1]
                              [--transport loopback] [--out soak-results.tsv]
                              [--pause] [--self-test-corrupt]
*/

#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Uses the message calls declared in sdk/rocketnet.h: start_message,
// finish_message, receive_messages (one batch, no release call) and
// send_packets, with int client indexes.
#define CHECK( cond ) do { if ( !( cond ) ) { printf( "FAIL: %s (line %d)\n", #cond, __LINE__ ); goto cleanup; } } while ( 0 )

// serialize a uint32_t little-endian straight into a started message
static int write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
    return 4;
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

#define MAX_SOAK_CLIENTS ROCKETNET_MAX_CLIENTS
#define BATCH_CLIENTS 250
#define DISCONNECT_BATCH 64
#define MAX_TICKS 7200

// MSVC deprecates fopen, so this uses fopen_s there
static FILE * soak_fopen( const char * path, const char * mode ) {
#if defined( _MSC_VER )
    FILE * file = NULL;
    return fopen_s( &file, path, mode ) == 0 ? file : NULL;
#else
    return fopen( path, mode );
#endif
}

struct soak_alloc_t { uint64_t live_allocs, live_bytes, peak_bytes, steady_allocs; bool steady; };
static struct soak_alloc_t g_alloc;

static void * soak_malloc( void * ctx, size_t bytes ) {
    (void) ctx;
    uint8_t * raw = (uint8_t *) malloc( bytes + 64 );
    if ( !raw ) return NULL;
    *(uint64_t *) raw = (uint64_t) bytes;
    g_alloc.live_allocs++;
    g_alloc.live_bytes += (uint64_t) bytes;
    if ( g_alloc.live_bytes > g_alloc.peak_bytes ) g_alloc.peak_bytes = g_alloc.live_bytes;
    if ( g_alloc.steady ) g_alloc.steady_allocs++;
    return raw + 64;
}

static void soak_free( void * ctx, void * p ) {
    (void) ctx;
    if ( !p ) return;
    uint8_t * raw = (uint8_t *) p - 64;
    g_alloc.live_allocs--;
    g_alloc.live_bytes -= *(uint64_t *) raw;
    free( raw );
}

struct soak_peer_t {
    uint32_t tx_seq[2], rx_ordered_seq, rx_sequenced_max, acked_events;
    bool rx_has_sequenced;
};

static uint8_t soak_byte( uint64_t seed, uint64_t client_idx, uint32_t kind, uint32_t seq, int i ) {
    uint64_t x = seed ^ ( client_idx * 0x9e3779b185ebca87ULL ) ^
                 ( (uint64_t) kind << 40 ) ^ ( (uint64_t) seq << 16 ) ^ (uint64_t) i;
    return (uint8_t) ( ( x ^ ( x >> 17 ) ^ ( x >> 33 ) ) & 0xffu );
}

static void soak_write_msg( uint8_t * out, int len, uint64_t seed, uint64_t client_idx, uint32_t kind, uint32_t seq ) {
    uint32_t hash = 2166136261u;
    for ( int i = 16; i < len; i++ ) {
        out[i] = soak_byte( seed, client_idx, kind, seq, i );
        hash = ( hash ^ out[i] ) * 16777619u;
    }
    uint8_t * p = out;
    p += write_u32( p, (uint32_t) client_idx );
    p += write_u32( p, ( kind & 0xffu ) | ( (uint32_t) len << 8 ) );
    p += write_u32( p, seq );
    p += write_u32( p, hash );
}

static bool soak_check_msg( const uint8_t * data, int len, uint64_t seed, uint64_t want_client,
                            int ch, struct soak_peer_t * rx, uint64_t * corrupt, uint64_t * gaps ) {
    if ( len < 16 ) { (*corrupt)++; printf( "FAIL: short message client=%" PRIu64 " seq=0\n", want_client ); return false; }
    const uint32_t hdr[4] = { read_u32( data ), read_u32( data + 4 ), read_u32( data + 8 ), read_u32( data + 12 ) };
    uint32_t client_id = hdr[0], kind = hdr[1] & 0xffu, seq = hdr[2], want_hash = hdr[3];
    int msg_len = (int) ( hdr[1] >> 8 );
    uint32_t hash = 2166136261u;
    for ( int i = 16; i < len; i++ ) {
        if ( data[i] != soak_byte( seed, client_id, kind, seq, i ) ) {
            (*corrupt)++;
            printf( "FAIL: corrupt body client=%u seq=%u byte=%d\n", client_id, seq, i );
            return false;
        }
        hash = ( hash ^ data[i] ) * 16777619u;
    }
    if ( client_id != (uint32_t) want_client || ch != (int) kind || msg_len != len || hash != want_hash ) {
        (*corrupt)++;
        printf( "FAIL: header/checksum mismatch client=%u seq=%u\n", client_id, seq );
        return false;
    }
    if ( kind == 1 ) {
        if ( seq != rx->rx_ordered_seq ) {
            (*gaps)++;
            printf( "FAIL: reliable-ordered gap client=%u expected=%u seq=%u\n", client_id, rx->rx_ordered_seq, seq );
            return false;
        }
        rx->rx_ordered_seq = seq + 1;
    } else {
        if ( rx->rx_has_sequenced && seq <= rx->rx_sequenced_max ) {
            (*gaps)++;
            printf( "FAIL: unreliable-sequenced backwards client=%u prev=%u seq=%u\n", client_id, rx->rx_sequenced_max, seq );
            return false;
        }
        rx->rx_has_sequenced = true;
        rx->rx_sequenced_max = seq;
    }
    return true;
}

static int cmp_double( const void * a, const void * b ) {
    double da = *(const double *) a, db = *(const double *) b;
    return ( da > db ) - ( da < db );
}

int main( int argc, char ** argv ) {
    int num_clients = 1000, seconds = 3, exit_code = 1;
    uint64_t seed = 1;
    const char * transport_name = "loopback";
    const char * out_path = "soak-results.tsv";
    bool pause_start = false, self_test_corrupt = false, inited = false;
    for ( int i = 1; i < argc; i++ ) {
        if ( strcmp( argv[i], "--clients" ) == 0 && i + 1 < argc ) num_clients = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc ) seconds = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--seed" ) == 0 && i + 1 < argc ) seed = (uint64_t) strtoull( argv[++i], NULL, 10 );
        else if ( strcmp( argv[i], "--transport" ) == 0 && i + 1 < argc ) transport_name = argv[++i];
        else if ( strcmp( argv[i], "--out" ) == 0 && i + 1 < argc ) out_path = argv[++i];
        else if ( strcmp( argv[i], "--pause" ) == 0 ) pause_start = true;
        else if ( strcmp( argv[i], "--self-test-corrupt" ) == 0 ) self_test_corrupt = true;
        else { printf( "FAIL: unknown argument %s\n", argv[i] ); return 1; }
    }
    if ( num_clients < 1 || num_clients > MAX_SOAK_CLIENTS || seconds < 1 || seconds * 60 > MAX_TICKS ) return 1;
    if ( pause_start ) {
        int pid = 0, ch = 0;
        char line[64];
        FILE * f = soak_fopen( "/proc/self/stat", "r" );
        if ( f ) { if ( fgets( line, sizeof( line ), f ) ) pid = (int) strtol( line, NULL, 10 ); fclose( f ); }
        printf( "PAUSED pid=%d (press Enter to start)\n", pid );
        fflush( stdout );
        while ( ( ch = getchar() ) != 10 && ch != EOF ) {}
    }
    struct soak_peer_t * c2s = (struct soak_peer_t *) calloc( (size_t) num_clients, sizeof( *c2s ) );
    struct soak_peer_t * s2c = (struct soak_peer_t *) calloc( (size_t) num_clients, sizeof( *s2c ) );
    struct rocketnet_client_t ** clients = (struct rocketnet_client_t **) calloc( (size_t) num_clients, sizeof( *clients ) );
    double * srv_us = (double *) calloc( MAX_TICKS, sizeof( double ) );
    double * cli_us = (double *) calloc( MAX_TICKS, sizeof( double ) );
    struct rocketnet_server_t * server = NULL;
    int * index_client = NULL;
    CHECK( c2s && s2c && clients && srv_us && cli_us );
    CHECK( rocketnet_allocator( soak_malloc, soak_free ) == ROCKETNET_STATUS_OK && rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    inited = true;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    struct rocketnet_server_config_t s_cfg;
    struct rocketnet_client_config_t c_cfg;
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &s_cfg );
    s_cfg.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( s_cfg.backend_public_key, dev_backend_public_key, sizeof( s_cfg.backend_public_key ) );
    CHECK( rocketnet_client_default_config( &c_cfg ) == ROCKETNET_STATUS_OK );
    s_cfg.max_clients = num_clients; s_cfg.transport = c_cfg.transport = transport_name;
    s_cfg.per_client_send_kbps = c_cfg.send_kbps = 100000; s_cfg.max_message_bytes = c_cfg.max_message_bytes = 2048;
    s_cfg.num_channels = c_cfg.num_channels = s_cfg.max_fragmented_messages = c_cfg.max_fragmented_messages = 2;
    s_cfg.reliable_messages_in_flight = c_cfg.reliable_messages_in_flight = s_cfg.send_queue_messages = c_cfg.send_queue_messages = 64;
    s_cfg.packets_in_flight = c_cfg.packets_in_flight = s_cfg.unreliable_messages_per_update = c_cfg.unreliable_messages_per_update = s_cfg.max_message_receipts = c_cfg.max_message_receipts = 64;
    s_cfg.receive_message_buffer_bytes = c_cfg.receive_message_buffer_bytes = s_cfg.send_message_buffer_bytes = c_cfg.send_message_buffer_bytes = 32768;
    s_cfg.sequenced_messages_held = c_cfg.sequenced_messages_held = 8;
    s_cfg.message_resend_delay_min_ms = c_cfg.message_resend_delay_min_ms = s_cfg.message_resend_delay_initial_ms = c_cfg.message_resend_delay_initial_ms = 200; s_cfg.message_resend_delay_max_ms = c_cfg.message_resend_delay_max_ms = 500;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &s_cfg, &create_status );
    // the development network simulator: lose 1% of every packet both ways
    if ( server )
        rocketnet_server_network_simulator( server, 0, 0, 1, 0 );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    char addr[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    CHECK( rocketnet_server_ipv4_address( server, addr, sizeof( addr ) ) );
    rocketnet_server_public_key( server, server_public_key );
    for ( uint64_t c = 0; c < (uint64_t) num_clients; c++ ) {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        CHECK( rocketnet_create_dev_connect_token( addr, NULL, server_public_key, 30, 10, c + 1, EXAMPLE_PROTOCOL_ID, 4102444800ULL, NULL, token ) == ROCKETNET_STATUS_OK );
        enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
        clients[c] = rocketnet_client_create( NULL, &c_cfg, &create_status_2 );
        if ( clients[c] )
            rocketnet_client_network_simulator( clients[c], 0, 0, 1, 0 );
        if ( !clients[c] ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
        if ( clients[c] && rocketnet_client_connect( clients[c], token ) != ROCKETNET_STATUS_OK )
        {
            rocketnet_client_destroy( clients[c] );
            clients[c] = NULL;
        }
        CHECK( clients[c] != NULL );
    }
    double sim_time = 1000.0;
    for ( int step = 0; step < 300; step++ ) {
        int all_conn = ( rocketnet_server_num_connected_clients( server ) == num_clients );
        for ( int c = 0; all_conn && c < num_clients; c++ )
            if ( rocketnet_client_state( clients[c] ) != ROCKETNET_CLIENT_STATE_CONNECTED ) all_conn = 0;
        if ( all_conn ) break;
        sim_time += 1.0 / 60.0;
        for ( int base = 0; base < num_clients; base += BATCH_CLIENTS ) {
            int end = base + BATCH_CLIENTS < num_clients ? base + BATCH_CLIENTS : num_clients;
            for ( int c = base; c < end; c++ ) {
                rocketnet_client_update( clients[c], sim_time );
                rocketnet_client_send_packets( clients[c] ); // send last so this frame's messages leave this frame
            }
            rocketnet_server_update( server, sim_time );
            rocketnet_server_send_packets( server ); // send last so this frame's messages leave this frame
        }
    }
    for ( int c = 0; c < num_clients; c++ )
        CHECK( rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    CHECK( rocketnet_server_num_connected_clients( server ) == num_clients );
    // the server receives from one queue across every client, each message
    // naming its sender's client index: this maps an index back to its client
    index_client = (int *) calloc( (size_t) num_clients, sizeof( int ) );
    CHECK( index_client != NULL );
    for ( int c = 0; c < num_clients; c++ )
        index_client[rocketnet_client_index( clients[c] )] = c;
    g_alloc.steady = true;
    const int soak_ticks = seconds * 60, drain_ticks = 90, stride = num_clients > 25 ? ( num_clients / 25 ) : 1;
    uint64_t sent_rel = 0, recv_rel = 0, sent_seq = 0, recv_seq = 0, corrupt = 0, gaps = 0;
    bool injected = false;
    for ( int tick = 0; tick < soak_ticks + drain_ticks; tick++ ) {
        sim_time += 1.0 / 60.0;
        double tick_cli_us = 0.0, tick_srv_us = 0.0;
        for ( int base = 0; base < num_clients; base += BATCH_CLIENTS ) {
            int end = base + BATCH_CLIENTS < num_clients ? base + BATCH_CLIENTS : num_clients;
            double t0 = rocketnet_time();
            for ( int c = base; c < end; c++ ) {
                // (1) update pumps the transport and takes in what arrived
                rocketnet_client_update( clients[c], sim_time );
                // (2) receive every received message, in batches from the one
                // queue: each entry names its channel, and the next batch
                // call or update releases the batch
                const struct rocketnet_message_batch_t * batch = NULL;
                while ( ( batch = rocketnet_client_receive_messages( clients[c] ) ) != NULL ) {
                    for ( int m = 0; m < batch->num_messages; m++ ) {
                        const int ch = batch->channel[m];
                        bool ok = soak_check_msg( batch->data[m], batch->bytes[m], seed, (uint64_t) c, ch, &s2c[c], &corrupt, &gaps );
                        CHECK( ok );
                        if ( ch == 1 ) recv_rel++; else recv_seq++;
                    }
                }
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_client_receipts( clients[c], recs, 16 );
                for ( int r = 0; r < nrec; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ) c2s[c].acked_events++;
                // (3) simulate & (4) queue this frame's outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_client_start_message( clients[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, c2s[c].tx_seq[0]++ );
                    CHECK( rocketnet_client_finish_message( clients[c], 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_client_start_message( clients[c], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
                        CHECK( p1 != NULL );
                        soak_write_msg( p1, 96, seed, (uint64_t) c, 1, c2s[c].tx_seq[1]++ );
                        if ( self_test_corrupt && !injected ) { p1[32] ^= 0xffu; injected = true; }
                        CHECK( rocketnet_client_finish_message( clients[c], 96 ) != 0 );
                        sent_rel++;
                    }
                }
                // (5) send_packets last so this frame's messages leave this frame
                rocketnet_client_send_packets( clients[c] );
            }
            tick_cli_us += ( rocketnet_time() - t0 ) * 1e6;
            double s0 = rocketnet_time();
            // (1) server update
            rocketnet_server_update( server, sim_time );
            // (2) server receive, in batches from the one queue across every
            // client: each entry names its sender and its channel
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL ) {
                for ( int m = 0; m < batch->num_messages; m++ ) {
                    CHECK( batch->client_index[m] >= 0 && batch->client_index[m] < num_clients );
                    const int c = index_client[batch->client_index[m]];
                    const int ch = batch->channel[m];
                    bool ok = soak_check_msg( batch->data[m], batch->bytes[m], seed, (uint64_t) c, ch, &c2s[c], &corrupt, &gaps );
                    CHECK( ok );
                    if ( ch == 1 ) recv_rel++; else recv_seq++;
                }
            }
            for ( int c = base; c < end; c++ ) {
                const int client_idx = rocketnet_client_index( clients[c] );
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_server_receipts( server, client_idx, recs, 16 );
                for ( int r = 0; r < nrec; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ) s2c[c].acked_events++;
                // (3) simulate & (4) server queue outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_server_start_message( server, (uint64_t) client_idx, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, s2c[c].tx_seq[0]++ );
                    CHECK( rocketnet_server_finish_message( server, 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_server_start_message( server, (uint64_t) client_idx, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
                        CHECK( p1 != NULL );
                        soak_write_msg( p1, 96, seed, (uint64_t) c, 1, s2c[c].tx_seq[1]++ );
                        CHECK( rocketnet_server_finish_message( server, 96 ) != 0 );
                        sent_rel++;
                    }
                }
            }
            // (5) server send_packets last so this frame's messages leave this frame
            rocketnet_server_send_packets( server );
            tick_srv_us += ( rocketnet_time() - s0 ) * 1e6;
        }
        if ( tick < soak_ticks ) { cli_us[tick] = tick_cli_us; srv_us[tick] = tick_srv_us; }
    }
    g_alloc.steady = false;
    uint64_t acked_total = 0;
    for ( int c = 0; c < num_clients; c++ ) {
        acked_total += c2s[c].acked_events + s2c[c].acked_events;
        CHECK( c2s[c].rx_ordered_seq == c2s[c].tx_seq[1] && s2c[c].rx_ordered_seq == s2c[c].tx_seq[1] );
    }
    struct rocketnet_server_stats_t srv_stats;
    rocketnet_server_stats( server, &srv_stats );
    uint64_t srv_pkts_tx = srv_stats.packets_sent;
    uint64_t srv_pkts_rx = srv_stats.packets_received;
    uint64_t srv_bytes_tx = srv_stats.bytes_sent;
    uint64_t srv_bytes_rx = srv_stats.bytes_received;
    CHECK( srv_stats.receipts_lost == 0 );
    CHECK( srv_stats.messages_dropped_over_budget == 0 );
    CHECK( srv_stats.api_misuses == 0 );
    CHECK( sent_rel > 0 && recv_rel == sent_rel && acked_total == sent_rel && recv_seq > 0 && corrupt == 0 && gaps == 0 );
    for ( int pass = 0; pass < 2 && rocketnet_server_num_connected_clients( server ) > 0; pass++ ) {
        for ( int base = 0; base < num_clients; base += DISCONNECT_BATCH ) {
            int end = base + DISCONNECT_BATCH < num_clients ? base + DISCONNECT_BATCH : num_clients;
            for ( int c = base; c < end; c++ ) {
                rocketnet_client_disconnect( clients[c] );
                rocketnet_client_send_packets( clients[c] ); // send last so this frame's messages leave this frame
            }
            sim_time += 1.0 / 60.0;
            rocketnet_server_update( server, sim_time );
            rocketnet_server_send_packets( server ); // send last so this frame's messages leave this frame
        }
    }
    CHECK( rocketnet_server_num_connected_clients( server ) == 0 );
    for ( int c = 0; c < num_clients; c++ ) { rocketnet_client_destroy( clients[c] ); clients[c] = NULL; }
    rocketnet_server_destroy( server ); server = NULL;
    rocketnet_term(); inited = false;
    CHECK( g_alloc.live_allocs == 0 && g_alloc.live_bytes == 0 );
    qsort( srv_us, (size_t) soak_ticks, sizeof( double ), cmp_double );
    qsort( cli_us, (size_t) soak_ticks, sizeof( double ), cmp_double );
    double srv_p50 = srv_us[soak_ticks / 2], srv_p99 = srv_us[( soak_ticks * 99 ) / 100], srv_max = srv_us[soak_ticks - 1];
    double cli_p50 = cli_us[soak_ticks / 2], cli_p99 = cli_us[( soak_ticks * 99 ) / 100], cli_max = cli_us[soak_ticks - 1];
    FILE * fp = soak_fopen( out_path, "w" );
    CHECK( fp != NULL );
    const char * m_names[] = { "sent_reliable", "recv_reliable", "acked_reliable", "sent_sequenced", "recv_sequenced", "corrupt", "seq_gaps", "peak_bytes",
                               "server_tick_us_p50", "server_tick_us_p99", "server_tick_us_max", "client_frame_us_p50", "client_frame_us_p99", "client_frame_us_max",
                               "client_per_peer_us_p50", "client_per_peer_us_p99", "server_pkts_per_s_tx", "server_pkts_per_s_rx", "server_bytes_per_s_tx", "server_bytes_per_s_rx", "allocs_per_tick" };
    double m_vals[] = { (double) sent_rel, (double) recv_rel, (double) acked_total, (double) sent_seq, (double) recv_seq, (double) corrupt, (double) gaps, (double) g_alloc.peak_bytes,
                        srv_p50, srv_p99, srv_max, cli_p50, cli_p99, cli_max, cli_p50 / num_clients, cli_p99 / num_clients,
                        (double) srv_pkts_tx / seconds, (double) srv_pkts_rx / seconds, (double) srv_bytes_tx / seconds, (double) srv_bytes_rx / seconds, (double) g_alloc.steady_allocs / soak_ticks };
    fprintf( fp, "clients\tseconds\tseed\ttransport\tbuild\tmetric\tvalue\n" );
    for ( int m = 0; m < 21; m++ )
        fprintf( fp, m < 8 ? "%d\t%d\t%" PRIu64 "\t%s\tRelease\t%s\t%.0f\n" : "%d\t%d\t%" PRIu64 "\t%s\tRelease\t%s\t%.2f\n", num_clients, seconds, seed, transport_name, m_names[m], m_vals[m] );
    fclose( fp );
    printf( "soak: clients=%d seconds=%d seed=%" PRIu64 " transport=%s rel=%" PRIu64 "/%" PRIu64 " seq=%" PRIu64 "/%" PRIu64 " srv_p50=%.1fus srv_p99=%.1fus cli_peer_p50=%.2fus cli_peer_p99=%.2fus (budget 100us) allocs/tick=%.2f peak_bytes=%" PRIu64 "\n",
            num_clients, seconds, seed, transport_name, recv_rel, sent_rel, recv_seq, sent_seq,
            srv_p50, srv_p99, cli_p50 / num_clients, cli_p99 / num_clients,
            (double) g_alloc.steady_allocs / soak_ticks, g_alloc.peak_bytes );
    exit_code = 0;

cleanup:
    if ( clients ) for ( int c = 0; c < num_clients; c++ ) if ( clients[c] ) rocketnet_client_destroy( clients[c] );
    if ( server ) rocketnet_server_destroy( server );
    if ( inited ) rocketnet_term();
    free( index_client ); free( clients ); free( c2s ); free( s2c ); free( srv_us ); free( cli_us );
    // PASS after the cleanup, so it stays the last line
    if ( exit_code == 0 )
        printf( "PASS\n" );
    return exit_code;
}
