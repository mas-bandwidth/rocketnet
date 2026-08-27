/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box client: connects to a server with a development
    token minted locally from the published development keys, then
    exchanges payloads. Run ./bin/server, then ./bin/client. Pass an
    address to connect elsewhere: ./bin/client 192.0.2.10:40000
*/

#include "rocketnet.h"
#include <stdio.h>
#include <signal.h>

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    const char * connect_address = argc > 1 ? argv[1] : "127.0.0.1:40000";

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, connect_address );
    if ( !client )
    {
        rocketnet_term();
        return 1;
    }

    int last_state = 0;
    uint64_t payloads_sent = 0;
    uint64_t payloads_received = 0;

    while ( !quit )
    {
        rocketnet_client_update( client );

        const int state = rocketnet_client_state( client );
        if ( state != last_state )
        {
            printf( "client state: %s\n", rocketnet_client_state_name( state ) );
            last_state = state;
            if ( state < ROCKETNET_CLIENT_STATE_DISCONNECTED )
                break;
        }

        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            rocketnet_client_send_packet( client, payload, sizeof(payload) );
            payloads_sent++;

            uint8_t received[ROCKETNET_MAX_PAYLOAD_BYTES];
            int received_bytes;
            while ( ( received_bytes = rocketnet_client_receive_packet( client, received ) ) > 0 )
            {
                payloads_received++;
                if ( ( payloads_received % 100 ) == 0 )
                    printf( "client sent %llu and received %llu payloads\n",
                            (unsigned long long) payloads_sent, (unsigned long long) payloads_received );
            }
        }

        rocketnet_sleep( 0.01 );
    }

    printf( "\nclient disconnecting\n" );
    rocketnet_client_disconnect( client );
    rocketnet_sleep( 0.1 );
    rocketnet_client_destroy( client );

    rocketnet_term();

    return 0;
}
