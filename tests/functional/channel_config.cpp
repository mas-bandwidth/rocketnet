/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional checks for the CONFIG announcement (standard/MESSAGES.md m11) over
    the channel harness: two objects whose wire configuration differs refuse
    each other with CONFIG_MISMATCH, and two equal objects recover from the
    first CONFIG packet each way being lost, verify, and exchange reliable
    ordered messages over a 10% loss link.
*/

#include "channel_harness.h"
#include "../portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rocketnet_config_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_config_check( condition )                                                    \
do                                                                                             \
{                                                                                              \
    if ( !(condition) )                                                                        \
    {                                                                                          \
        rocketnet_config_check_handler( #condition, (const char*) __FUNCTION__,                \
                                         (const char*) __FILE__, __LINE__ );                    \
    }                                                                                          \
} while( 0 )

static const int rocketnet_config_message_count = 32;
static const int rocketnet_config_payload_bytes = 32;
static const double rocketnet_config_step = 1.0 / 60.0;

static float rocketnet_config_loss_percent( float fallback )
{
    char value[256];
    if ( rocketnet_test_getenv( "ROCKETNET_CHANNEL_LOSS_PERCENT", value, sizeof( value ) ) && value[0] )
        return (float) atof( value );
    return fallback;
}

static void rocketnet_config_fill( uint8_t * data, int index )
{
    for ( int i = 0; i < rocketnet_config_payload_bytes; i++ )
    {
        uint32_t x = (uint32_t) index * 0x9E3779B9u + (uint32_t) i * 0x85EBCA6Bu + 0xC2B2AE35u;
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        data[i] = (uint8_t) ( x >> 24 );
    }
}

static void rocketnet_config_print_failure( const char * name, rocketnet_channel_harness_t * h,
                                            double elapsed, int client_reason, int server_reason,
                                            int delivered_client, int delivered_server )
{
    rocketnet_channel_harness_read_counters( h );
    printf( "%s: seed=0x%016llx elapsed_s=%.3f resends=%llu client_reason=%d server_reason=%d delivered_client=%d delivered_server=%d\n",
            name, (unsigned long long) h->seed, elapsed, (unsigned long long) h->resends,
            client_reason, server_reason, delivered_client, delivered_server );
    fflush( stdout );
}

// RNEndpoint OnPayload's CONFIG half (standard/MESSAGES.md m11): each endpoint
// compares the remote endpoint's CONFIG body field by field with its own and ends the
// connection with CONFIG_MISMATCH on the first difference. The server object
// is given a different num_channels before its reset announces its CONFIG, so
// the two objects on the wire disagree exactly as two differently configured
// builds do.
void config_mismatch_over_loopback()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_config_check( 0 );
        return;
    }

    h.message_server->object.config.num_channels = 3;

    const double timeout = (double) h.message_config.reliable_message_timeout_ms / 1000.0;
    const double start = h.now;
    int client_reason = ROCKETNET_DISCONNECT_REASON_NONE;
    int server_reason = ROCKETNET_DISCONNECT_REASON_NONE;
    int updates = 0;

    // Hold the payload packets both ways while the transport connects, so the
    // first CONFIG each way is handed over together and both ends compare
    // before either one's disconnect can take the other's CONFIG out of reach.

    rocketnet_channel_harness_set_link( &h, 0.020, 0.010, rocketnet_config_loss_percent( 0.0f ), 0.0f );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS );

    int held_to_server = 0;
    int held_to_client = 0;
    while ( ( !h.connected || held_to_server == 0 || held_to_client == 0 ) &&
            h.now - start <= timeout && updates < (int) ( timeout * 60.0 ) + 60 )
    {
        rocketnet_channel_harness_pump( &h, rocketnet_config_step );
        updates++;
        held_to_server = 0;
        held_to_client = 0;

        for ( int i = 0; i < h.held_count; i++ )
        {
            if ( h.held_to_server[i] )
            {
                held_to_server++;
            }
            else
            {
                held_to_client++;
            }
        }
    }

    if ( held_to_server > 0 && held_to_client > 0 )
    {
        int indices[ROCKETNET_CHANNEL_HARNESS_MAX_HELD];

        for ( int i = 0; i < h.held_count; i++ )
        {
            indices[i] = i;
        }

        rocketnet_channel_harness_deliver_held( &h, indices, h.held_count );
        rocketnet_channel_harness_clear_held( &h );
    }

    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );

    while ( ( client_reason != ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH ||
              server_reason != ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH ) &&
            h.now - start <= timeout && updates < (int) ( timeout * 60.0 ) + 60 )
    {
        rocketnet_channel_harness_pump( &h, rocketnet_config_step );
        updates++;
        client_reason = rocketnet_message_client_disconnect_reason( h.message_client );
        server_reason = rocketnet_message_server_disconnect_reason( h.message_server, h.client_slot );
    }

    const double elapsed = h.now - start;

    if ( client_reason != ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH ||
         server_reason != ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH )
        rocketnet_config_print_failure( "test_loopback_transport_config_mismatch", &h, elapsed,
                                        client_reason, server_reason, 0, 0 );

    rocketnet_config_check( client_reason == ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH );
    rocketnet_config_check( server_reason == ROCKETNET_DISCONNECT_REASON_CONFIG_MISMATCH );
    rocketnet_config_check( elapsed <= timeout );
    rocketnet_config_check( rocketnet_channel_harness_overflows( &h ) == 0 );

    rocketnet_channel_harness_destroy( &h );
}

