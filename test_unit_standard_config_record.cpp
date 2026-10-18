// The CONFIG control record vectors (standard/MESSAGES.md m11). Every vector
// is regenerated from the sdk codec and compared byte for byte, so a drift in
// the codec or the default config turns this case red.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_channel.h"
#include "sdk/rocketnet_message.h"

#ifndef RN_STANDARD_VECTOR
#define RN_STANDARD_VECTOR( id_, ... ) static const uint8_t RN_##id_[] = __VA_ARGS__
#endif

// The record that carries the config body: code 2 with CONTROL set.
static int rn_config_payload( uint8_t * out, int cap, const struct rocketnet_message_config_body_t * body )
{
    struct rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.control = 1;
    record.bytes = ROCKETNET_MESSAGE_CONFIG_BYTES;
    uint8_t config_bytes[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_message_write_config_body( config_bytes, body );
    record.data = config_bytes;
    struct rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, out, cap );
    if ( !rocketnet_message_writer_add( &writer, &record ) )
        return 0;
    return rocketnet_message_writer_finish( &writer );
}

// These vectors are produced by running the codec. The defaults vector is
// the library's real default config; each mismatch vector differs in exactly
// one field.
RN_STANDARD_VECTOR( CONFIG_DEFAULTS, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_VERSION_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_ENABLED_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_NUM_CHANNELS_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_WINDOW_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_RECEIVE_RING_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_MAX_MESSAGE_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x80, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );
RN_STANDARD_VECTOR( CONFIG_FRAGMENT_MISMATCH, { 0x22, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } );

static void standard_config_record_vectors()
{
    // defaults: the library's real default config
    struct rocketnet_message_config_body_t body;
    body.version = ROCKETNET_MESSAGE_CONFIG_VERSION;
    body.enabled = ROCKETNET_CHANNEL_ENABLED_FIRST_VERSION & 0xFFu;
    body.num_channels = 32;
    body.window_messages = 1024;
    body.receive_ring_bytes = 262144;
    body.max_message_bytes = 262144;
    body.fragment_bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES;

    uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES];
    int n;

    n = rn_config_payload( payload, sizeof( payload ), &body );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_DEFAULTS ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_DEFAULTS, sizeof( RN_CONFIG_DEFAULTS ) ) == 0 );

    // one mismatch vector per field
    struct rocketnet_message_config_body_t m = body;
    m.version = 2;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_VERSION_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_VERSION_MISMATCH, sizeof( RN_CONFIG_VERSION_MISMATCH ) ) == 0 );

    m = body;
    m.enabled = 1;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_ENABLED_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_ENABLED_MISMATCH, sizeof( RN_CONFIG_ENABLED_MISMATCH ) ) == 0 );

    m = body;
    m.num_channels = 16;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_NUM_CHANNELS_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_NUM_CHANNELS_MISMATCH, sizeof( RN_CONFIG_NUM_CHANNELS_MISMATCH ) ) == 0 );

    m = body;
    m.window_messages = 512;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_WINDOW_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_WINDOW_MISMATCH, sizeof( RN_CONFIG_WINDOW_MISMATCH ) ) == 0 );

    m = body;
    m.receive_ring_bytes = 131072;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_RECEIVE_RING_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_RECEIVE_RING_MISMATCH, sizeof( RN_CONFIG_RECEIVE_RING_MISMATCH ) ) == 0 );

    m = body;
    m.max_message_bytes = 131072;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_MAX_MESSAGE_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_MAX_MESSAGE_MISMATCH, sizeof( RN_CONFIG_MAX_MESSAGE_MISMATCH ) ) == 0 );

    m = body;
    m.fragment_bytes = 1024;
    n = rn_config_payload( payload, sizeof( payload ), &m );
    rocketnet_unit_check( n == (int) sizeof( RN_CONFIG_FRAGMENT_MISMATCH ) );
    rocketnet_unit_check( memcmp( payload, RN_CONFIG_FRAGMENT_MISMATCH, sizeof( RN_CONFIG_FRAGMENT_MISMATCH ) ) == 0 );
}

REGISTER_UNIT_TEST( standard_config_record_vectors );
