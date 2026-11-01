/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_MAX_CLIENTS is a build setting (-DROCKETNET_MAX_CLIENTS=10000,
// MEMORY.md "Maximum clients"): the most clients a config may ask for. Every
// server buffer that scales with clients is sized from the configured
// max_clients, so these cases hold each sizing to the configured count in
// every build, pin the sizes at 1000 clients to the values they had when the
// build ceiling was a fixed 1000, show a server's plan follows its config and
// not the ceiling, and create a server of ROCKETNET_MAX_CLIENTS slots through
// the public API. They run in every build; the 10000 build runs them at 10000.

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
#include "rocketnet_client_slots.h"

#include <stdint.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

static void test_max_clients_derived_limits()
{
    const int clients = ROCKETNET_MAX_CLIENTS;

    // the default config asks for 1000, or fewer when the build holds fewer

    rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.max_clients == ( clients < 1000 ? clients : 1000 ) );
    check( ROCKETNET_DEFAULT_MAX_CLIENTS == config.max_clients );

    // the build-wide tables hold every client the build allows: the loopback
    // hub's records, the embedded XDP program's replay map and the packet
    // lease table

    check( rocketnet_loopback_hub_bytes( clients ) > 0 && rocketnet_loopback_hub_bytes( clients + 1 ) == 0 );
    check( ROCKETNET_XDP_REPLAY_MAX_ENTRIES >= clients );
    check( rocketnet_xdp_replay_entries( clients ) <= ROCKETNET_XDP_REPLAY_MAX_ENTRIES );
    check( rocketnet_xdp_replay_entries( clients + 1 ) == 0 );
    check( ROCKETNET_SERVER_PACKET_LEASES / 2 >= 2 * clients );
    check( ROCKETNET_SERVER_PACKET_LEASES / 2 >= rocketnet_server_send_staging_packets( clients ) );

    // each server-wide sizing follows the configured count, raised to the
    // smallest sizing count and never past the build's

    const int counts[] = { 1, ROCKETNET_SERVER_MIN_SIZED_CLIENTS, ROCKETNET_DEFAULT_MAX_CLIENTS, clients };
    for ( const int n : counts )
    {
        const int sized = n > ROCKETNET_SERVER_MIN_SIZED_CLIENTS ? n : ROCKETNET_SERVER_MIN_SIZED_CLIENTS;
        check( rocketnet_server_sized_clients( n ) == sized );
        check( rocketnet_server_receive_packets_per_update( n ) == ROCKETNET_RECEIVE_PACKETS_PER_CLIENT * sized );
        check( rocketnet_server_send_batch_packets( n ) == sized );

        // the server-wide send staging holds two packets for every client,
        // and each receive side twice that

        const int staging = rocketnet_server_send_staging_packets( n );
        check( staging >= 2 * sized && staging < 4 * sized && ( staging & ( staging - 1 ) ) == 0 );
        check( rocketnet_server_iocp_send_queue_size( n ) == staging );
        check( rocketnet_server_iocp_receive_queue_size( n ) == 2 * staging );
        check( rocketnet_server_rio_send_queue_size( n ) == staging );
        check( rocketnet_server_rio_receives( n ) == 2 * staging );

        const int macs = rocketnet_xdp_reply_mac_entries( n );
        check( macs >= 4 * sized && ( macs & ( macs - 1 ) ) == 0 );
        check( rocketnet_xdp_replay_entries( n ) >= (uint32_t) n );
        check( rocketnet_server_uring_bank_packets( n ) >= rocketnet_server_receive_packets_per_update( n ) );
    }
    check( rocketnet_server_sized_clients( clients + 1 ) == clients );
    check( ROCKETNET_LOOPBACK_QUEUE_PACKETS == 2 * ROCKETNET_CONNECTION_MAX_PACKETS_PER_UPDATE );

    // the gso producer bank and the io_uring send array are each one
    // server-wide staging whatever the queue count, sized for two packets per
    // client of the default configuration; a larger server spreads its sends
    // across updates as the transport's send room reports

    check( ROCKETNET_GSO_SEND_QUEUE_SIZE >= 2 * ROCKETNET_DEFAULT_MAX_CLIENTS );
    check( ROCKETNET_URING_SEND_QUEUE_SIZE >= 2 * ROCKETNET_DEFAULT_MAX_CLIENTS );

    // the reassembly budget scales at 512 MiB per 1000 clients

    check( ROCKETNET_REASSEMBLY_MEMORY_MIB >= 512 * ( ( clients + 999 ) / 1000 ) );

    // a server of 1000 clients is sized as it was when ROCKETNET_MAX_CLIENTS
    // was a fixed 1000, in every build that holds 1000

    if ( clients >= 1000 )
    {
        check( rocketnet_server_receive_packets_per_update( 1000 ) == 16000 );
        check( rocketnet_server_send_batch_packets( 1000 ) == 1000 );
        check( rocketnet_server_send_staging_packets( 1000 ) == 2048 );
        check( rocketnet_server_iocp_send_queue_size( 1000 ) == 2048 );
        check( rocketnet_server_iocp_receive_queue_size( 1000 ) == 4096 );
        check( rocketnet_server_rio_send_queue_size( 1000 ) == 2048 );
        check( rocketnet_server_rio_receives( 1000 ) == 4096 );
        check( rocketnet_xdp_replay_entries( 1000 ) == 1024 );
        check( rocketnet_xdp_reply_mac_entries( 1000 ) == 4096 );
        check( rocketnet_server_uring_bank_packets( 1000 ) == 16000 );
        check( rocketnet_loopback_server_queue_packets( 1000 ) == 16384 );
    }

    // the default build's fixed tables are unchanged

    if ( clients == 1000 )
    {
        check( ROCKETNET_LOOPBACK_QUEUE_PACKETS == 2048 );
        check( ROCKETNET_XDP_REPLAY_MAX_ENTRIES == 1024 );
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
    config->packets_in_flight = 64;                 // at least the window: the sent map holds W entries
    config->packets_per_update = 4;
    config->unreliable_messages_per_update = 16;
    config->max_message_receipts = 16;
    config->max_fragmented_messages = 1;
    config->message_resend_delay_initial_ms = 100;
}

// A server's memory is its config's choice: its plan follows the configured
// max_clients, not the build's ROCKETNET_MAX_CLIENTS. Nearly all of the light
// config's plan is buffers that scale with clients, so a 1000-client server
// plans close to 1000 / ROCKETNET_MAX_CLIENTS of a full one, and the check
// allows twice that for the plan's client-independent part. Sized from the
// build ceiling instead, a 1000-client server in a 10000 build would plan
// nearly what a 10000-client one does, ten times its share. In the default
// build the two plans are the same server.

static void test_server_plan_follows_configured_max_clients()
{
    if ( ROCKETNET_MAX_CLIENTS < 1000 )
        return;

    const char * transports[] = { "portable", NULL };
    for ( const char * transport : transports )
    {
        rocketnet_server_config_t config;
        max_clients_server_config( &config );
        config.transport = transport;

        const uint64_t at_max = (uint64_t) rocketnet_server_required_memory( &config );
        config.max_clients = 1000;
        const uint64_t at_1000 = (uint64_t) rocketnet_server_required_memory( &config );
        config.max_clients = 999;
        const uint64_t at_999 = (uint64_t) rocketnet_server_required_memory( &config );

        check( at_max > 0 && at_1000 > 0 && at_999 > 0 );
        check( at_999 <= at_1000 && at_1000 <= at_max );
        check( at_1000 * (uint64_t) ROCKETNET_MAX_CLIENTS <= 2 * 1000 * at_max );
    }
}

REGISTER_UNIT_TEST( test_server_plan_follows_configured_max_clients );

// the suite reads the bound before any case runs, with no library and no
// hub, so it plans the portable transport (a superset of loopback's plan,
// which allocates nothing) and adds the case's test hub
static long max_clients_server_rss_bound_mb()
{
    rocketnet_server_config_t config;
    max_clients_server_config( &config );
    config.transport = "portable";
    const uint64_t server = (uint64_t) rocketnet_server_required_memory( &config );
    if ( server == 0 )
        return 0;
    return suite_rss_bound_mb( server + rocketnet_loopback_hub_bytes( ROCKETNET_TEST_LOOPBACK_HUB_CLIENTS ) + rocketnet_allocation_overhead(), 32 );
}

SUITE_CASE_RSS_BOUND( test_server_create_at_max_clients, max_clients_server_rss_bound_mb );

// The public API creates a server of every slot the build holds: create
// plans and allocates the one block for ROCKETNET_MAX_CLIENTS clients and
// opens no socket (start binds). One more client is refused.

static void test_server_create_at_max_clients()
{
    rocketnet_server_config_t config;
    max_clients_server_config( &config );
    config.loopback_hub = rocketnet_test_loopback_hub();
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

#if ROCKETNET_DEV

// The flat index is deliberately tested through its development-only probe
// seam. Sixteen distinct full-width IDs start twelve cells before the end of
// the table. Removing the middle entry then shifts survivors across the end
// through zero, and must not hide any later member; a stale generation handle
// must not remove the replacement that reuses its client slot.

static void test_client_slots_hostile_collision_and_stale_generation()
{
    const int capacity = 32;
    const int collision_count = 16;
    const uint64_t probe_bound = (uint64_t) capacity * 2;
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, capacity );
    check( slots != NULL );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0xC7, sizeof( user_data ) );

    uint64_t ids[collision_count];
    const uint64_t wanted = probe_bound - 12;
    check( rocketnet_client_slots_test_force_hash_index( slots, (int) wanted ) );

    for ( int i = 0; i < collision_count; ++i )
    {
        ids[i] = UINT64_C( 0x80000000A5A5A5A5 ) | ( (uint64_t) (uint32_t) i << 32 );
        check( rocketnet_client_slots_test_hash_index( slots, ids[i] ) == wanted );
    }

    struct rocketnet_client_slot_handle_t handles[collision_count];
    const uint64_t probes_before = rocketnet_client_slots_test_total_probes( slots );

    for ( int i = 0; i < collision_count; ++i )
    {
        check( rocketnet_client_slots_assign( slots, ids[i], i, ids[i], user_data ) == i );
        check( rocketnet_client_slots_test_last_probes( slots ) >= 1 );
        check( rocketnet_client_slots_test_last_probes( slots ) <= probe_bound );
        check( rocketnet_client_slots_handle( slots, i, &handles[i] ) );
        check( handles[i].client_id == ids[i] );
        check( handles[i].client_index == i );
    }

    for ( int i = 0; i < collision_count; ++i )
    {
        check( rocketnet_client_slots_find( slots, ids[i] ) == i );
        check( rocketnet_client_slots_test_last_probes( slots ) >= 1 );
        check( rocketnet_client_slots_test_last_probes( slots ) <= probe_bound );
    }

    check( rocketnet_client_slots_test_total_probes( slots ) > probes_before );

    const int middle = collision_count / 2;
    const struct rocketnet_client_slot_handle_t stale = handles[middle];
    check( rocketnet_client_slots_release_handle( slots, &handles[middle] ) );
    check( rocketnet_client_slots_test_last_probes( slots ) >= 1 );
    check( rocketnet_client_slots_test_last_probes( slots ) <= probe_bound );
    check( rocketnet_client_slots_find( slots, ids[middle] ) == -1 );
    for ( int i = middle + 1; i < collision_count; ++i )
    {
        check( rocketnet_client_slots_find( slots, ids[i] ) == i );
    }

    const uint64_t replacement = UINT64_C( 0xF0000010A5A5A5A5 );
    check( rocketnet_client_slots_test_hash_index( slots, replacement ) == wanted );
    check( rocketnet_client_slots_assign( slots, replacement, 99, replacement, user_data ) == middle );
    check( rocketnet_client_slots_handle( slots, middle, &handles[middle] ) );
    check( handles[middle].client_id == replacement );
    check( handles[middle].generation != stale.generation );

    // The stale release names the old full ID and generation. It is refused
    // without disturbing the replacement or any member behind it.

    check( !rocketnet_client_slots_release_handle( slots, &stale ) );
    check( rocketnet_client_slots_find( slots, replacement ) == middle );
    check( rocketnet_client_slots_mapping_index( slots, middle ) == 99 );
    check( memcmp( rocketnet_client_slots_user_data( slots, middle ), user_data, sizeof( user_data ) ) == 0 );

    for ( int i = 0; i < collision_count; ++i )
    {
        if ( i != middle )
        {
            check( rocketnet_client_slots_find( slots, ids[i] ) == i );
        }
    }

    for ( int i = 0; i < collision_count; ++i )
    {
        check( rocketnet_client_slots_release_handle( slots, &handles[i] ) );
    }
    check( rocketnet_client_slots_num_connected( slots ) == 0 );
    check( rocketnet_client_slots_num_free( slots ) == capacity );
    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_client_slots_hostile_collision_and_stale_generation );

