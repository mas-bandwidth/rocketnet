/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Reliability: the server sends eight numbered messages in every reliability
    mode, one channel per mode, over loopback with simulated latency and
    jitter. The first message of each mode is dropped on purpose. The client
    prints the arrival order per mode, and the example checks each mode's
    delivery, ordering and receipt guarantees. Run: ./bin/example_reliability
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NUM_MODES 8
#define NUM_MESSAGES 8

static const enum rocketnet_reliability_t modes[NUM_MODES] = {
    ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED,
    ROCKETNET_RELIABILITY_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
    ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
    ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT };

static const char * const names[NUM_MODES] = {
    "unreliable", "unreliable_sequenced", "reliable", "reliable_ordered",
    "reliable_sequenced", "unreliable_receipt", "reliable_receipt", "reliable_ordered_receipt" };

struct mode_t
{
    uint32_t serials[NUM_MESSAGES];
    int order[NUM_MESSAGES * 2], arrived, duplicates, increasing, in_order, acked, lost;
    int seen[NUM_MESSAGES + 1];
};

int main( void )
{
    rocketnet_init( NULL, NULL, NULL );

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.num_channels = NUM_MODES;

    // the development network simulator: 20 ms latency, 35 ms jitter, set before start
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    if ( server )
        rocketnet_server_network_simulator( server, 20, 35, 0, 0 );
    if ( !server || rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start\n" );
        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) );
    rocketnet_server_public_key( server, server_public_key );
    rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.num_channels = NUM_MODES;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    rocketnet_client_network_simulator( client, 20, 35, 0, 0 );
    rocketnet_client_connect( client, connect_token );

    struct mode_t stats[NUM_MODES];
    memset( stats, 0, sizeof( stats ) );
    for ( int m = 0; m < NUM_MODES; m++ )
        stats[m].increasing = stats[m].in_order = 1;
    int sent = 0;

    for ( int frame = 0; frame < 400; frame++ )
    {
        const double now = 1000.0 + (double) frame * 0.010;
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        // each message is its mode and its number, 1 to 8
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                struct mode_t * s = &stats[batch->data[i][0]];
                const int number = batch->data[i][1];
                s->increasing &= s->arrived == 0 || number > s->order[s->arrived - 1];
                s->in_order &= number == s->arrived + 1;
                s->duplicates += s->seen[number];
                s->seen[number] = 1;
                if ( s->arrived < NUM_MESSAGES * 2 )
                    s->order[s->arrived++] = number;
            }
        }
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL ) {}

        const int slot = rocketnet_client_index( client );
        struct rocketnet_receipt_t receipts[32];
        const int count = slot >= 0 ? rocketnet_server_receipts( server, slot, receipts, 32 ) : 0;
        for ( int r = 0; r < count; r++ )
        {
            for ( int m = 5; m < NUM_MODES; m++ )
            {
                for ( int i = 0; i < NUM_MESSAGES; i++ )
                {
                    if ( stats[m].serials[i] != receipts[r].serial )
                        continue;
                    stats[m].acked += receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED;
                    stats[m].lost += receipts[r].status == ROCKETNET_RECEIPT_STATUS_LOSS;
                }
            }
        }

        // one message a frame; the first of each mode goes out at 100% loss.
        // The loss setting holds through send_packets, since an unreliable
        // message can leave at finish and a reliable one at send
        rocketnet_server_network_simulator( server, 20, 35, 0, 0 );
        if ( slot >= 0 && rocketnet_server_client_connected( server, slot ) && sent < NUM_MODES * NUM_MESSAGES )
        {
            const int m = sent / NUM_MESSAGES, number = sent % NUM_MESSAGES + 1;
            if ( number == 1 )
                rocketnet_server_network_simulator( server, 20, 35, 100, 0 );
            uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot, m, modes[m], 2 );
            p[0] = (uint8_t) m;
            p[1] = (uint8_t) number;
            stats[m].serials[number - 1] = rocketnet_server_finish_message( server, 2 );
            sent++;
        }

        // the client's packets carry its acks back to the server
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 );
            p[0] = 0;
            rocketnet_client_finish_message( client, 1 );
        }

        rocketnet_server_send_packets( server );
        rocketnet_client_send_packets( client );
        rocketnet_sleep( 0.0005 );
    }

    int result = sent == NUM_MODES * NUM_MESSAGES ? 0 : 1;
    for ( int m = 0; m < NUM_MODES; m++ )
    {
        const struct mode_t * s = &stats[m];
        const int last = s->arrived > 0 ? s->order[s->arrived - 1] : 0;
        const int sequenced = m == 1 || m == 4;
        char order[64] = "";
        for ( int i = 0, n = 0; i < s->arrived && n < 56; i++ )
            n += snprintf( order + n, sizeof( order ) - (size_t) n, "%s%d", i ? "," : "", s->order[i] );
        printf( "%-24s sent=%d arrived=%d order=[%s] gaps=%d duplicates=%d receipts=%d/%d\n", names[m], NUM_MESSAGES, s->arrived, order,
                sequenced ? last - s->arrived : NUM_MESSAGES - s->arrived + s->duplicates, s->duplicates, s->acked, s->lost );

        int ok = s->duplicates == 0;
        if ( m == 0 || m == 5 ) ok &= s->arrived == NUM_MESSAGES - 1 && !s->seen[1];      // unreliable: the dropped one is gone
        if ( sequenced ) ok &= s->increasing && s->arrived > 0;                            // sequenced: never older than the last
        if ( m == 4 ) ok &= last == NUM_MESSAGES;                                          // reliable sequenced: the newest arrives
        if ( m == 2 || m == 3 || m == 6 || m == 7 ) ok &= s->arrived == NUM_MESSAGES;      // reliable: every one arrives
        if ( m == 3 || m == 7 ) ok &= s->in_order;                                         // ordered: in the order sent
        if ( m >= 5 ) ok &= s->acked + s->lost == NUM_MESSAGES && s->acked == s->arrived;  // receipts: one per message
        if ( !ok )
        {
            printf( "FAIL: %s\n", names[m] );
            result = 1;
        }
    }

    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    if ( result == 0 )
        printf( "PASS\n" );
    return result;
}
