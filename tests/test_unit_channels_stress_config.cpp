/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "channels_stress_config.h"

#include "../sdk/rocketnet.h"

#include <string.h>

// The wide channel stress (bin/test_channels_1024, bin/test_channels_10k)
// runs only in the nightly. Its message config is checked here on every push,
// at this build's ROCKETNET_MAX_CHANNELS: the config rules that refuse it do
// not depend on the channel count, so a rule that refuses it goes red here
// first. A refused config plans zero bytes. The relations the stress relies
// on are checked too: the send queue and the sent-packet map each hold the
// whole reliable window. The channel count, the window and the receipts per
// update are pinned to the stress's own constants, a change detector for the
// shared helper.

static void test_channels_stress_config_is_valid()
{
    rocketnet_client_config_t client;
    rocketnet_unit_check( rocketnet_client_default_config( &client ) == ROCKETNET_STATUS_OK );
    client.send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    memset( client.packet_filter, 0x5A, sizeof( client.packet_filter ) );    // not the published test filter, which a production build refuses first
    channels_stress_message_config( &client.num_channels, &client.reliable_messages_in_flight, &client.send_message_buffer_bytes,
                                    &client.send_queue_messages, &client.packets_in_flight, &client.packets_per_update,
                                    &client.unreliable_messages_per_update, &client.max_message_receipts, ROCKETNET_MAX_CHANNELS );
    rocketnet_unit_check( rocketnet_client_required_memory( &client ) > 0 );

    // what the stress relies on, not only that the config is accepted: every
    // channel; a send queue that holds the whole reliable window; a sent-packet
    // map that holds it too (m9.1); and the receipts the stress reads per update
    rocketnet_unit_check( client.num_channels == ROCKETNET_MAX_CHANNELS );
    rocketnet_unit_check( client.reliable_messages_in_flight == CHANNELS_STRESS_WINDOW );
    rocketnet_unit_check( client.send_queue_messages >= client.reliable_messages_in_flight );
    rocketnet_unit_check( client.packets_in_flight >= client.reliable_messages_in_flight );
    rocketnet_unit_check( client.max_message_receipts == CHANNELS_STRESS_RECEIPTS_PER_UPDATE );

    rocketnet_server_config_t server;
    rocketnet_unit_check( rocketnet_server_default_config( &server ) == ROCKETNET_STATUS_OK );
    server.max_clients = 1;
    memset( server.backend_public_key, 0x11, sizeof( server.backend_public_key ) );
    server.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    memset( server.packet_filter, 0x5A, sizeof( server.packet_filter ) );
    channels_stress_message_config( &server.num_channels, &server.reliable_messages_in_flight, &server.send_message_buffer_bytes,
                                    &server.send_queue_messages, &server.packets_in_flight, &server.packets_per_update,
                                    &server.unreliable_messages_per_update, &server.max_message_receipts, ROCKETNET_MAX_CHANNELS );
    rocketnet_unit_check( rocketnet_server_required_memory( &server ) > 0 );
    rocketnet_unit_check( server.num_channels == ROCKETNET_MAX_CHANNELS );
    rocketnet_unit_check( server.reliable_messages_in_flight == CHANNELS_STRESS_WINDOW );
    rocketnet_unit_check( server.send_queue_messages >= server.reliable_messages_in_flight );
    rocketnet_unit_check( server.packets_in_flight >= server.reliable_messages_in_flight );
    rocketnet_unit_check( server.max_message_receipts == CHANNELS_STRESS_RECEIPTS_PER_UPDATE );
}

REGISTER_UNIT_TEST( test_channels_stress_config_is_valid );
