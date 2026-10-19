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

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Note: sdk/rocketnet.h at BASE declares the pre-rename message calls
// (*_start, *_finish, *_receive, *_release, *_send_packets) and int client_index.
#define CHECK( cond ) do { if ( !( cond ) ) { printf( "FAIL: %s (line %d)\n", #cond, __LINE__ ); goto cleanup; } } while ( 0 )
#define MAX_SOAK_CLIENTS ROCKETNET_MAX_CLIENTS
#define BATCH_CLIENTS 250
#define DISCONNECT_BATCH 64
#define MAX_TICKS 7200

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
    uint32_t hdr[4] = { (uint32_t) client_idx, ( kind & 0xffu ) | ( (uint32_t) len << 8 ), seq, hash };
    memcpy( out, hdr, 16 );
}

static bool soak_check_msg( const uint8_t * data, int len, uint64_t seed, uint64_t want_client,
                            int ch, struct soak_peer_t * rx, uint64_t * corrupt, uint64_t * gaps ) {
    if ( len < 16 ) { (*corrupt)++; printf( "FAIL: short message client=%" PRIu64 " seq=0\n", want_client ); return false; }
    uint32_t hdr[4];
    memcpy( hdr, data, 16 );
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
        FILE * f = fopen( "/proc/self/stat", "r" );
        if ( f ) { if ( fscanf( f, "%d", &pid ) != 1 ) pid = 0; fclose( f ); }
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
    CHECK( c2s && s2c && clients && srv_us && cli_us );
    CHECK( rocketnet_allocator( soak_malloc, soak_free ) && rocketnet_init( NULL ) );
    inited = true;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    struct rocketnet_server_config_t s_cfg;
    struct rocketnet_client_config_t c_cfg;
    CHECK( rocketnet_server_dev_config_sized( &s_cfg, sizeof( s_cfg ) ) );
    CHECK( rocketnet_client_default_config_sized( &c_cfg, sizeof( c_cfg ) ) );
    s_cfg.max_clients = num_clients; c_cfg.step_driven = 1; s_cfg.transport = c_cfg.transport = transport_name;
    s_cfg.per_client_send_kbps = c_cfg.send_kbps = 100000; s_cfg.max_message_bytes = c_cfg.max_message_bytes = 2048;
    s_cfg.num_channels = c_cfg.num_channels = s_cfg.num_reassembly_slots = c_cfg.num_reassembly_slots = 2;
    s_cfg.window_messages = c_cfg.window_messages = s_cfg.send_queue_messages = c_cfg.send_queue_messages = 64;
    s_cfg.packets_in_flight = c_cfg.packets_in_flight = s_cfg.unreliable_deliveries = c_cfg.unreliable_deliveries = s_cfg.receipt_ring = c_cfg.receipt_ring = 64;
    s_cfg.receive_ring_bytes = c_cfg.receive_ring_bytes = s_cfg.send_buffer_bytes = c_cfg.send_buffer_bytes = 32768;
    s_cfg.sequenced_holds = c_cfg.sequenced_holds = 8; s_cfg.simulated_loss_percent = c_cfg.simulated_loss_percent = 1;
    s_cfg.rto_min_ms = c_cfg.rto_min_ms = s_cfg.rto_initial_ms = c_cfg.rto_initial_ms = 200; s_cfg.rto_max_ms = c_cfg.rto_max_ms = 500;
    server = rocketnet_server_create_sized( NULL, "127.0.0.1:0", &s_cfg, sizeof( s_cfg ), NULL, 0 );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    char addr[64];
    snprintf( addr, sizeof( addr ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    for ( uint64_t c = 0; c < (uint64_t) num_clients; c++ ) {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        CHECK( rocketnet_write_dev_connect_token( addr, c + 1, 4102444800ULL, token ) );
        clients[c] = rocketnet_client_create_sized( NULL, &c_cfg, sizeof( c_cfg ), NULL, 0 );
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
                CHECK( rocketnet_client_update( clients[c], sim_time ) == ROCKETNET_MESSAGE_OK );
                rocketnet_client_send_packets( clients[c] ); // send last so this frame's messages leave this frame
            }
            CHECK( rocketnet_server_update( server, sim_time ) == ROCKETNET_MESSAGE_OK );
            rocketnet_server_send_packets( server ); // send last so this frame's messages leave this frame
        }
    }
    for ( int c = 0; c < num_clients; c++ )
        CHECK( rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    CHECK( rocketnet_server_num_connected_clients( server ) == num_clients );
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
                CHECK( rocketnet_client_update( clients[c], sim_time ) == ROCKETNET_MESSAGE_OK );
                // (2) receive and release every received message
                for ( int ch = 0; ch < 2; ch++ ) {
                    const uint8_t * data = NULL; int bytes = 0;
                    while ( rocketnet_client_receive_message( clients[c], ch, &data, &bytes ) ) {
                        bool ok = soak_check_msg( data, bytes, seed, (uint64_t) c, ch, &s2c[c], &corrupt, &gaps );
                        rocketnet_client_release_message( clients[c], data );
                        CHECK( ok );
                        if ( ch == 1 ) recv_rel++; else recv_seq++;
                    }
                }
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_client_receipts( clients[c], recs, 16 );
                for ( int r = 0; r < nrec; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_ACKED ) c2s[c].acked_events++;
                // (3) simulate & (4) queue this frame's outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_client_start_message( clients[c], 0, ROCKETNET_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, c2s[c].tx_seq[0]++ );
                    CHECK( rocketnet_client_finish_message( clients[c], 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_client_start_message( clients[c], 1, ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
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
            CHECK( rocketnet_server_update( server, sim_time ) == ROCKETNET_MESSAGE_OK );
            for ( int c = base; c < end; c++ ) {
                uint64_t client_idx = (uint64_t) rocketnet_client_index( clients[c] );
                // (2) server receive and release
                for ( int ch = 0; ch < 2; ch++ ) {
                    const uint8_t * data = NULL; int bytes = 0;
                    while ( rocketnet_server_receive_message( server, (int) client_idx, ch, &data, &bytes ) ) {
                        bool ok = soak_check_msg( data, bytes, seed, (uint64_t) c, ch, &c2s[c], &corrupt, &gaps );
                        rocketnet_server_release_message( server, (int) client_idx, data );
                        CHECK( ok );
                        if ( ch == 1 ) recv_rel++; else recv_seq++;
                    }
                }
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_server_receipts( server, (int) client_idx, recs, 16 );
                for ( int r = 0; r < nrec; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_ACKED ) s2c[c].acked_events++;
                // (3) simulate & (4) server queue outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_server_start_message( server, (int) client_idx, 0, ROCKETNET_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, s2c[c].tx_seq[0]++ );
                    CHECK( rocketnet_server_finish_message( server, (int) client_idx, 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_server_start_message( server, (int) client_idx, 1, ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
                        CHECK( p1 != NULL );
                        soak_write_msg( p1, 96, seed, (uint64_t) c, 1, s2c[c].tx_seq[1]++ );
                        CHECK( rocketnet_server_finish_message( server, (int) client_idx, 96 ) != 0 );
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
    uint64_t srv_pkts_tx = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_PACKETS_SENT );
    uint64_t srv_pkts_rx = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED );
    uint64_t srv_bytes_tx = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_BYTES_SENT );
    uint64_t srv_bytes_rx = rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED );
    CHECK( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST ) == 0 );
    CHECK( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_BUDGET_DROPS ) == 0 );
    CHECK( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED ) == 0 );
    CHECK( rocketnet_api_misuse_count() == 0 );
    CHECK( sent_rel > 0 && recv_rel == sent_rel && acked_total == sent_rel && recv_seq > 0 && corrupt == 0 && gaps == 0 );
    for ( int pass = 0; pass < 2 && rocketnet_server_num_connected_clients( server ) > 0; pass++ ) {
        for ( int base = 0; base < num_clients; base += DISCONNECT_BATCH ) {
            int end = base + DISCONNECT_BATCH < num_clients ? base + DISCONNECT_BATCH : num_clients;
            for ( int c = base; c < end; c++ ) {
                rocketnet_client_disconnect( clients[c] );
                rocketnet_client_send_packets( clients[c] ); // send last so this frame's messages leave this frame
            }
            sim_time += 1.0 / 60.0;
            CHECK( rocketnet_server_update( server, sim_time ) == ROCKETNET_MESSAGE_OK );
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
    FILE * fp = fopen( out_path, "w" );
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
    free( clients ); free( c2s ); free( s2c ); free( srv_us ); free( cli_us );
    // PASS after the cleanup, so it stays the last line
    if ( exit_code == 0 )
        printf( "PASS\n" );
    return exit_code;
}
