/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Reliability: send a numbered run of messages in every reliability mode
    RocketNet offers (one channel per mode) over the loopback transport with
    simulated packet loss, latency, and jitter, print what arrived and in what
    order, and check each mode's delivery, ordering, and receipt guarantees.
    Run it from the repository root: ./bin/example_reliability
*/

#include "rocketnet.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// This example uses the send names the header at this base declares (start,
// finish, receive, release, send_packets); a later lifecycle rename makes
// them *_start_message and friends.

#define CHECK( cond ) do { if ( !( cond ) ) { printf( "FAIL: %s\n", #cond ); goto cleanup; } } while ( 0 )
#define NUM_MODES 8
#define NUM_MSGS  8

struct mode_stats_t {
    uint8_t order[NUM_MSGS * 2], seen[NUM_MSGS + 1]; uint32_t serials[NUM_MSGS];
    int arrived, dups, monotonic, strict, acked, lost;
};
int main( void ) {
    static const int modes[NUM_MODES] = {
        ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_RELIABLE,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
        ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT
    };
    static const char * const names[NUM_MODES] = {
        "unreliable", "unreliable_sequenced", "reliable", "reliable_ordered",
        "reliable_sequenced", "unreliable_receipt", "reliable_receipt", "reliable_ordered_receipt"
    };
    struct rocketnet_server_t * server = NULL; struct rocketnet_client_t * client = NULL;
    struct rocketnet_server_config_t server_config; struct rocketnet_client_config_t client_config;
    struct mode_stats_t stats[NUM_MODES]; uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char server_address[64]; int sent = 0, result = 1;
    memset( stats, 0, sizeof( stats ) );
    for ( int m = 0; m < NUM_MODES; m++ ) stats[m].monotonic = stats[m].strict = 1;

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    CHECK( (rocketnet_server_dev_config)( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.transport = "loopback"; server_config.num_channels = NUM_MODES;
    server_config.simulated_loss_percent = 25; server_config.simulated_latency_ms = 20; server_config.simulated_jitter_ms = 35;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = (rocketnet_server_create)( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    CHECK( rocketnet_write_dev_connect_token( server_address, 1, 4102444800ull, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    CHECK( (rocketnet_client_default_config)( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback"; client_config.num_channels = NUM_MODES; client_config.step_driven = true;
    client_config.simulated_loss_percent = 25; client_config.simulated_latency_ms = 20; client_config.simulated_jitter_ms = 35;
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = (rocketnet_client_create)( NULL, &client_config, sizeof( client_config ), NULL, 0, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );

    const double wall_start = rocketnet_time();
    for ( int frame = 0; frame < 400 && rocketnet_time() - wall_start < 60.0; frame++ )
    {
        const double now = 1000.0 + (double) frame * 0.010;
        // (1) update pumps the transport and takes in what arrived
        CHECK( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        CHECK( rocketnet_client_update( client, now ) == ROCKETNET_UPDATE_OK );

        // (2) receive and release every message, and read resolved ack receipts
        const uint8_t * data = NULL;
        int bytes = 0;
        for ( int ch = 0; ch < NUM_MODES; ch++ )
            while ( rocketnet_client_receive_message( client, ch, &data, &bytes ) )
            {
                CHECK( bytes == 2 && data[0] < NUM_MODES && data[1] >= 1 && data[1] <= NUM_MSGS );
                struct mode_stats_t * s = &stats[data[0]];
                const uint8_t seq = data[1];
                if ( s->arrived > 0 && seq <= s->order[s->arrived - 1] ) s->monotonic = 0;
                if ( seq != (uint8_t) ( s->arrived + 1 ) ) s->strict = 0;
                if ( s->seen[seq] ) s->dups++; else s->seen[seq] = 1;
                if ( s->arrived < NUM_MSGS * 2 ) s->order[s->arrived] = seq;
                s->arrived++;
                rocketnet_client_release_message( client, data );
            }
        const int slot = rocketnet_client_index( client );
        while ( slot >= 0 && rocketnet_server_receive_message( server, slot, 0, &data, &bytes ) )
            rocketnet_server_release_message( server, slot, data );
        struct rocketnet_receipt_t receipts[32];
        const int n_rec = slot >= 0 ? rocketnet_server_receipts( server, slot, receipts, 32 ) : 0;
        for ( int r = 0; r < n_rec; r++ )
            for ( int m = 5; m < NUM_MODES; m++ )
                for ( int i = 0; i < NUM_MSGS; i++ )
                    if ( stats[m].serials[i] == receipts[r].serial )
                        receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ? stats[m].acked++ : stats[m].lost++;

        // (3) run the game simulation for the frame
        // (4) queue this frame's outgoing messages with start and finish
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             slot >= 0 && rocketnet_server_client_connected( server, slot ) )
        {
            if ( sent < NUM_MODES * NUM_MSGS )
            {
                const int m = sent / NUM_MSGS, seq = sent % NUM_MSGS + 1;
                uint8_t * out = rocketnet_server_start_message( server, slot, m, modes[m], 2 );
                CHECK( out != NULL );
                out[0] = (uint8_t) m; out[1] = (uint8_t) seq;
                stats[m].serials[seq - 1] = rocketnet_server_finish_message( server, slot, 2 );
                CHECK( stats[m].serials[seq - 1] != 0 );
                sent++;
            }
            uint8_t * tick = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 );
            if ( tick ) { tick[0] = 0; CHECK( rocketnet_client_finish_message( client, 1 ) != 0 ); }
        }

        // (5) send_packets is last so this frame's messages leave this frame
        rocketnet_server_send_packets( server );
        rocketnet_client_send_packets( client );
        rocketnet_sleep( 0.0005 );
    }

    CHECK( sent == NUM_MODES * NUM_MSGS );
    for ( int m = 0; m < NUM_MODES; m++ )
    {
        const struct mode_stats_t * s = &stats[m];
        const int max_seq = s->arrived > 0 ? s->order[s->arrived - 1] : 0;
        const int gaps = ( m == 1 || m == 4 ) ? ( max_seq - s->arrived ) : ( NUM_MSGS - s->arrived + s->dups );
        char ord[64] = "";
        for ( int i = 0, pos = 0; i < s->arrived && pos < 56; i++ )
            pos += snprintf( ord + pos, sizeof( ord ) - (size_t) pos, "%s%u", i ? "," : "", s->order[i] );
        printf( "%-24s sent=%d arrived=%d order=[%s] gaps=%d duplicates=%d receipts=%d/%d\n",
                names[m], NUM_MSGS, s->arrived, ord, gaps, s->dups, s->acked, s->lost );
        CHECK( s->dups == 0 );
        if ( m == 0 || m == 5 ) CHECK( s->arrived < NUM_MSGS );
        if ( m == 1 || m == 4 ) CHECK( s->monotonic && s->arrived > 0 );
        if ( m == 2 || m == 3 || m == 6 || m == 7 ) CHECK( s->arrived == NUM_MSGS && gaps == 0 );
        if ( m == 3 || m == 7 ) CHECK( s->strict );
        if ( m >= 5 ) CHECK( s->acked + s->lost == NUM_MSGS && s->acked == s->arrived );
    }
    result = 0;
cleanup:
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();
    // destroy ends a live connection and logs it, so PASS is printed after
    // the cleanup to stay the last line
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