// RNEndpoint ConfigReliable (standard/MESSAGES.md m11): CONFIG is reliable
// number 0 and is resent until acknowledged. Holding every payload packet
// while the transport connects drops the first CONFIG each way; once the wire
// passes again the resends must verify both ends, which then exchange 32
// reliable-ordered messages each way over a 10% loss link.
void config_equal_over_lossy_loopback()
{
    rocketnet_channel_harness_t h;
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    if ( !rocketnet_channel_harness_create_with_config( &h, &config ) )
    {
        rocketnet_config_check( 0 );
        return;
    }

    rocketnet_channel_harness_set_link( &h, 0.020, 0.010, rocketnet_config_loss_percent( 10.0f ), 0.0f );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS );

    const double timeout = (double) h.message_config.reliable_message_timeout_ms / 1000.0;
    const double start = h.now;
    int updates = 0;
    int held_to_server = 0;
    int held_to_client = 0;

    while ( !h.connected && h.now - start <= timeout && updates < (int) ( timeout * 60.0 ) + 60 )
    {
        rocketnet_channel_harness_pump( &h, rocketnet_config_step );
        updates++;
    }

    // the first CONFIG packet each way was sent while payloads were held

    for ( int i = 0; i < h.held_count; i++ )
    {
        if ( h.held_to_server[i] )
        {
            held_to_server++;
        }
        else
        {
            held_to_client++;
        }
    }

    rocketnet_channel_harness_clear_held( &h );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );

    int client_verified = h.message_client->object.connections[0].ordered.config_received;
    int server_verified = h.message_server->object.connections[h.client_slot].ordered.config_received;
    while ( ( !client_verified || !server_verified ) && h.now - start <= timeout && updates < (int) ( timeout * 60.0 ) + 60 )
    {
        rocketnet_channel_harness_pump( &h, rocketnet_config_step );
        updates++;
        client_verified = h.message_client->object.connections[0].ordered.config_received;
        server_verified = h.message_server->object.connections[h.client_slot].ordered.config_received;
    }

    int sent_to_server = 0;
    int sent_to_client = 0;
    int delivered_server = 0;
    int delivered_client = 0;
    uint8_t payload[rocketnet_config_payload_bytes];
    uint8_t expected[rocketnet_config_payload_bytes];
    int order_error = 0;

    while ( ( delivered_server < rocketnet_config_message_count || delivered_client < rocketnet_config_message_count ) &&
            h.now - start <= timeout && updates < (int) ( timeout * 60.0 ) + 60 )
    {
        while ( sent_to_server < rocketnet_config_message_count )
        {
            rocketnet_config_fill( payload, sent_to_server );
            if ( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, rocketnet_config_payload_bytes ) == 0 )
                break;
            sent_to_server++;
        }

        while ( sent_to_client < rocketnet_config_message_count )
        {
            rocketnet_config_fill( payload, sent_to_client );
            if ( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, rocketnet_config_payload_bytes ) == 0 )
                break;
            sent_to_client++;
        }

        rocketnet_channel_harness_pump( &h, rocketnet_config_step );
        updates++;

        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            if ( delivered_server < rocketnet_config_message_count )
            {
                rocketnet_config_fill( expected, delivered_server );
                if ( delivery.bytes != rocketnet_config_payload_bytes || memcmp( delivery.data, expected, rocketnet_config_payload_bytes ) != 0 )
                {
                    order_error = 1;
                }
            }

            delivered_server++;
        }

        while ( rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
        {
            if ( delivered_client < rocketnet_config_message_count )
            {
                rocketnet_config_fill( expected, delivered_client );
                if ( delivery.bytes != rocketnet_config_payload_bytes || memcmp( delivery.data, expected, rocketnet_config_payload_bytes ) != 0 )
                {
                    order_error = 1;
                }
            }

            delivered_client++;
        }
    }

    const double elapsed = h.now - start;
    const int client_reason = rocketnet_message_client_disconnect_reason( h.message_client );
    const int server_reason = rocketnet_message_server_disconnect_reason( h.message_server, h.client_slot );

    const int ok = h.connected && held_to_server > 0 && held_to_client > 0 &&
                   client_verified && server_verified &&
                   client_reason == ROCKETNET_DISCONNECT_REASON_NONE && server_reason == ROCKETNET_DISCONNECT_REASON_NONE &&
                   sent_to_server == rocketnet_config_message_count && sent_to_client == rocketnet_config_message_count &&
                   delivered_server == rocketnet_config_message_count && delivered_client == rocketnet_config_message_count &&
                   !order_error && rocketnet_channel_harness_overflows( &h ) == 0;

    if ( !ok )
        rocketnet_config_print_failure( "test_loopback_transport_config_equal_over_lossy", &h, elapsed,
                                        client_reason, server_reason, delivered_client, delivered_server );

    rocketnet_config_check( h.connected );
    rocketnet_config_check( held_to_server > 0 );
    rocketnet_config_check( held_to_client > 0 );
    rocketnet_config_check( client_verified );
    rocketnet_config_check( server_verified );
    rocketnet_config_check( client_reason == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_config_check( server_reason == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_config_check( sent_to_server == rocketnet_config_message_count );
    rocketnet_config_check( sent_to_client == rocketnet_config_message_count );
    rocketnet_config_check( delivered_server == rocketnet_config_message_count );
    rocketnet_config_check( delivered_client == rocketnet_config_message_count );
    rocketnet_config_check( !order_error );
    rocketnet_config_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_config_check( rocketnet_channel_allocator_calls == h.allocations_after_create );

    rocketnet_channel_harness_destroy( &h );
}