#endif // #if ROCKETNET_DEV

// Exercise the runtime maximum rather than a small fixture: initial indices
// remain ascending, identities which share their low 32 bits remain distinct,
// the dense free stack stays LIFO, a full table refuses without mutation, and
// repeated arbitrary reuse leaves every untouched identity reachable.

static void test_client_slots_full_capacity_reuse_and_full_width_ids()
{
    const int capacity = ROCKETNET_MAX_CLIENTS;
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, capacity );
    check( slots != NULL );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0x5A, sizeof( user_data ) );

    for ( int i = 0; i < capacity; ++i )
    {
        const uint64_t id = ( (uint64_t) (uint32_t) ( i + 1 ) << 32 ) | UINT64_C( 0xA5A5A5A5 );
        check( rocketnet_client_slots_assign( slots, id, i, id, user_data ) == i );
    }

    check( rocketnet_client_slots_num_connected( slots ) == capacity );
    check( rocketnet_client_slots_num_free( slots ) == 0 );
    check( rocketnet_client_slots_assign( slots, UINT64_MAX, -1, UINT64_MAX, user_data ) == -1 );

    for ( int i = 0; i < capacity; ++i )
    {
        const uint64_t id = ( (uint64_t) (uint32_t) ( i + 1 ) << 32 ) | UINT64_C( 0xA5A5A5A5 );
        check( rocketnet_client_slots_find( slots, id ) == i );
    }

    int released[8];
    const int released_count = capacity < 8 ? capacity : 8;
    for ( int i = 0; i < released_count; ++i )
    {
        released[i] = ( capacity / 3 + i ) % capacity;
        rocketnet_client_slots_release( slots, released[i] );
    }

    check( rocketnet_client_slots_num_free( slots ) == released_count );

    for ( int i = 0; i < released_count; ++i )
    {
        const int expected = released[released_count - 1 - i];
        const uint64_t id = UINT64_C( 0xF000000000000000 ) | (uint64_t) (uint32_t) i;
        check( rocketnet_client_slots_assign( slots, id, 10000 + i, id, user_data ) == expected );
        check( rocketnet_client_slots_find( slots, id ) == expected );
    }

    check( rocketnet_client_slots_num_connected( slots ) == capacity );
    check( rocketnet_client_slots_num_free( slots ) == 0 );
    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_client_slots_full_capacity_reuse_and_full_width_ids );
