/* RocketNet. Copyright 2026 Más Bandwidth LLC.
   Licensed under the GNU Affero General Public License v3.0 */
#include "channel_harness.h"

static void packing_check_impl( bool ok, const char * condition, int line )
{
    if ( ok )
        return;
    printf( "unreliable packing failed: %s at line %d\n", condition, line );
    fflush( stdout );
    exit( 1 );
}
#define packing_check( x ) packing_check_impl( (x), #x, __LINE__ )

void unreliable_small_messages_pack_in_place()
{
    for ( int to_server = 0; to_server < 2; ++to_server )
        for ( int bytes : { 1, 4, 64 } )
        {
            rocketnet_channel_harness_t h;
            packing_check( rocketnet_channel_harness_create_with_unreliable_messages_per_update( &h, 64 ) );
            rocketnet_channel_harness_set_link( &h, 0, 0, 0, 0 );
            packing_check( rocketnet_channel_harness_connect( &h, 600 ) );

            for ( int i = 0; i < 120; ++i )
            {
                rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
            }

            rocketnet_channel_harness_set_wire( &h, to_server ? ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS : ROCKETNET_CHANNEL_WIRE_PASS,
                                                to_server ? ROCKETNET_CHANNEL_WIRE_PASS : ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS );
            rocketnet_channel_endpoint_t * endpoint = to_server ? &h.client_endpoint : &h.server_endpoint;
            const int count = 32;
            for ( int i = 0; i < count; ++i )
            {
                uint8_t * p = to_server ? rocketnet_message_client_start( h.message_client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes ) : rocketnet_message_server_start( h.message_server, h.client_slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
                packing_check( p != NULL );

            // the application writes directly into the transport's final frame

                const uintptr_t address = (uintptr_t) p;
                const uintptr_t begin = (uintptr_t) &endpoint->frame[0][0];
                packing_check( address >= begin && address + (uintptr_t) bytes <= begin + sizeof( endpoint->frame ) );
                memset( p, i, (size_t) bytes );
                packing_check( ( to_server ? rocketnet_message_client_finish( h.message_client, bytes ) : rocketnet_message_server_finish( h.message_server, h.client_slot, bytes ) ) != 0 );
            }

            if ( to_server )
            {
                rocketnet_message_client_send_packets( h.message_client );
            }
            else
            {
                rocketnet_message_server_send_packets( h.message_server );
            }

            const int per_packet = ( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 ) / ( bytes + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES );
            const int expected = ( count + per_packet - 1 ) / per_packet;

            printf( "unreliable packing direction=%s message_bytes=%d messages=%d packets=%d expected=%d\n",
                    to_server ? "client_to_server" : "server_to_client", bytes, count, h.held_count, expected );
            fflush( stdout );
            packing_check( h.held_count == expected );

            int order[ROCKETNET_CHANNEL_HARNESS_MAX_HELD];

            for ( int i = 0; i < h.held_count; ++i )
            {
                order[i] = i;
                packing_check( h.held_bytes[i] <= ROCKETNET_SEND_BUDGET );
            }

            packing_check( rocketnet_channel_harness_deliver_held( &h, order, h.held_count ) == h.held_count );
            rocketnet_channel_harness_clear_held( &h );
            rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );

            uint32_t seen = 0;

            for ( int step = 0; step < 10; ++step )
            {
                packing_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );

                rocketnet_channel_delivery_t delivery = {};
                while ( to_server ? rocketnet_channel_harness_server_collect( &h, 0, &delivery ) : rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
                {
                    packing_check( delivery.bytes == bytes && delivery.data[0] < count );

                    const uint32_t bit = 1u << delivery.data[0];
                    packing_check( !( seen & bit ) );

                    for ( int j = 0; j < bytes; ++j )
                    {
                        packing_check( delivery.data[j] == delivery.data[0] );
                    }

                    seen |= bit;
                }
            }

            packing_check( seen == UINT32_MAX );
            packing_check( rocketnet_channel_harness_overflows( &h ) == 0 );
            packing_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
            rocketnet_channel_harness_destroy( &h );
        }
}
