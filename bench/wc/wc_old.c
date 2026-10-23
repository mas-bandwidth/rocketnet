/* Worst-case server timing against the 2026-10-16 message-layer API
 * (rocketnet 40263777, the space pin). Mirrors load_split full load:
 * server ticks at 10 Hz and sends one 32-byte reliable-ordered message to
 * every connected client every update; every client updates at 10 Hz (100
 * clients per 10 ms slice) and sends one 32-byte message every update,
 * rotating the enabled reliability types on channel = type.
 *   wc_old server <n> <seconds> <address:port> <driver>
 *   wc_old clients <n> <seconds> <address:port> <driver> */
#include "rocketnet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#define BUCKETS 100001
enum { T_UPDATE, T_RECEIVE, T_START, T_FINISH, T_SEND, T_COUNT };
static const char * names[T_COUNT] = { "server_update", "server_receive_messages", "server_start_message", "server_finish_message", "server_send_packets" };
static uint64_t * buckets; static uint64_t count[T_COUNT], maxns[T_COUNT], over[T_COUNT];
static int timing_on;

static uint64_t ns( void ) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t) t.tv_sec * 1000000000ULL + (uint64_t) t.tv_nsec; }
static void rec( int c, uint64_t start ) {
    if ( !timing_on ) return;
    uint64_t e = ns() - start, b = e / 1000; if ( b >= BUCKETS ) b = BUCKETS - 1;
    buckets[(size_t) c * BUCKETS + b]++; count[c]++; if ( e > maxns[c] ) maxns[c] = e; if ( e >= 100000 ) over[c]++;
}
static uint64_t q( int c, unsigned p ) {
    uint64_t target = count[c] * p / 100, seen = 0;
    for ( uint64_t b = 0; b < BUCKETS; b++ ) { seen += buckets[(size_t) c * BUCKETS + b]; if ( seen > target ) return b + 1; }
    return (maxns[c] + 999) / 1000;
}

static const int types[5] = { ROCKETNET_UNRELIABLE, ROCKETNET_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABLE, ROCKETNET_RELIABLE_ORDERED, ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT };

static int run_server( int n, int seconds, const char * address, const char * driver )
{
    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config(&config);
    config.max_clients = n;
    snprintf(config.driver, sizeof(config.driver), "%s", driver);
    struct rocketnet_message_server_t * server = rocketnet_message_server_create(NULL, address, &config, NULL, NULL, 0, ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS);
    if ( !server ) { printf("wc: server create failed\n"); return 1; }
    printf("wc_ready: n=%d\n", n); fflush(stdout);
    buckets = (uint64_t *) calloc((size_t) T_COUNT * BUCKETS, sizeof(uint64_t));
    uint64_t received = 0, sent = 0, refused = 0; int connected = 0;
    double next = rocketnet_time(), start = -1.0, wait_start = next;
    for ( ;; )
    {
        double now = rocketnet_time();
        uint64_t t0 = ns(); int r = rocketnet_message_server_update(server, now); rec(T_UPDATE, t0);
        if ( r != ROCKETNET_MESSAGE_OK ) { printf("wc: update error %d\n", r); return 1; }
        connected = 0;
        t0 = ns();
        for ( int i = 0; i < n; i++ ) {
            if ( !rocketnet_message_server_client_connected(server, i) ) continue;
            connected++;
            for ( int ch = 0; ch < 8; ch++ ) { const uint8_t * data; int bytes;
                while ( rocketnet_message_server_receive(server, i, ch, &data, &bytes) ) { received++; rocketnet_message_server_release(server, i, data); } }
        }
        rec(T_RECEIVE, t0);
        if ( start < 0.0 && connected == n ) { start = now; timing_on = 1; printf("wc: all %d connected after %.1f s\n", n, now - wait_start); fflush(stdout); }
        if ( start < 0.0 && now - wait_start > 60.0 ) { printf("wc: only %d connected\n", connected); return 1; }
        if ( start >= 0.0 && now - start >= seconds ) break;
        if ( start >= 0.0 )
            for ( int i = 0; i < n; i++ ) {
                if ( !rocketnet_message_server_client_connected(server, i) ) continue;
                t0 = ns(); uint8_t * out = rocketnet_message_server_start(server, i, 3, ROCKETNET_RELIABLE_ORDERED, 32); rec(T_START, t0);
                if ( !out ) { refused++; continue; }
                memset(out, 0x5a, 32);
                t0 = ns(); uint32_t s = rocketnet_message_server_finish(server, i, 32); rec(T_FINISH, t0);
                if ( s ) sent++; else refused++;
            }
        t0 = ns(); rocketnet_message_server_send_packets(server); rec(T_SEND, t0);
        next += start >= 0.0 ? 0.100 : 0.010;
        double left = next - rocketnet_time(); if ( left > 0.0 ) rocketnet_sleep(left); else next = rocketnet_time();
    }
    timing_on = 0;
    for ( int c = 0; c < T_COUNT; c++ )
        printf("split_server_call: run_id=0000000000000001 total=%d transport=%s scope=traffic_plus_drain load=full call=%s count=%" PRIu64 " p50_upper_us=%" PRIu64 " p99_upper_us=%" PRIu64 " max_us=%.3f overbudget_count=%" PRIu64 " budget_us=100\n",
               n, driver, names[c], count[c], q(c, 50), q(c, 99), (double) maxns[c] / 1000.0, over[c]);
    printf("wc_server: connected_end=%d received=%" PRIu64 " sent=%" PRIu64 " refused=%" PRIu64 "\n", connected, received, sent, refused);
    rocketnet_message_server_destroy(server);
    return 0;
}

