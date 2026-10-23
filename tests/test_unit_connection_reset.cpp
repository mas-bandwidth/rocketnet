/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_connection_reset.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_server_replay_marks.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_fragment.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_client_io.h"
#include "../sdk/rocketnet_driver.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// The loopback pair names the in-memory loopback transport in its transport configs;
// a caller's ROCKETNET_SERVER_TRANSPORT / ROCKETNET_CLIENT_TRANSPORT pin wins over
// the struct by design, so a CI leg pinned to a real transport would replace it. Each
// create sets its pin aside and puts it back, as test_unit_status_server_transport_create_ex.cpp does.
struct reset_env_pin_t
{
    const char * name;
    bool was_set;
    char value[256];
};

static void reset_env_pin_set_aside( reset_env_pin_t * pin, const char * name )
{
    pin->name = name;
    pin->was_set = rocketnet_test_getenv( name, pin->value, sizeof(pin->value) ) != 0;
#if defined(_WIN32)
    _putenv_s( name, "" );
#else
    unsetenv( name );
#endif
}

static void reset_env_pin_put_back( const reset_env_pin_t * pin )
{
    if ( !pin->was_set )
        return;
#if defined(_WIN32)
    _putenv_s( pin->name, pin->value );
#else
    setenv( pin->name, pin->value, 1 );
#endif
}

#define check rocketnet_unit_check

#define CLIENT_PROTOCOL_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_client_protocol_t, field, #decision[0] == 'R' ),
static const rocketnet_reset_member_t client_protocol_fields[] =
{
    ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( CLIENT_PROTOCOL_DECISION )
};
#undef CLIENT_PROTOCOL_DECISION

#define CLIENT_PROTOCOL_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_client_protocol_t, field, -1 ),
static const rocketnet_reset_member_t client_protocol_layout[] =
{
    ROCKETNET_CLIENT_PROTOCOL_LAYOUT_FIELDS( CLIENT_PROTOCOL_MEMBER )
};
#undef CLIENT_PROTOCOL_MEMBER

#define MAPPING_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_encryption_mapping_t, field, #decision[0] == 'R' ),
static const rocketnet_reset_member_t encryption_mapping_fields[] =
{
    ROCKETNET_ENCRYPTION_MAPPING_CONNECTION_FIELDS( MAPPING_DECISION )
};
#undef MAPPING_DECISION

#define MAPPING_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_encryption_mapping_t, field, -1 ),
static const rocketnet_reset_member_t encryption_mapping_layout[] =
{
    ROCKETNET_ENCRYPTION_MAPPING_LAYOUT_FIELDS( MAPPING_MEMBER )
};
#undef MAPPING_MEMBER

#define CONTROL_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rocketnet_encryption_mapping_control_t, field, #decision[0] == 'R' ),
static const rocketnet_reset_member_t encryption_mapping_control_fields[] =
{
    ROCKETNET_ENCRYPTION_MAPPING_CONTROL_CONNECTION_FIELDS( CONTROL_DECISION )
};
#undef CONTROL_DECISION

#define CONTROL_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rocketnet_encryption_mapping_control_t, field, -1 ),
static const rocketnet_reset_member_t encryption_mapping_control_layout[] =
{
    ROCKETNET_ENCRYPTION_MAPPING_CONTROL_LAYOUT_FIELDS( CONTROL_MEMBER )
};
#undef CONTROL_MEMBER

#define ENDPOINT_DECISION( decision, field ) ROCKETNET_RESET_MEMBER( rn_endpoint_t, field, #decision[0] == 'R' ),
static const rocketnet_reset_member_t endpoint_fields[] =
{
    ROCKETNET_ENDPOINT_CONNECTION_FIELDS( ENDPOINT_DECISION )
};
#undef ENDPOINT_DECISION

#define ENDPOINT_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rn_endpoint_t, field, -1 ),
static const rocketnet_reset_member_t endpoint_layout[] =
{
    RN_ENDPOINT_LAYOUT_FIELDS( ENDPOINT_MEMBER )
};
#undef ENDPOINT_MEMBER

// rn_message_connection_t is not standard layout (it derives from
// rn_endpoint_t), so offsetof is only conditionally supported on it; each
// member's offset is measured on a probe object instead, which is all the
// reset audit compares
static rn_message_connection_t reset_message_connection_probe;
#define MESSAGE_FIELD( decision, field ) { #field, (size_t) ( (const char *) &reset_message_connection_probe.field - (const char *) &reset_message_connection_probe ), sizeof( ((rn_message_connection_t*)0)->field ), alignof(decltype(((rn_message_connection_t*)0)->field)), #decision[0] == 'R' },
static const rocketnet_reset_member_t message_connection_fields[] =
{
    ROCKETNET_MESSAGE_CONNECTION_FIELDS( MESSAGE_FIELD )
};
#undef MESSAGE_FIELD

#define HOT_FIELD( decision, field ) ROCKETNET_RESET_MEMBER( rn_message_connection_hot_t, field, #decision[0] == 'R' ),
static const rocketnet_reset_member_t message_connection_hot_fields[] =
{
    ROCKETNET_MESSAGE_CONNECTION_HOT_FIELDS( HOT_FIELD )
};
#undef HOT_FIELD

#define RESET_COUNT( array ) ( sizeof(array) / sizeof((array)[0]) )

// one inventory field walk: a failure names <struct>.<member>
static void reset_walk_passes( bool passed, const char * failure )
{
    if ( !passed )
    {
        printf( "reset inventory: %s\n", failure );
        fflush( stdout );
    }
    check( passed );
}

// one RESET or KEEP comparison after a reset: a failure names <struct>.<member>
static void reset_field_holds( const char * structure, const char * member, const char * decision,
                               const void * value, const void * expected, size_t bytes )
{
    const bool holds = memcmp( value, expected, bytes ) == 0;
    if ( !holds )
    {
        printf( "reset %s field %s.%s does not hold after the reset\n", decision, structure, member );
        fflush( stdout );
    }
    check( holds );
}

