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
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
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
    struct mode_stats_t stats[NUM_MODES]; char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES]; int sent = 0, result = 1;
    memset( stats, 0, sizeof( stats ) );
    for ( int m = 0; m < NUM_MODES; m++ ) stats[m].monotonic = stats[m].strict = 1;

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_public_key, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback"; server_config.num_channels = NUM_MODES;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    // the development network simulator: 20 ms latency, 35 ms jitter, 25% loss
    if ( server )
        rocketnet_server_network_simulator( server, 20, 35, 25, 0 );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );

    CHECK( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );
    rocketnet_server_public_key( server, server_public_key );
    CHECK( rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, 4102444800ull, NULL, connect_token ) == ROCKETNET_STATUS_OK );

    CHECK( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback"; client_config.num_channels = NUM_MODES;
    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    client = rocketnet_client_create( NULL, &client_config, &create_status_2 );
    if ( client )
        rocketnet_client_network_simulator( client, 20, 35, 25, 0 );
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
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // (2) receive and release every message, and read resolved ack receipts
        const uint8_t * data = NULL;
        int bytes = 0;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                    CHECK( bytes == 2 && data[0] < NUM_MODES && data[1] >= 1 && data[1] <= NUM_MSGS );
                    struct mode_stats_t * s = &stats[data[0]];
                    const uint8_t seq = data[1];
                    if ( s->arrived > 0 && seq <= s->order[s->arrived - 1] ) s->monotonic = 0;
                    if ( seq != (uint8_t) ( s->arrived + 1 ) ) s->strict = 0;
                    if ( s->seen[seq] ) s->dups++; else s->seen[seq] = 1;
                    if ( s->arrived < NUM_MSGS * 2 ) s->order[s->arrived] = seq;
                    s->arrived++;
            }
        }
        const int slot = rocketnet_client_index( client ); // -1 until CONNECTED
        int sender = -1;
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                sender = incoming->client_index[i];
            }
        }
        (void) data; (void) bytes; (void) sender;
        struct rocketnet_receipt_t receipts[32];
        const int n_rec = slot >= 0 ? rocketnet_server_receipts( server, slot, receipts, 32 ) : 0;
        for ( int r = 0; r < n_rec && r < 32; r++ )
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
                uint8_t * out = rocketnet_server_start_message( server, (uint64_t) slot, m, modes[m], 2 );
                CHECK( out != NULL );
                out[0] = (uint8_t) m; out[1] = (uint8_t) seq;
                stats[m].serials[seq - 1] = rocketnet_server_finish_message( server, 2 );
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