static int run_clients( int n, int seconds, const char * address, const char * driver )
{
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config(&config);
    snprintf(config.driver, sizeof(config.driver), "%s", driver);
    struct rocketnet_message_client_t ** clients = (struct rocketnet_message_client_t **) calloc((size_t) n, sizeof(*clients));
    for ( int i = 0; i < n; i++ ) {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( !rocketnet_write_dev_connect_token(address, rocketnet_random_uint64(), (uint64_t) time(NULL), token) ) { printf("wc: token failed\n"); return 1; }
        clients[i] = rocketnet_message_client_create(NULL, token, &config, NULL, NULL, 0, ROCKETNET_DEFAULT_CLIENT_SEND_KBPS);
        if ( !clients[i] ) { printf("wc: client create failed %d\n", i); return 1; }
    }
    uint64_t sent = 0, received = 0, refused = 0, missed = 0; int cursor = 0; unsigned rot = 0;
    double start = rocketnet_time(), next = start, all = -1.0;
    while ( rocketnet_time() - start < seconds + 90.0 )
    {
        int slice = n < 100 ? n : 100;
        int connected_now = 0;
        for ( int k = 0; k < slice; k++ ) {
            struct rocketnet_message_client_t * c = clients[cursor]; cursor = (cursor + 1) % n;
            rocketnet_message_client_update(c, rocketnet_time());
            for ( int ch = 0; ch < 8; ch++ ) { const uint8_t * data; int bytes;
                while ( rocketnet_message_client_receive(c, ch, &data, &bytes) ) { received++; rocketnet_message_client_release(c, data); } }
            if ( rocketnet_message_client_state(c) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) {
                connected_now++;
                int t = types[rot++ % 5];
                uint8_t * out = rocketnet_message_client_start(c, t, t, 32);
                if ( out ) { memset(out, 0xa5, 32); if ( rocketnet_message_client_finish(c, 32) ) sent++; else refused++; } else refused++;
            }
            rocketnet_message_client_send_packets(c);
        }
        if ( all < 0.0 ) { int ok = 1; for ( int i = 0; i < n; i++ ) if ( rocketnet_message_client_state(clients[i]) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) { ok = 0; break; }
            if ( ok ) { all = rocketnet_time(); printf("wc_clients: all connected after %.1f s\n", all - start); fflush(stdout); } }
        if ( all >= 0.0 && rocketnet_time() - all > seconds + 5.0 ) break;
        next += 0.010;
        double left = next - rocketnet_time(); if ( left > 0.0 ) rocketnet_sleep(left); else { missed++; next = rocketnet_time(); }
    }
    int disconnected = 0;
    for ( int i = 0; i < n; i++ ) if ( rocketnet_message_client_state(clients[i]) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) disconnected++;
    printf("wc_clients: sent=%" PRIu64 " received=%" PRIu64 " refused=%" PRIu64 " late_ticks=%" PRIu64 " not_connected=%d\n", sent, received, refused, missed, disconnected);
    for ( int i = 0; i < n; i++ ) rocketnet_message_client_destroy(clients[i]);
    return 0;
}

int main( int argc, char ** argv )
{
    if ( argc != 6 ) { fprintf(stderr, "usage: wc_old server|clients <n> <seconds> <address:port> <driver>\n"); return 2; }
    if ( !rocketnet_init(NULL) ) return 1;
    int rc = strcmp(argv[1], "server") == 0 ? run_server(atoi(argv[2]), atoi(argv[3]), argv[4], argv[5])
                                            : run_clients(atoi(argv[2]), atoi(argv[3]), argv[4], argv[5]);
    rocketnet_term();
    return rc;
}