static void test_connection_reset_inventory_covers_every_field()
{
    char failure[256];

    reset_walk_passes( rocketnet_reset_inventory_matches( "rocketnet_client_protocol_t",
        client_protocol_layout, RESET_COUNT( client_protocol_layout ),
        client_protocol_fields, RESET_COUNT( client_protocol_fields ), 0,
        sizeof(rocketnet_client_protocol_t), alignof(rocketnet_client_protocol_t), failure, sizeof(failure) ), failure );
    int reset_fields = 0;
    int keep_fields = 0;
    for ( size_t i = 0; i < RESET_COUNT( client_protocol_fields ); i++ )
    {
        if ( client_protocol_fields[i].reset ) reset_fields++; else keep_fields++;
    }
    check( reset_fields == 33 );
    check( keep_fields == 8 );

    reset_walk_passes( rocketnet_reset_inventory_matches( "rocketnet_encryption_mapping_t",
        encryption_mapping_layout, RESET_COUNT( encryption_mapping_layout ),
        encryption_mapping_fields, RESET_COUNT( encryption_mapping_fields ), 0,
        sizeof(rocketnet_encryption_mapping_t), alignof(rocketnet_encryption_mapping_t), failure, sizeof(failure) ), failure );
    reset_walk_passes( rocketnet_reset_inventory_matches( "rocketnet_encryption_mapping_control_t",
        encryption_mapping_control_layout, RESET_COUNT( encryption_mapping_control_layout ),
        encryption_mapping_control_fields, RESET_COUNT( encryption_mapping_control_fields ), 0,
        sizeof(rocketnet_encryption_mapping_control_t), alignof(rocketnet_encryption_mapping_control_t), failure, sizeof(failure) ), failure );
    reset_walk_passes( rocketnet_reset_inventory_matches( "rn_endpoint_t",
        endpoint_layout, RESET_COUNT( endpoint_layout ), endpoint_fields, RESET_COUNT( endpoint_fields ), 0,
        sizeof(rn_endpoint_t), alignof(rn_endpoint_t), failure, sizeof(failure) ), failure );
    reset_walk_passes( rocketnet_client_transport_inventory_matches_for_test( failure, sizeof(failure) ), failure );
    reset_walk_passes( rocketnet_client_slots_inventory_matches_for_test( failure, sizeof(failure) ), failure );
    reset_walk_passes( rocketnet_server_replay_marks_inventory_matches_for_test( failure, sizeof(failure) ), failure );

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
#define CHECK_RESET( field ) reset_field_holds( "rocketnet_client_protocol_t", #field, "RESET", &value.field, &fresh.field, sizeof(value.field) );
#define CHECK_KEEP( field ) reset_field_holds( "rocketnet_client_protocol_t", #field, "KEEP", &value.field, &before.field, sizeof(value.field) );
#define CHECK_FIELD( decision, field ) CHECK_##decision( field )
    ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( CHECK_FIELD )
#undef CHECK_FIELD
#undef CHECK_KEEP
#undef CHECK_RESET

    check( rocketnet_client_slots_reset_witness_for_test() );
    check( rocketnet_server_replay_marks_reset_witness_for_test() );

    // A mapping removal and same-index reuse must reproduce its exact fresh
    // initializer for every mapping field, including the nonzero sequence and
    // fragment seeds and the -1 client index.
    rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 1 );
    check( map );
    rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "198.51.100.90:4090" ) );
    const int mapping_index = rocketnet_encryption_map_add( map, &address, 1005.0 );
    check( mapping_index == 0 );
    rocketnet_encryption_mapping_t fresh_mapping = *rocketnet_encryption_map_entry( map, mapping_index );
    rocketnet_encryption_mapping_control_t fresh_control = *rocketnet_encryption_map_control( map, mapping_index );
#define POISON_MAPPING_RESET( field ) memset( &rocketnet_encryption_map_entry( map, mapping_index )->field, 0xA5, sizeof(rocketnet_encryption_map_entry( map, mapping_index )->field) );
#define POISON_MAPPING_KEEP( field )
#define POISON_MAPPING( decision, field ) POISON_MAPPING_##decision( field )
    ROCKETNET_ENCRYPTION_MAPPING_CONNECTION_FIELDS( POISON_MAPPING )
#undef POISON_MAPPING
#undef POISON_MAPPING_KEEP
#undef POISON_MAPPING_RESET
#define POISON_CONTROL_RESET( field ) memset( &rocketnet_encryption_map_control( map, mapping_index )->field, 0xA5, sizeof(rocketnet_encryption_map_control( map, mapping_index )->field) );
#define POISON_CONTROL_KEEP( field )
#define POISON_CONTROL( decision, field ) POISON_CONTROL_##decision( field )
    ROCKETNET_ENCRYPTION_MAPPING_CONTROL_CONNECTION_FIELDS( POISON_CONTROL )
#undef POISON_CONTROL
#undef POISON_CONTROL_KEEP
#undef POISON_CONTROL_RESET
    rocketnet_encryption_map_remove( map, mapping_index );
    check( rocketnet_encryption_map_add( map, &address, 1005.0 ) == mapping_index );
    rocketnet_encryption_mapping_t * reused_mapping = rocketnet_encryption_map_entry( map, mapping_index );
#define CHECK_MAPPING_RESET( field ) reset_field_holds( "rocketnet_encryption_mapping_t", #field, "RESET", &reused_mapping->field, &fresh_mapping.field, sizeof(reused_mapping->field) );
#define CHECK_MAPPING_KEEP( field )
#define CHECK_MAPPING( decision, field ) CHECK_MAPPING_##decision( field )
    ROCKETNET_ENCRYPTION_MAPPING_CONNECTION_FIELDS( CHECK_MAPPING )
#undef CHECK_MAPPING
#undef CHECK_MAPPING_KEEP
#undef CHECK_MAPPING_RESET
    rocketnet_encryption_mapping_control_t * reused_control = rocketnet_encryption_map_control( map, mapping_index );
#define CHECK_CONTROL_RESET( field ) reset_field_holds( "rocketnet_encryption_mapping_control_t", #field, "RESET", &reused_control->field, &fresh_control.field, sizeof(reused_control->field) );
#define CHECK_CONTROL_KEEP( field )
#define CHECK_CONTROL( decision, field ) CHECK_CONTROL_##decision( field )
    ROCKETNET_ENCRYPTION_MAPPING_CONTROL_CONNECTION_FIELDS( CHECK_CONTROL )
#undef CHECK_CONTROL
#undef CHECK_CONTROL_KEEP
#undef CHECK_CONTROL_RESET
    rocketnet_encryption_map_destroy( map );

    // The endpoint and message connection are opaque to callers but visible
    // to this internal test. Capture a fresh initializer over one region,
    // poison every RESET field, then require byte equality after the central
    // message reset while every KEEP byte and its allocation stays unchanged.
    rn_message_link_t link;
    memset( &link, 0, sizeof(link) );
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                             ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 1000.0 );
    check( object );
    rn_message_connection_t * connection = &object->connections[0];
    check( connection->hot == object->hot_connections );
#define HOT_MEMBER( field, declaration ) ROCKETNET_RESET_MEMBER( rn_message_connection_hot_t, field, -1 ),
    const rocketnet_reset_member_t message_connection_hot_layout[] = { RN_MESSAGE_CONNECTION_HOT_LAYOUT_FIELDS( HOT_MEMBER ) };
