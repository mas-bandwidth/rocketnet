/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "sdk/rocketnet_connection_reset.h"
#include "sdk/rocketnet_client_protocol.h"
#include "sdk/rocketnet_server_protocol.h"
#include "sdk/rocketnet_client_slots.h"
#include "sdk/rocketnet_encryption_map.h"
#include "sdk/rocketnet_fragment.h"
#include "sdk/rocketnet_message_object.h"
#include "sdk/rocketnet_layers.h"

#include <stddef.h>
#include <string.h>

#define check rocketnet_unit_check

struct reset_field_t
{
    const char * name;
    size_t offset;
    size_t bytes;
    int reset;
};

#define RESET_FIELD( decision, field ) { #field, offsetof( rocketnet_client_protocol_t, field ), sizeof( ((rocketnet_client_protocol_t*)0)->field ), #decision[0] == 'R' },
static const reset_field_t client_protocol_fields[] =
{
    ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( RESET_FIELD )
};
#undef RESET_FIELD

static void connection_reset_inventory_covers_every_field()
{
    size_t last_end = 0;
    int reset_fields = 0;
    int keep_fields = 0;
    for ( size_t i = 0; i < sizeof(client_protocol_fields)/sizeof(client_protocol_fields[0]); i++ )
    {
        const reset_field_t & field = client_protocol_fields[i];
        check( field.name && field.bytes > 0 );
        check( field.offset >= last_end );
        check( field.offset + field.bytes <= sizeof(rocketnet_client_protocol_t) );
        last_end = field.offset + field.bytes;
        if ( field.reset ) reset_fields++; else keep_fields++;
    }
    // record_sequence is intentionally the last member. Appending a member
    // without adding an inventory row makes this witness fail.
    check( last_end == sizeof(rocketnet_client_protocol_t) );
    check( reset_fields == 33 );
    check( keep_fields == 8 );

    // Poison every RESET decision, preserve every KEEP decision, and compare
    // the result field-by-field with a freshly initialized protocol object.
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rocketnet_client_protocol_t value;
    rocketnet_client_protocol_init( &value, &counters );
    value.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED;
#define POISON_RESET( field ) memset( &value.field, 0xA5, sizeof(value.field) );
#define POISON_KEEP( field )
#define POISON_FIELD( decision, field ) POISON_##decision( field )
    ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( POISON_FIELD )
#undef POISON_FIELD
#undef POISON_KEEP
#undef POISON_RESET
    value.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED;
    const rocketnet_client_protocol_t before = value;
    rocketnet_client_connection_reset( &value );

    rocketnet_client_protocol_t fresh;
    rocketnet_client_protocol_init( &fresh, &counters );
#define CHECK_RESET( field ) check( memcmp( &value.field, &fresh.field, sizeof(value.field) ) == 0 );
#define CHECK_KEEP( field ) check( memcmp( &value.field, &before.field, sizeof(value.field) ) == 0 );
#define CHECK_FIELD( decision, field ) CHECK_##decision( field )
    ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( CHECK_FIELD )
#undef CHECK_FIELD
#undef CHECK_KEEP
#undef CHECK_RESET
}
REGISTER_UNIT_TEST( connection_reset_inventory_covers_every_field );

struct reset_server_fixture_t
{
    rocketnet_server_protocol_t server;
    rocketnet_counters_t counters;
    uint64_t connection_ids[2];
};

static void reset_server_setup( reset_server_fixture_t * f )
{
    memset( f, 0, sizeof(*f) );
    f->server.encryption_map = rocketnet_encryption_map_create( NULL, 4 );
    f->server.client_slots = rocketnet_client_slots_create( NULL, 2 );
    f->server.acks = rocketnet_server_acks_create( NULL, 2 );
    f->server.reassembly = rocketnet_reassembly_set_create( NULL, 2, 2, 1024, 4 );
    f->server.counters = &f->counters;
    f->server.connection_ids = f->connection_ids;
    check( f->server.encryption_map && f->server.client_slots && f->server.acks && f->server.reassembly );
}

