/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_MAX_CLIENTS is a build setting (-DROCKETNET_MAX_CLIENTS=10000,
// BUILDING.md "Maximum clients"). These cases hold every limit that serves all
// of a server's clients to the value the build was compiled with, pin the
// default build's limits to the values they had before the setting existed,
// and create a server of ROCKETNET_MAX_CLIENTS slots through the public API.
// They run in every build; the 10000 build runs them at 10000.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "suite_runner.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_constants.h"
#include "rocketnet_memory.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"
#include "rocketnet_server_gso.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_replay_marks.h"
#include "rocketnet_slots.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static void test_max_clients_derived_limits()
{
    const int clients = ROCKETNET_MAX_CLIENTS;

    // the default config asks for 1000, or fewer when the build holds fewer
    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.max_clients == ( clients < 1000 ? clients : 1000 ) );
    check( ROCKETNET_DEFAULT_MAX_CLIENTS == config.max_clients );

    // the server-wide tables hold every client
    check( (int) ( sizeof( ( (rn_slots_t *) 0 )->connection_id ) / sizeof( uint64_t ) ) == clients );
    check( ROCKETNET_LOOPBACK_MAX_ENDPOINTS == clients + 1 );
    check( ROCKETNET_XDP_REPLAY_MAX_ENTRIES >= clients );
    check( (int) ( sizeof( ( (rocketnet_xdp_replay_marks_t *) 0 )->mark_keys ) / sizeof( rocketnet_server_xdp_replay_key_t ) ) == ROCKETNET_XDP_REPLAY_MAX_ENTRIES );
    check( ROCKETNET_XDP_REPLY_MAC_ENTRIES >= 4 * clients );
    check( ( ROCKETNET_XDP_REPLY_MAC_ENTRIES & ( ROCKETNET_XDP_REPLY_MAC_ENTRIES - 1 ) ) == 0 );
    check( ROCKETNET_SERVER_PACKET_LEASES / 2 >= 2 * clients );
    check( ROCKETNET_SERVER_PACKET_LEASES / 2 >= ROCKETNET_SERVER_SEND_STAGING_PACKETS );
    check( ROCKETNET_SERVER_MAX_SEND_PACKETS == clients );
    check( ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE == ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * clients );

    // the server-wide send staging holds two packets for every client, and
    // each receive side twice that
    check( ROCKETNET_SERVER_SEND_STAGING_PACKETS >= 2 * clients );
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS == ROCKETNET_SERVER_SEND_STAGING_PACKETS );
    check( ROCKETNET_IOCP_SEND_QUEUE_SIZE == ROCKETNET_SERVER_SEND_STAGING_PACKETS );
    check( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE == 2 * ROCKETNET_SERVER_SEND_STAGING_PACKETS );
    check( ROCKETNET_RIO_SEND_QUEUE_SIZE == ROCKETNET_SERVER_SEND_STAGING_PACKETS );
    check( ROCKETNET_RIO_RECEIVES == 2 * ROCKETNET_SERVER_SEND_STAGING_PACKETS );

    // the per-queue rings: the most queues stage two packets for every client
    check( ROCKETNET_GSO_MAX_QUEUES * ROCKETNET_GSO_SEND_QUEUE_SIZE >= 2 * clients );
    check( ROCKETNET_URING_MAX_QUEUES * ROCKETNET_URING_SEND_QUEUE_SIZE >= 2 * clients );

    // the reassembly budget scales at 512 MiB per 1000 clients
    check( ROCKETNET_REASSEMBLY_MEMORY_MIB >= 512 * ( ( clients + 999 ) / 1000 ) );

    // the default build is unchanged: these are the values every limit had
    // when ROCKETNET_MAX_CLIENTS was a fixed 1000
    if ( clients == 1000 )
    {
        check( ROCKETNET_SERVER_SEND_STAGING_PACKETS == 2048 );
        check( ROCKETNET_LOOPBACK_QUEUE_PACKETS == 2048 );
        check( ROCKETNET_IOCP_SEND_QUEUE_SIZE == 2048 );
        check( ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE == 4096 );
        check( ROCKETNET_RIO_SEND_QUEUE_SIZE == 2048 );
        check( ROCKETNET_RIO_RECEIVES == 4096 );
        check( ROCKETNET_XDP_REPLAY_MAX_ENTRIES == 1024 );
        check( ROCKETNET_XDP_REPLY_MAC_ENTRIES == 4096 );
        check( ROCKETNET_SERVER_PACKET_LEASES == 131072 );
        check( ROCKETNET_REASSEMBLY_MEMORY_MIB == 512 );
        check( ROCKETNET_GSO_SEND_QUEUE_SIZE == 4096 );
        check( ROCKETNET_URING_SEND_QUEUE_SIZE == 2048 );
    }
}

REGISTER_UNIT_TEST( test_max_clients_derived_limits );

// A light per-client config, so a server of ROCKETNET_MAX_CLIENTS slots stays
// small enough for the unit suite (about 370 MB at 1000 clients, 3.7 GB at
// 10000). The slot count is what the case is about, not the per-client depth.
static void max_clients_server_config( rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
    config->transport = "loopback";
    config->max_clients = ROCKETNET_MAX_CLIENTS;
    config->per_client_send_kbps = 256;
    config->num_channels = 1;
    config->reliable_messages_in_flight = 64;
    config->receive_message_buffer_bytes = 4096;
    config->max_message_bytes = 4096;
    config->send_message_buffer_bytes = 16384;
    config->send_queue_messages = 16;
    config->packets_in_flight = 16;
    config->packets_per_update = 4;
    config->unreliable_messages_per_update = 16;
    config->max_message_receipts = 16;
    config->max_fragmented_messages = 1;
    config->message_resend_delay_initial_ms = 100;
}

static long max_clients_server_rss_bound_mb()
{
    rocketnet_server_config_t config;
    max_clients_server_config( &config );
    return suite_rss_bound_mb( (uint64_t) rocketnet_server_required_memory( &config ), 32 );
}

SUITE_CASE_RSS_BOUND( test_server_create_at_max_clients, max_clients_server_rss_bound_mb );

// The public API creates a server of every slot the build holds: create
// plans and allocates the one block for ROCKETNET_MAX_CLIENTS clients and
// opens no socket (start binds). One more client is refused.
static void test_server_create_at_max_clients()
{
    rocketnet_server_config_t config;
    max_clients_server_config( &config );
    check( rocketnet_server_required_memory( &config ) > 0 );

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_max_clients( server ) == ROCKETNET_MAX_CLIENTS );
    check( rocketnet_server_running( server ) == 0 );
    rn_test_server_stop_and_destroy( server );

    config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    status = ROCKETNET_TEST_STATUS_UNSET;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

REGISTER_UNIT_TEST( test_server_create_at_max_clients );