#undef HOT_MEMBER
    reset_walk_passes( rocketnet_reset_inventory_matches( "rn_message_connection_hot_t",
        message_connection_hot_layout, RESET_COUNT( message_connection_hot_layout ),
        message_connection_hot_fields, RESET_COUNT( message_connection_hot_fields ), 0,
        sizeof(rn_message_connection_hot_t), alignof(rn_message_connection_hot_t), failure, sizeof(failure) ), failure );
#define MESSAGE_MEMBER( field, declaration ) { #field, (size_t) ( (const uint8_t *) &connection->field - (const uint8_t *) connection ), \
    sizeof( connection->field ), alignof( decltype( connection->field ) ), -1 },
    const rocketnet_reset_member_t message_connection_layout[] = { RN_MESSAGE_CONNECTION_LAYOUT_FIELDS( MESSAGE_MEMBER ) };
#undef MESSAGE_MEMBER
    reset_walk_passes( rocketnet_reset_inventory_matches( "rn_message_connection_t",
        message_connection_layout, RESET_COUNT( message_connection_layout ),
        message_connection_fields, RESET_COUNT( message_connection_fields ), sizeof(rn_endpoint_t),
        sizeof(rn_message_connection_t), alignof(rn_message_connection_t), failure, sizeof(failure) ), failure );
    rocketnet_client_connection_reset( object );
    const rn_message_connection_t fresh_connection = *connection;
    const rn_endpoint_t & fresh_endpoint = fresh_connection;
    const rn_message_connection_hot_t fresh_hot = *connection->hot;
    connection->disconnect_reason = ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT;
    memset( connection->hot->views, 0xA5, connection->region->delivery_entries );
    memset( connection->hot->cursors, 0xA5,
            sizeof(uint32_t) * (size_t) connection->region->num_channels );
#define POISON_ENDPOINT_RESET( field ) memset( &connection->field, 0xA5, sizeof(connection->field) );
#define POISON_ENDPOINT_KEEP( field )
#define POISON_ENDPOINT( decision, field ) POISON_ENDPOINT_##decision( field )
    ROCKETNET_ENDPOINT_CONNECTION_FIELDS( POISON_ENDPOINT )
#undef POISON_ENDPOINT
#undef POISON_ENDPOINT_KEEP
#undef POISON_ENDPOINT_RESET
#define POISON_MESSAGE_RESET( field ) memset( &connection->field, 0xA5, sizeof(connection->field) );
#define POISON_MESSAGE_KEEP( field )
#define POISON_MESSAGE( decision, field ) POISON_MESSAGE_##decision( field )
    ROCKETNET_MESSAGE_CONNECTION_FIELDS( POISON_MESSAGE )
#undef POISON_MESSAGE
#undef POISON_MESSAGE_KEEP
#undef POISON_MESSAGE_RESET
    const rn_message_connection_t before_connection = *connection;
#define POISON_HOT_RESET( field ) memset( &connection->hot->field, 0xA5, sizeof(connection->hot->field) );
#define POISON_HOT_KEEP( field )
#define POISON_HOT( decision, field ) POISON_HOT_##decision( field )
    ROCKETNET_MESSAGE_CONNECTION_HOT_FIELDS( POISON_HOT )
#undef POISON_HOT
#undef POISON_HOT_KEEP
#undef POISON_HOT_RESET
    const rn_message_connection_hot_t before_hot = *connection->hot;
    rocketnet_client_connection_reset( object );
#define CHECK_ENDPOINT_RESET( field ) reset_field_holds( "rn_endpoint_t", #field, "RESET", &connection->field, &fresh_endpoint.field, sizeof(connection->field) );
#define CHECK_ENDPOINT_KEEP( field ) reset_field_holds( "rn_endpoint_t", #field, "KEEP", &connection->field, &before_connection.field, sizeof(connection->field) );
#define CHECK_ENDPOINT( decision, field ) CHECK_ENDPOINT_##decision( field )
    ROCKETNET_ENDPOINT_CONNECTION_FIELDS( CHECK_ENDPOINT )
#undef CHECK_ENDPOINT
#undef CHECK_ENDPOINT_KEEP
#undef CHECK_ENDPOINT_RESET
#define CHECK_MESSAGE_RESET( field ) reset_field_holds( "rn_message_connection_t", #field, "RESET", &connection->field, &fresh_connection.field, sizeof(connection->field) );
#define CHECK_MESSAGE_KEEP( field ) reset_field_holds( "rn_message_connection_t", #field, "KEEP", &connection->field, &before_connection.field, sizeof(connection->field) );
#define CHECK_MESSAGE( decision, field ) CHECK_MESSAGE_##decision( field )
    ROCKETNET_MESSAGE_CONNECTION_FIELDS( CHECK_MESSAGE )
#undef CHECK_MESSAGE
#undef CHECK_MESSAGE_KEEP
#undef CHECK_MESSAGE_RESET
#define CHECK_HOT_RESET( field ) reset_field_holds( "rn_message_connection_hot_t", #field, "RESET", &connection->hot->field, &fresh_hot.field, sizeof(connection->hot->field) );
#define CHECK_HOT_KEEP( field ) reset_field_holds( "rn_message_connection_hot_t", #field, "KEEP", &connection->hot->field, &before_hot.field, sizeof(connection->hot->field) );
#define CHECK_HOT( decision, field ) CHECK_HOT_##decision( field )
    ROCKETNET_MESSAGE_CONNECTION_HOT_FIELDS( CHECK_HOT )
#undef CHECK_HOT
#undef CHECK_HOT_KEEP
#undef CHECK_HOT_RESET
    for ( uint32_t i = 0; i < connection->region->delivery_entries; i++ )
        check( connection->hot->views[i] == 0 );
    for ( uint32_t i = 0; i < connection->region->num_channels; i++ )
        check( connection->hot->cursors[i] == 0 );
    rn_message_object_destroy( object );
}
REGISTER_UNIT_TEST( test_connection_reset_inventory_covers_every_field );

extern const rocketnet_transport_backend_t rocketnet_loopback_transport;

enum { RESET_LOOPBACK_MAX_CLIENTS = 2 };

struct reset_loopback_t
{
    rocketnet_message_server_t * server;
    rocketnet_message_client_t * clients[RESET_LOOPBACK_MAX_CLIENTS];
    double now;
};