static int reset_server_assign( reset_server_fixture_t * f, const char * address, uint64_t client_id )
{
    rocketnet_address_t a;
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, (int) client_id, sizeof(user_data) );
    check( rocketnet_address_parse( &a, address ) );
    const int mapping_index = rocketnet_encryption_map_add( f->server.encryption_map, &a, 1005.0 );
    check( mapping_index >= 0 );
    const int slot = rocketnet_client_slots_assign( f->server.client_slots, client_id, mapping_index, user_data );
    check( slot >= 0 );
    rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( f->server.encryption_map, mapping_index );
    mapping->client_index = slot;
    mapping->established = 1;
    mapping->send_sequence_session = 17;
    f->connection_ids[slot] = client_id + 100;
    return slot;
}

static void reset_server_teardown( reset_server_fixture_t * f )
{
    rocketnet_reassembly_set_destroy( f->server.reassembly );
    rocketnet_server_acks_destroy( f->server.acks );
    rocketnet_client_slots_destroy( f->server.client_slots );
    rocketnet_encryption_map_destroy( f->server.encryption_map );
}

static void connection_reset_server_slot_reuse_is_clean()
{
    reset_server_fixture_t f;
    reset_server_setup( &f );
    const int a = reset_server_assign( &f, "198.51.100.1:4000", 1 );
    f.server.acks->endpoints[a].highest_sequence = 99;
    f.server.acks->endpoints[a].rtt = 42.0f;
    f.server.reassembly->newest_completed[a] = 77;
    rocketnet_server_connection_reset( &f.server, a );
    check( !rocketnet_client_slots_connected( f.server.client_slots, a ) );
    check( f.connection_ids[a] == 0 );
    check( f.server.acks->endpoints[a].highest_sequence == 0 );
    check( f.server.acks->endpoints[a].rtt == 0.0f );
    check( f.server.reassembly->newest_completed[a] == 0 );
    const int b = reset_server_assign( &f, "198.51.100.2:4000", 2 );
    check( b == a );
    check( f.server.acks->endpoints[b].highest_sequence == 0 );
    check( f.server.reassembly->newest_completed[b] == 0 );
    reset_server_teardown( &f );
}
REGISTER_UNIT_TEST( connection_reset_server_slot_reuse_is_clean );

static void connection_reset_server_reset_touches_only_its_slot()
{
    reset_server_fixture_t f;
    reset_server_setup( &f );
    const int a = reset_server_assign( &f, "198.51.100.3:4000", 3 );
    const int b = reset_server_assign( &f, "198.51.100.4:4000", 4 );
    f.server.acks->endpoints[a].highest_sequence = 11;
    f.server.acks->endpoints[b].highest_sequence = 22;
    f.server.reassembly->newest_completed[a] = 33;
    f.server.reassembly->newest_completed[b] = 44;
    const uint64_t b_id = f.connection_ids[b];
    rocketnet_server_connection_reset( &f.server, a );
    check( !rocketnet_client_slots_connected( f.server.client_slots, a ) );
    check( rocketnet_client_slots_connected( f.server.client_slots, b ) );
    check( f.server.acks->endpoints[b].highest_sequence == 22 );
    check( f.server.reassembly->newest_completed[b] == 44 );
    check( f.connection_ids[b] == b_id );
    reset_server_teardown( &f );
}
REGISTER_UNIT_TEST( connection_reset_server_reset_touches_only_its_slot );