struct reset_loopback_keys_t
{
    int initialized;
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static const reset_loopback_keys_t * reset_loopback_keys()
{
    static reset_loopback_keys_t keys;
    if ( !keys.initialized )
    {
        uint8_t seed[32];
        for ( int i = 0; i < 32; i++ )
            seed[i] = (uint8_t) ( 0x5A ^ i );
        check( rocketnet_generate_backend_keypair_from_seed( seed, keys.backend_public, keys.backend_secret ) == ROCKETNET_STATUS_OK );
        check( rocketnet_generate_server_keypair( keys.server_public, keys.server_secret ) == ROCKETNET_STATUS_OK );
        keys.initialized = 1;
    }
    return &keys;
}

static const uint64_t RESET_LOOPBACK_PROTOCOL_ID = 0x5245534554ULL;

static rocketnet_message_server_t * reset_loopback_server_create( double now, const rocketnet_message_config_t * message_config = NULL )
{
    const reset_loopback_keys_t * keys = reset_loopback_keys();
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.protocol_id = RESET_LOOPBACK_PROTOCOL_ID;
    config.max_clients = RESET_LOOPBACK_MAX_CLIENTS;
    memcpy( config.backend_public_key, keys->backend_public, sizeof(config.backend_public_key) );
    memcpy( config.server_public_key, keys->server_public, sizeof(config.server_public_key) );
    memcpy( config.server_secret_key, keys->server_secret, sizeof(config.server_secret_key) );
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    int status = -1;
    reset_env_pin_t pin;
    reset_env_pin_set_aside( &pin, "ROCKETNET_SERVER_TRANSPORT" );
    rocketnet_server_transport_t * transport = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, now, &status );
    reset_env_pin_put_back( &pin );
    check( transport && status == ROCKETNET_STATUS_OK );
    rocketnet_server_transport_set_test_timestamp( transport, 1000 );
    rn_message_link_t link;
    rn_message_server_link( &link, transport );
    return (rocketnet_message_server_t *) rn_message_object_create( NULL, &link, 1, RESET_LOOPBACK_MAX_CLIENTS,
                                                                    message_config, NULL, 0,
                                                                    ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, now );
}

static rocketnet_message_client_t * reset_loopback_client_create( rocketnet_message_server_t * server, uint64_t client_id,
                                                                    const rocketnet_message_config_t * message_config = NULL )
{
    const reset_loopback_keys_t * keys = reset_loopback_keys();
    char address[64];
    snprintf( address, sizeof(address), "127.0.0.1:%u",
              (unsigned) rocketnet_server_transport_port( rocketnet_message_server_transport( server ) ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public,
                                                     RESET_LOOPBACK_PROTOCOL_ID, addresses, 1,
                                                     client_id, 1000, 1030, 10, NULL,
                                                     token, sizeof(token) ) == ROCKETNET_STATUS_OK );
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    snprintf( config.transport, sizeof(config.transport), "%s", "loopback" );
    config.step_driven = 1;
    reset_env_pin_t pin;
    reset_env_pin_set_aside( &pin, "ROCKETNET_CLIENT_TRANSPORT" );
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_at_timestamp(
        NULL, token, &config, 0.0, 1000, rocketnet_loopback_transport.client, NULL );
    reset_env_pin_put_back( &pin );
    check( transport );
    rn_message_link_t link;
    rn_message_client_link( &link, transport );
    return (rocketnet_message_client_t *) rn_message_object_create( NULL, &link, 0, 1, message_config, NULL, 0,
                                                                    ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 0.0 );
}

// every delivery is received before the next update (standard/MESSAGES.md
// m1.6: one left waiting ends the connection). A scenario that does not read
// its deliveries itself drains them here.

static void reset_loopback_drain_client( rocketnet_message_client_t * client )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    while ( rocketnet_message_client_receive( client, NULL, &data, &bytes ) )
        rocketnet_message_client_release( client, data );
}

static void reset_loopback_drain_server( rocketnet_message_server_t * server )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    while ( rocketnet_message_server_receive( server, NULL, NULL, &data, &bytes ) )
        rocketnet_message_server_release( server, data );
}