static void connection_reset_client_disconnect_is_clean()
{
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 2, 1024, 4 );
    check( client.reassembly );
    client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT;
    client.send_sequence_handshake = 7;
    client.send_sequence_session = 9;
    client.next_fragment_payload_id = 12;
    client.replay_session.highest_opened = 100;
    client.ack.highest_sequence = 101;
    client.num_acked = 2;
    client.reassembly->newest_completed[0] = 8;
    rocketnet_client_connection_reset( &client );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );
    check( client.send_sequence_handshake == 0 && client.send_sequence_session == 0 );
    check( client.next_fragment_payload_id == 1 );
    check( client.replay_session.highest_opened == 0 );
    check( client.ack.highest_sequence == 0 && client.num_acked == 0 );
    check( client.reassembly->newest_completed[0] == 0 );
    rocketnet_reassembly_set_destroy( client.reassembly );
}
REGISTER_UNIT_TEST( connection_reset_client_disconnect_is_clean );

static void connection_reset_each_state_leaves_a_clean_slot()
{
    static const int terminal[] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED,
        ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED,
    };
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    for ( size_t i = 0; i < sizeof(terminal)/sizeof(terminal[0]); i++ )
    {
        rocketnet_client_protocol_t client;
        rocketnet_client_protocol_init( &client, &counters );
        client.state = terminal[i];
        client.send_sequence_session = 123;
        client.ack.highest_received_sequence = 456;
        rocketnet_client_connection_reset( &client );
        check( client.state == terminal[i] );
        check( client.send_sequence_session == 0 );
        check( client.ack.highest_received_sequence == 0 );
    }
}
REGISTER_UNIT_TEST( connection_reset_each_state_leaves_a_clean_slot );

static void reset_link_update( void *, double ) {}
static int reset_link_payloads( void *, const int **, uint8_t * const **, const int ** ) { return 0; }
static int reset_link_acked( void *, const int **, const uint64_t ** ) { return 0; }
static int reset_link_max_payload( void * ) { return ROCKETNET_MAX_PAYLOAD_BYTES; }
static uint8_t * reset_link_start( void *, int, int, rocketnet_packet_context_t * ) { return NULL; }
static int reset_link_finish( void *, rocketnet_packet_context_t *, int ) { return 0; }
static void reset_link_abort( void *, rocketnet_packet_context_t * ) {}
static void reset_link_send( void * ) {}
static uint64_t reset_link_connection( void *, int ) { return 0; }
static void reset_link_disconnect( void *, int ) {}
static int reset_link_state( void * ) { return ROCKETNET_SERVER_TRANSPORT_RUNNING; }

static void connection_reset_disconnect_reason_kept_until_next_connection()
{
    rn_message_link_t link;
    memset( &link, 0, sizeof(link) );
    link.update = reset_link_update;
    link.payloads = reset_link_payloads;
    link.acked = reset_link_acked;
    link.max_payload_bytes = reset_link_max_payload;
    link.start_packet = reset_link_start;
    link.finish_packet = reset_link_finish;
    link.abort_packet = reset_link_abort;
    link.send_packets = reset_link_send;
    link.connection = reset_link_connection;
    link.disconnect = reset_link_disconnect;
    link.state = reset_link_state;
    rn_message_object_t * server = rn_message_object_create( NULL, &link, 1, 2, NULL, NULL, 0,
                                                             ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS, 1000.0 );
    check( server );
    server->connections[0].disconnect_reason = ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT;
    server->connections[1].disconnect_reason = ROCKETNET_MESSAGE_ERROR_NONE;
    rocketnet_server_connection_reset( server, 0 );
    check( server->connections[0].disconnect_reason == ROCKETNET_MESSAGE_ERROR_RELIABLE_TIMEOUT );
    check( server->connections[1].disconnect_reason == ROCKETNET_MESSAGE_ERROR_NONE );
    // The Observe/establish transition clears the retained reason.
    server->connections[0].disconnect_reason = ROCKETNET_MESSAGE_ERROR_NONE;
    check( server->connections[0].disconnect_reason == ROCKETNET_MESSAGE_ERROR_NONE );
    rn_message_object_destroy( server );
}
REGISTER_UNIT_TEST( connection_reset_disconnect_reason_kept_until_next_connection );