static void reset_loopback_step( reset_loopback_t * f )
{
    f->now += 0.01;
    for ( int i = 0; i < RESET_LOOPBACK_MAX_CLIENTS; i++ )
    {
        if ( !f->clients[i] )
            continue;
        check( rocketnet_message_client_update( f->clients[i], f->now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f->clients[i] );
    }
    check( rocketnet_message_server_update( f->server, f->now ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( f->server );
}

static int reset_loopback_connect( reset_loopback_t * f, int which, uint64_t client_id,
                                   const rocketnet_message_config_t * message_config = NULL )
{
    f->clients[which] = reset_loopback_client_create( f->server, client_id, message_config );
    for ( int i = 0; i < 400; i++ )
    {
        reset_loopback_step( f );
        if ( rocketnet_message_client_state( f->clients[which] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            const int slot = rocketnet_client_transport_client_index( rocketnet_message_client_transport( f->clients[which] ) );
            if ( slot >= 0 && slot < RESET_LOOPBACK_MAX_CLIENTS &&
                 rocketnet_message_server_client_connected( f->server, slot ) )
                return slot;
        }
    }
    check( false );
    return -1;
}

static int reset_loopback_advance_client_to_state( reset_loopback_t * f, int which, int target )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_message_client_state( f->clients[which] ) == target )
            return 1;
        f->now += 0.01;
        check( rocketnet_message_client_update( f->clients[which], f->now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f->clients[which] );
        if ( rocketnet_message_client_state( f->clients[which] ) == target )
            return 1;
        check( rocketnet_message_server_update( f->server, f->now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f->server );
    }
    return 0;
}

static void reset_loopback_wait_for_server_connection_state_clean( reset_loopback_t * f )
{
    for ( int i = 0; i < 800; i++ )
    {
        if ( rocketnet_server_transport_connection_state_clean_for_test(
                rocketnet_message_server_transport( f->server ) ) )
            return;
        f->now += 0.01;
        check( rocketnet_message_server_update( f->server, f->now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f->server );
    }
    check( false );
}

static void reset_loopback_destroy( reset_loopback_t * f )
{
    for ( int i = 0; i < RESET_LOOPBACK_MAX_CLIENTS; i++ )
        if ( f->clients[i] )
            rn_test_message_client_stop_and_destroy( f->clients[i] );
    rn_test_message_server_stop_and_destroy( f->server );
}

static void reset_loopback_slot_reuse_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    const int a_slot = reset_loopback_connect( &f, 0, 0xA001 );

    uint8_t fragment[4096];
    memset( fragment, 0xA1, sizeof(fragment) );
    const uint32_t a_receipt = rocketnet_message_client_send( f.clients[0], 0,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, fragment, sizeof(fragment) );
    check( a_receipt != 0 );
    check( rocketnet_message_client_send( f.clients[0], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
        fragment, 512 ) != 0 );
    check( rocketnet_message_server_send( f.server, a_slot, 1,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, fragment, sizeof(fragment) ) != 0 );
    reset_loopback_step( &f );
    reset_loopback_drain_client( f.clients[0] );
    rocketnet_server_transport_poison_slot_for_test( rocketnet_message_server_transport( f.server ), a_slot );

    rn_message_object_kick( &f.server->object, a_slot, ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( rocketnet_message_server_disconnect_reason( f.server, a_slot ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    reset_loopback_step( &f );
    reset_loopback_drain_client( f.clients[0] );
    check( !rocketnet_message_server_client_connected( f.server, a_slot ) );
    reset_loopback_step( &f );
    check( rocketnet_message_client_state( f.clients[0] ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( rocketnet_client_transport_reset_witness_for_test( rocketnet_message_client_transport( f.clients[0] ) ) );

    rn_test_message_client_stop_and_destroy( f.clients[0] );
    f.clients[0] = NULL;
    const int b_slot = reset_loopback_connect( &f, 1, 0xB001 );
    check( b_slot == a_slot );
    check( rocketnet_message_server_disconnect_reason( f.server, b_slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    rocketnet_server_transport_client_stats_t b_stats;
    memset( &b_stats, 0, sizeof(b_stats) );
    rocketnet_server_transport_client_stats( rocketnet_message_server_transport( f.server ), b_slot, &b_stats );
    check( b_stats.packets_sent != 0xA5A5A5A5A5A5A5A5ULL );
    check( b_stats.packets_received != 0xA5A5A5A5A5A5A5A5ULL );
    check( b_stats.bytes_sent != 0xA5A5A5A5A5A5A5A5ULL );
    check( b_stats.bytes_received != 0xA5A5A5A5A5A5A5A5ULL );
    const uint8_t * stale = NULL;
    int stale_bytes = 0;
    check( !rocketnet_message_server_receive( f.server, NULL, NULL, &stale, &stale_bytes ) );
    check( !rocketnet_message_client_receive( f.clients[1], NULL, &stale, &stale_bytes ) );
    rocketnet_message_receipt_t stale_receipts[8];
    check( rocketnet_message_client_receipts( f.clients[1], stale_receipts, 8 ) == 0 );

    const uint8_t c2s[] = { 'B', '-', 'c', '2', 's' };
    const uint8_t s2c[] = { 'B', '-', 's', '2', 'c' };
    const uint32_t b_receipt = rocketnet_message_client_send( f.clients[1], 0,
        ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, c2s, sizeof(c2s) );
    check( b_receipt != 0 );
    check( rocketnet_message_server_send( f.server, b_slot, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
        s2c, sizeof(s2c) ) != 0 );

    int got_c2s = 0, got_s2c = 0, got_receipt = 0;
    for ( int i = 0; i < 400 && !( got_c2s && got_s2c && got_receipt ); i++ )
    {
        reset_loopback_step( &f );
        const uint8_t * data = NULL;
        int bytes = 0;
        int from = -1;
        int channel = -1;
        if ( rocketnet_message_server_receive( f.server, &from, &channel, &data, &bytes ) )
        {
            check( from == b_slot && channel == 0 );
            check( bytes == (int) sizeof(c2s) && memcmp( data, c2s, sizeof(c2s) ) == 0 );
            rocketnet_message_server_release( f.server, data );
            got_c2s = 1;
        }
        if ( rocketnet_message_client_receive( f.clients[1], &channel, &data, &bytes ) )
        {
            check( channel == 1 );
            check( bytes == (int) sizeof(s2c) && memcmp( data, s2c, sizeof(s2c) ) == 0 );
            rocketnet_message_client_release( f.clients[1], data );
            got_s2c = 1;
        }
        rocketnet_message_receipt_t receipts[8];
        const int n = rocketnet_message_client_receipts( f.clients[1], receipts, 8 );
        check( n >= 0 && n <= 8 );
        for ( int r = 0; r < n; r++ )
        {
            check( receipts[r].serial == b_receipt );
            if ( receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED )
                got_receipt = 1;
        }
    }
    check( got_c2s && got_s2c && got_receipt );
    reset_loopback_destroy( &f );
}

static void reset_loopback_other_slot_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    const int a = reset_loopback_connect( &f, 0, 0xA002 );
    const int b = reset_loopback_connect( &f, 1, 0xB002 );
    check( a != b );
    const uint8_t payload[] = { 's', 'u', 'r', 'v', 'i', 'v', 'e' };
    check( rocketnet_message_client_send( f.clients[1], 2, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
        payload, sizeof(payload) ) != 0 );
    rn_message_object_kick( &f.server->object, a, ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    int delivered = 0;
    for ( int i = 0; i < 400 && !delivered; i++ )
    {
        reset_loopback_step( &f );
        const uint8_t * data = NULL;
        int bytes = 0;
        int from = -1;
        int channel = -1;
        if ( rocketnet_message_server_receive( f.server, &from, &channel, &data, &bytes ) )
        {
            check( from == b && channel == 2 );
            check( bytes == (int) sizeof(payload) && memcmp( data, payload, sizeof(payload) ) == 0 );
            rocketnet_message_server_release( f.server, data );
            delivered = 1;
        }
    }
    check( !rocketnet_message_server_client_connected( f.server, a ) );
    check( rocketnet_message_server_client_connected( f.server, b ) );
    check( delivered );
    reset_loopback_destroy( &f );
}

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
    f->server.reassembly = rocketnet_reassembly_set_create( NULL, 2, 2, 8192, 4 );
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
    mapping->control->client_index = slot;
    mapping->control->established = 1;
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


static void reset_client_transport_clean( rocketnet_client_transport_t * transport )
{
    char failure[256];
    const bool clean = rocketnet_client_transport_connection_state_clean_for_test( transport, failure, sizeof(failure) );
    if ( !clean )
    {
        printf( "reset: %s\n", failure );
        fflush( stdout );
    }
    check( clean );
}

static void reset_server_slot_clean( rocketnet_message_server_t * server, int client_index )
{
    char failure[256];
    const bool clean = rocketnet_server_transport_slot_clean_for_test( rocketnet_message_server_transport( server ),
                                                                       client_index, failure, sizeof(failure) );
    if ( !clean )
    {
        printf( "reset: %s\n", failure );
        fflush( stdout );
    }
    check( clean );
}

// The server's idle sweep ends a slot whose client went silent: the
// transport's own timeout (rocketnet_server_protocol_update), not a message
// layer kick. The slot is clean the moment the sweep ends it, and the next
// client given that slot index connects and delivers both ways.
static void reset_loopback_server_sweep_timeout_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    const int a_slot = reset_loopback_connect( &f, 0, 0xA003 );
    const uint8_t a_payload[] = { 'A', '-', 'i', 'd', 'l', 'e' };
    check( rocketnet_message_client_send( f.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, a_payload, sizeof(a_payload) ) != 0 );
    check( rocketnet_message_server_send( f.server, a_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, a_payload, sizeof(a_payload) ) != 0 );
    for ( int i = 0; i < 40; i++ )
    {
        reset_loopback_step( &f );
        reset_loopback_drain_server( f.server );
        reset_loopback_drain_client( f.clients[0] );
    }

    // the client goes silent: only the server runs from here
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( f.server );
    const uint64_t timed_out_before = rocketnet_server_transport_counters( transport )->values[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];
    for ( int i = 0; i < 400 && rocketnet_server_transport_is_client_connected( transport, a_slot ); i++ )
    {
        f.now += 0.05;
        check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f.server );
    }
    check( !rocketnet_server_transport_is_client_connected( transport, a_slot ) );
    check( rocketnet_server_transport_counters( transport )->values[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out_before + 1 );
    reset_server_slot_clean( f.server, a_slot );

    rn_test_message_client_stop_and_destroy( f.clients[0] );
    f.clients[0] = NULL;
    const int b_slot = reset_loopback_connect( &f, 1, 0xB003 );
    check( b_slot == a_slot );
    const uint8_t c2s[] = { 'B', '-', 'u', 'p' };
    const uint8_t s2c[] = { 'B', '-', 'd', 'n' };
    check( rocketnet_message_client_send( f.clients[1], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, c2s, sizeof(c2s) ) != 0 );
    check( rocketnet_message_server_send( f.server, b_slot, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, s2c, sizeof(s2c) ) != 0 );
    int got_c2s = 0, got_s2c = 0;
    for ( int i = 0; i < 400 && !( got_c2s && got_s2c ); i++ )
    {
        reset_loopback_step( &f );
        const uint8_t * data = NULL;
        int bytes = 0;
        {
            int from = -1;
            int channel = -1;
            if ( rocketnet_message_server_receive( f.server, &from, &channel, &data, &bytes ) )
            {
                check( from == b_slot );
                check( channel == 1 && bytes == (int) sizeof(c2s) && memcmp( data, c2s, sizeof(c2s) ) == 0 );
                rocketnet_message_server_release( f.server, data );
                got_c2s = 1;
            }
            if ( rocketnet_message_client_receive( f.clients[1], &channel, &data, &bytes ) )
            {
                check( channel == 1 && bytes == (int) sizeof(s2c) && memcmp( data, s2c, sizeof(s2c) ) == 0 );
                rocketnet_message_client_release( f.clients[1], data );
                got_s2c = 1;
            }
        }
    }
    check( got_c2s && got_s2c );
    reset_loopback_destroy( &f );
}

static void test_connection_reset_server_slot_reuse_is_clean()
{
    reset_loopback_slot_reuse_scenario();
    reset_loopback_server_sweep_timeout_scenario();
    reset_server_fixture_t f;
    reset_server_setup( &f );
    const int a = reset_server_assign( &f, "198.51.100.1:4000", 1 );
    const rocketnet_ack_endpoint_t fresh_ack = f.server.acks->endpoints[a];
    memset( &f.server.acks->endpoints[a], 0xA5, sizeof(f.server.acks->endpoints[a]) );
    // The stats producer binding belongs to the transport lifetime. Poison
    // connection state, but retain the valid binding that reset must keep.
    f.server.acks->endpoints[a].stats_delta = fresh_ack.stats_delta;
    rocketnet_reassembly_slot_t fresh_slots[2];
    memcpy( fresh_slots, f.server.reassembly->slots + a * f.server.reassembly->num_slots, sizeof(fresh_slots) );
    memset( f.server.reassembly->slots + a * f.server.reassembly->num_slots, 0xA5, sizeof(fresh_slots) );
    f.server.reassembly->newest_completed[a] = 77;
    rocketnet_server_connection_reset( &f.server, a );
    check( !rocketnet_client_slots_connected( f.server.client_slots, a ) );
    check( f.connection_ids[a] == 0 );
    check( f.server.acks->endpoints[a].stats_delta == fresh_ack.stats_delta );
    check( memcmp( &f.server.acks->endpoints[a], &fresh_ack, sizeof(fresh_ack) ) == 0 );
    check( memcmp( f.server.reassembly->slots + a * f.server.reassembly->num_slots,
                   fresh_slots, sizeof(fresh_slots) ) == 0 );
    check( f.server.reassembly->newest_completed[a] == 0 );
    const int b = reset_server_assign( &f, "198.51.100.2:4000", 2 );
    check( b == a );
    check( f.server.acks->endpoints[b].highest_sequence == 0 );
    check( f.server.reassembly->newest_completed[b] == 0 );
    reset_server_teardown( &f );
}
REGISTER_UNIT_TEST( test_connection_reset_server_slot_reuse_is_clean );

static void test_connection_reset_server_reset_touches_only_its_slot()
{
    reset_loopback_other_slot_scenario();
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
REGISTER_UNIT_TEST( test_connection_reset_server_reset_touches_only_its_slot );


// The client ends the connection itself, by its own timeout: the server goes
// silent and only the client runs. The transport state a connection leaves
// behind is marked first, so the reset on the transport's update path is the
// only thing that can make it clean.
static void reset_loopback_client_timeout_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    reset_loopback_connect( &f, 0, 0xF001 );
    for ( int i = 0; i < 20; i++ )
        reset_loopback_step( &f );
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( f.clients[0] );
    rocketnet_client_transport_mark_connection_state_for_test( transport );
    for ( int i = 0; i < 400 && rocketnet_message_client_state( f.clients[0] ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED; i++ )
    {
        f.now += 0.05;
        check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f.clients[0] );
    }
    check( rocketnet_message_client_state( f.clients[0] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );
    reset_client_transport_clean( transport );
    reset_loopback_destroy( &f );
}

// The client's connection ends by a denial: both slots are taken, so the
// server denies a third client mid-handshake.
static void reset_loopback_client_denied_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    reset_loopback_connect( &f, 0, 0xF101 );
    reset_loopback_connect( &f, 1, 0xF102 );
    rocketnet_message_client_t * denied = reset_loopback_client_create( f.server, 0xF103 );
    check( denied );
    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( denied );
    rocketnet_client_transport_mark_connection_state_for_test( transport );
    for ( int i = 0; i < 400 && rocketnet_message_client_state( denied ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED; i++ )
    {
        reset_loopback_step( &f );
        check( rocketnet_message_client_update( denied, f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( denied );
    }
    check( rocketnet_message_client_state( denied ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED );
    reset_client_transport_clean( transport );
    rn_test_message_client_stop_and_destroy( denied );
    reset_loopback_destroy( &f );
}

static void test_connection_reset_client_disconnect_is_clean()
{
    reset_loopback_client_timeout_scenario();
    reset_loopback_client_denied_scenario();
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );
    rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );
    client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 2, 8192, 4 );
    check( client.reassembly );
    client.state = ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT;
    client.send_sequence_handshake = 7;
    client.send_sequence_session = 9;
    client.next_fragment_payload_id = 12;
    client.replay_session.highest_opened = 100;
    client.ack.highest_sequence = 101;
    client.num_acked = 2;
    rocketnet_reassembly_slot_t fresh_slots[2];
    memcpy( fresh_slots, client.reassembly->slots, sizeof(fresh_slots) );
    memset( client.reassembly->slots, 0xA5, sizeof(fresh_slots) );
    client.reassembly->newest_completed[0] = 8;
    rocketnet_client_connection_reset( &client );
    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT );
    check( client.send_sequence_handshake == 0 && client.send_sequence_session == 0 );
    check( client.next_fragment_payload_id == 1 );
    check( client.replay_session.highest_opened == 0 );
    check( client.ack.highest_sequence == 0 && client.num_acked == 0 );
    check( client.reassembly->newest_completed[0] == 0 );
    check( memcmp( client.reassembly->slots, fresh_slots, sizeof(fresh_slots) ) == 0 );
    rocketnet_reassembly_set_destroy( client.reassembly );
}
REGISTER_UNIT_TEST( test_connection_reset_client_disconnect_is_clean );

static void reset_loopback_client_disconnect_state_scenario( int state, int mid_fragment )
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    f.clients[0] = reset_loopback_client_create( f.server, 0xC000 + (uint64_t) state );
    check( f.clients[0] );
    check( reset_loopback_advance_client_to_state( &f, 0, state ) );

    if ( mid_fragment )
    {
        uint8_t payload[4096];
        memset( payload, 0xC5, sizeof(payload) );
        check( rocketnet_message_client_send( f.clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                               payload, sizeof(payload) ) != 0 );
        f.now += 0.01;
        check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f.clients[0] );
    }

    rocketnet_client_transport_t * transport = rocketnet_message_client_transport( f.clients[0] );
    rn_message_object_kick( &f.clients[0]->object, 0, ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_client_transport_state( transport ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( f.clients[0]->object.hot_connections[0].state == RN_MESSAGE_CONNECTION_KICKED );
    f.now += 0.01;
    check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
    check( f.clients[0]->object.hot_connections[0].state == RN_MESSAGE_CONNECTION_FREE );
    check( rocketnet_client_transport_reset_witness_for_test( transport ) );

    // Before a session exists the server owns only half-open mapping state;
    // once connected it owns a slot as well. The real sweep/disconnect paths
    // must leave both collections empty in every case.
    reset_loopback_wait_for_server_connection_state_clean( &f );
    reset_loopback_destroy( &f );
}

static void reset_loopback_server_disconnect_state_scenario( int mid_fragment )
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    const int slot = reset_loopback_connect( &f, 0, mid_fragment ? 0xD002 : 0xD001 );

    if ( mid_fragment )
    {
        uint8_t payload[4096];
        memset( payload, 0xD5, sizeof(payload) );
        check( rocketnet_message_server_send( f.server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                               payload, sizeof(payload) ) != 0 );
        f.now += 0.01;
        check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f.server );
    }

    rocketnet_server_transport_disconnect_client( rocketnet_message_server_transport( f.server ), slot );
    f.now += 0.01;
    check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( f.server );
    check( rocketnet_server_transport_connection_state_clean_for_test(
        rocketnet_message_server_transport( f.server ) ) );
    check( !rocketnet_message_server_client_connected( f.server, slot ) );

    for ( int i = 0; i < 100 && rocketnet_message_client_state( f.clients[0] ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED; i++ )
    {
        f.now += 0.01;
        check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f.clients[0] );
    }
    check( rocketnet_message_client_state( f.clients[0] ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( f.clients[0]->object.hot_connections[0].state == RN_MESSAGE_CONNECTION_FREE );
    check( rocketnet_client_transport_reset_witness_for_test(
        rocketnet_message_client_transport( f.clients[0] ) ) );
    reset_loopback_destroy( &f );
}

static void test_connection_reset_each_state_leaves_a_clean_slot()
{
    static const int handshake_and_connected[] = {
        ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST,
        ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE,
        ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED,
    };
    for ( size_t i = 0; i < sizeof(handshake_and_connected)/sizeof(handshake_and_connected[0]); i++ )
        reset_loopback_client_disconnect_state_scenario( handshake_and_connected[i], 0 );

    // Mid-fragment is a connected state with fragments already flushed into
    // the loopback wire but not yet consumed by the peer.
    reset_loopback_client_disconnect_state_scenario( ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED, 1 );
    reset_loopback_server_disconnect_state_scenario( 0 );
    reset_loopback_server_disconnect_state_scenario( 1 );
}
REGISTER_UNIT_TEST( test_connection_reset_each_state_leaves_a_clean_slot );

static void reset_loopback_settle_messages( reset_loopback_t * f )
{
    for ( int i = 0; i < 20; i++ )
        reset_loopback_step( f );
}

static void reset_loopback_explicit_server_disconnect_reason_scenario()
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now );
    check( f.server );
    const int slot = reset_loopback_connect( &f, 0, 0xE001 );
    check( rocketnet_message_server_disconnect_reason( f.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( f.server, 1 - slot ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // The public explicit-disconnect mechanism has no error argument; NONE is
    // its known reason. It still takes the real protocol reset path.
    rocketnet_server_transport_disconnect_client( rocketnet_message_server_transport( f.server ), slot );
    f.now += 0.01;
    check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_server_disconnect_reason( f.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_message_server_disconnect_reason( f.server, 1 - slot ) == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_test_message_client_stop_and_destroy( f.clients[0] );
    f.clients[0] = NULL;
    const int reused = reset_loopback_connect( &f, 1, 0xE002 );
    check( reused == slot );
    check( rocketnet_message_server_disconnect_reason( f.server, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );
    reset_loopback_destroy( &f );
}

static void reset_loopback_server_reliable_timeout_reason_scenario( const rocketnet_message_config_t * config )
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now, config );
    check( f.server );
    const int slot = reset_loopback_connect( &f, 0, 0xE101, config );
    reset_loopback_settle_messages( &f );

    const uint8_t payload[] = { 't', 'i', 'm', 'e', 'o', 'u', 't' };
    check( rocketnet_message_server_send( f.server, slot, 0,
                                  ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, sizeof(payload) ) != 0 );
    f.now += 0.01;
    check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_server_send_packets( f.server );

    // Do not update the client: the real endpoint resend window expires.
    for ( int i = 0; i < 40 && rocketnet_message_server_disconnect_reason( f.server, slot ) != ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT; i++ )
    {
        f.now += 0.05;
        check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f.server );
    }
    check( rocketnet_message_server_disconnect_reason( f.server, slot ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( rocketnet_message_server_disconnect_reason( f.server, 1 - slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    f.now += 0.01;
    check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_server_disconnect_reason( f.server, slot ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

    rn_test_message_client_stop_and_destroy( f.clients[0] );
    f.clients[0] = NULL;
    const int reused = reset_loopback_connect( &f, 1, 0xE102, config );
    check( reused == slot );
    check( rocketnet_message_server_disconnect_reason( f.server, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );
    reset_loopback_destroy( &f );
}

static void reset_loopback_client_reliable_timeout_reason_scenario( const rocketnet_message_config_t * config )
{
    reset_loopback_t f;
    memset( &f, 0, sizeof(f) );
    f.now = 1000.0;
    f.server = reset_loopback_server_create( f.now, config );
    check( f.server );
    reset_loopback_connect( &f, 0, 0xE201, config );
    reset_loopback_settle_messages( &f );

    const uint8_t payload[] = { 't', 'i', 'm', 'e', 'o', 'u', 't' };
    check( rocketnet_message_client_send( f.clients[0], 0,
                                  ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, sizeof(payload) ) != 0 );
    f.now += 0.01;
    check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
    rocketnet_message_client_send_packets( f.clients[0] );

    // Do not update the server: the client endpoint ends the connection.
    for ( int i = 0; i < 40 && rocketnet_message_client_disconnect_reason( f.clients[0] ) != ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT; i++ )
    {
        f.now += 0.05;
        check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f.clients[0] );
    }
    check( rocketnet_message_client_disconnect_reason( f.clients[0] ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    f.now += 0.01;
    check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
    check( rocketnet_message_client_disconnect_reason( f.clients[0] ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

    // BASE has no reconnect operation on an existing client object. A newly
    // established real client starts with NONE; the retained old object above
    // is kept separate so this does not pretend a same-object reconnect.
    rn_test_message_client_stop_and_destroy( f.clients[0] );
    f.clients[0] = NULL;
    reset_loopback_wait_for_server_connection_state_clean( &f );
    reset_loopback_connect( &f, 1, 0xE202, config );
    check( rocketnet_message_client_disconnect_reason( f.clients[1] ) == ROCKETNET_DISCONNECT_REASON_NONE );
    reset_loopback_destroy( &f );
}

static void test_connection_reset_disconnect_reason_kept_until_next_connection()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 100;
    config.message_resend_delay_max_ms = 1000;
    config.reliable_message_timeout_ms = 1000;
    reset_loopback_explicit_server_disconnect_reason_scenario();
    reset_loopback_server_reliable_timeout_reason_scenario( &config );
    reset_loopback_client_reliable_timeout_reason_scenario( &config );
}
REGISTER_UNIT_TEST( test_connection_reset_disconnect_reason_kept_until_next_connection );

// A transport with no send capacity left this update: the send pass goes
// straight to its flush and no connection's write pass runs.
static int reset_loopback_no_send_capacity( void * transport )
{
    (void) transport;
    return 0;
}

// The reliable timeout (6.4 "Giving up") is checked by every update for every
// connected slot, not only by a write pass that reaches its reliable phase.
// The message is sent once; then every send pass is starved and the peer
// never acks. The connection ends RELIABLE_TIMED_OUT at the first update at
// or past first send + reliable_message_timeout_ms, and not before.
static void test_reliable_timeout_fires_while_send_pass_starved()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.message_resend_delay_initial_ms = 100;
    config.message_resend_delay_max_ms = 1000;
    config.reliable_message_timeout_ms = 1000;
    const double timeout = (double) config.reliable_message_timeout_ms / 1000.0;
    const uint8_t payload[] = { 's', 't', 'a', 'r', 'v', 'e', 'd' };

    // the server's connection
    {
        reset_loopback_t f;
        memset( &f, 0, sizeof(f) );
        f.now = 1000.0;
        f.server = reset_loopback_server_create( f.now, &config );
        check( f.server );
        const int slot = reset_loopback_connect( &f, 0, 0xE301, &config );
        reset_loopback_settle_messages( &f );

        check( rocketnet_message_server_send( f.server, slot, 0,
                                      ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, sizeof(payload) ) != 0 );
        f.now += 0.01;
        check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_server_send_packets( f.server );
        const double first_send = f.now;

        f.server->object.link.send_capacity_available = reset_loopback_no_send_capacity;
        for ( int i = 0; i < 30; i++ )
        {
            f.now = first_send + 0.025 + 0.05 * i;
            check( rocketnet_message_server_update( f.server, f.now ) == ROCKETNET_UPDATE_OK );
            rocketnet_message_server_send_packets( f.server );
            const int expected = f.now - first_send < timeout ? ROCKETNET_DISCONNECT_REASON_NONE : ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT;
            check( rocketnet_message_server_disconnect_reason( f.server, slot ) == expected );
        }
        check( rocketnet_message_server_disconnect_reason( f.server, slot ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
        reset_loopback_destroy( &f );
    }

    // the client's connection
    {
        reset_loopback_t f;
        memset( &f, 0, sizeof(f) );
        f.now = 1000.0;
        f.server = reset_loopback_server_create( f.now, &config );
        check( f.server );
        reset_loopback_connect( &f, 0, 0xE302, &config );
        reset_loopback_settle_messages( &f );

        check( rocketnet_message_client_send( f.clients[0], 0,
                                      ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, sizeof(payload) ) != 0 );
        f.now += 0.01;
        check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
        rocketnet_message_client_send_packets( f.clients[0] );
        const double first_send = f.now;

        f.clients[0]->object.link.send_capacity_available = reset_loopback_no_send_capacity;
        for ( int i = 0; i < 30; i++ )
        {
            f.now = first_send + 0.025 + 0.05 * i;
            check( rocketnet_message_client_update( f.clients[0], f.now ) == ROCKETNET_UPDATE_OK );
            rocketnet_message_client_send_packets( f.clients[0] );
            const int expected = f.now - first_send < timeout ? ROCKETNET_DISCONNECT_REASON_NONE : ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT;
            check( rocketnet_message_client_disconnect_reason( f.clients[0] ) == expected );
        }
        check( rocketnet_message_client_disconnect_reason( f.clients[0] ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
        reset_loopback_destroy( &f );
    }
}
REGISTER_UNIT_TEST( test_reliable_timeout_fires_while_send_pass_starved );
