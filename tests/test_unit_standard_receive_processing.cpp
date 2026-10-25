/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_receive_processing.cpp -- the vector twins of RN-1 t10
// (standard/TRANSPORT.md, Receive processing), regenerated through the
// implementation. Each vector is one malformed packet and the counter its
// refusal fires. The bytes come from the wire writers under fixed keys, the
// counter from the same prefilter, server and client entry points a live
// packet takes. No socket, no thread, no clock: time is an argument.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_fragment.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"

static const rn_standard_vector_t standard_receive_processing_table[] = {
    RN_STANDARD_VECTOR( "t10.prefilter-too-short", "0500b75901" ),
    RN_STANDARD_VECTOR( "t10.prefilter-too-large", "0500b759010000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
                                                   "00000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t10.prefilter-wrong-epoch", "0501b7590100" ),
    RN_STANDARD_VECTOR( "t10.prefilter-wrong-value", "050000000000" ),
    RN_STANDARD_VECTOR( "t10.reserved-type", "0a00b7590100" ),
    RN_STANDARD_VECTOR( "t10.server-wrong-direction", "0100b7590100" ),
    RN_STANDARD_VECTOR( "t10.client-wrong-direction", "0000b7590100" ),
    RN_STANDARD_VECTOR( "t10.client-session-wrong-size", "0400b7590100" ),
    RN_STANDARD_VECTOR( "t10.client-cookie-state-before-size", "0700b759010000000000000000000000000000000000000000000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t10.client-wrong-state", "0400b7590100e80300001e9e1564b9d8a08f69ab9307a53cd68592162df7283ff2cde8361303f345c3" ),
    RN_STANDARD_VECTOR( "t10.server-no-mapping", "0400b75901000000000000000000000000000000000000000000000000000000000000000000000000" ),
    RN_STANDARD_VECTOR( "t10.server-not-connected", "0400b7590100e80300002e73ac3a5ddfe7a58af04af3979df3b7569ca5e68663302ed1bfd84ef9ac23" ),
    RN_STANDARD_VECTOR( "t10.zero-sequence", "0400b7590100000000001e9e1564b9d8a08f69ab9307a53cd68592162df7283ff2cde8361303f345c3" ),
    RN_STANDARD_VECTOR( "t10.already-received", "0400b7590100e80300001e9e1564b9d8a08f69ab9307a53cd68592162df7283ff2cde8361303f345c3" ),
    RN_STANDARD_VECTOR( "t10.did-not-open", "0400b7590100e8030000f8c3ad9b054ea2d3b579df2948342fad2dcab4051d60e7bef3176504cf342a" ),
    RN_STANDARD_VECTOR( "t10.bad-ack-block", "0400b7590100e80300001b9e1564b9d8a08f69ab9303a53cd6c126ae3cae1bd178c4a3c6ca6f48a017" ),
    RN_STANDARD_VECTOR( "t10.wrong-opened-size", "0400b7590100e80300001e9e1564b9d8a08f69ab9303a53cd6c62e2c88afc2454f6f6277034b30f3492a" ),
    RN_STANDARD_VECTOR( "t10.keep-alive-invalid-index", "0400b7590100e80300001e9e1564b9d8a08b69ab9307a53cd611137c3d36904ed330e5ecf7452e8e62" ),
    RN_STANDARD_VECTOR( "t10.fragment-zero-payload-id", "0900b7590100e80300001e9e1564b9d8a08f69ab9303a4d2eca3b0d7eb0eaf4b9bb6e394bd3b5d2c" ),
};

static const uint8_t standard_time_start = 0;
static const double standard_receive_time = 1000.0;

static const uint8_t standard_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
static const uint64_t standard_protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

static const uint8_t standard_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48 };
static const uint8_t standard_client_to_server_key[ROCKETNET_KEY_BYTES] = {
    0x10,
    0x11,
    0x12,
    0x13,
    0x14,
    0x15,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1a,
    0x1b,
    0x1c,
    0x1d,
    0x1e,
    0x1f,
    0x20,
    0x21,
    0x22,
    0x23,
    0x24,
    0x25,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2a,
    0x2b,
    0x2c,
    0x2d,
    0x2e,
    0x2f,
};
static const uint8_t standard_server_to_client_key[ROCKETNET_KEY_BYTES] = {
    0x30,
    0x31,
    0x32,
    0x33,
    0x34,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3a,
    0x3b,
    0x3c,
    0x3d,
    0x3e,
    0x3f,
    0x40,
    0x41,
    0x42,
    0x43,
    0x44,
    0x45,
    0x46,
    0x47,
    0x48,
    0x49,
    0x4a,
    0x4b,
    0x4c,
    0x4d,
    0x4e,
    0x4f,
};
static const uint8_t standard_wrong_key[ROCKETNET_KEY_BYTES] = {
    0x90,
    0x91,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0x9b,
    0x9c,
    0x9d,
    0x9e,
    0x9f,
    0xa0,
    0xa1,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0xa6,
    0xa7,
    0xa8,
    0xa9,
    0xaa,
    0xab,
    0xac,
    0xad,
    0xae,
    0xaf,
};

// the sealed plaintext of a valid keep-alive: a fresh ack block and the eight
// type bytes that name client index 0 of 4 slots

static int standard_fresh_block( uint8_t * out )
{
    memset( out, 0, ROCKETNET_ACK_BLOCK_MIN_BYTES );
    return ROCKETNET_ACK_BLOCK_MIN_BYTES;
}

static int standard_keep_alive_plaintext( uint8_t * out )
{
    uint8_t * p = out + standard_fresh_block( out );
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint32( &p, 4 );
    return (int) ( p - out );
}

// a malformed block, padded with zeros to the keep-alive's opened size

static int standard_bad_block_plaintext( uint8_t * out )
{
    const int bytes = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES - ROCKETNET_PACKET_OVERHEAD_BYTES;
    memset( out, 0, (size_t) bytes );
    uint8_t * p = out;
    rocketnet_write_uint32( &p, 5 );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint16( &p, 0 );
    return bytes;
}

// a valid block over nine type bytes where eight belong

static int standard_wrong_opened_size_plaintext( uint8_t * out )
{
    uint8_t * p = out + standard_fresh_block( out );
    memset( p, 0, 9 );
    p += 9;
    return (int) ( p - out );
}

// a fresh block, a fragment header naming payload id zero, and one body byte

static int standard_fragment_plaintext( uint8_t * out )
{
    uint8_t * p = out + standard_fresh_block( out );
    rocketnet_write_uint32( &p, 0 );
    rocketnet_write_uint8( &p, 0 );
    rocketnet_write_uint8( &p, 1 );
    rocketnet_write_uint8( &p, 0xEE );
    return (int) ( p - out );
}

static int standard_seal( uint8_t type, uint64_t sequence, const uint8_t * plaintext, int plaintext_bytes,
                          const uint8_t * key, uint8_t * out )
{
    return rocketnet_write_sealed_packet( out, type, standard_filter, sequence, NULL,
                                          plaintext, plaintext_bytes, standard_protocol_id,
                                          standard_salt, NULL, key );
}

static int standard_receive_processing_generate( const char * id, uint8_t * out, int capacity )
{
    uint8_t plaintext[256];
    if ( capacity < ROCKETNET_PREFILTER_BYTES )
        return -1;

    if ( strcmp( id, "t10.prefilter-too-short" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, standard_filter );
        return ROCKETNET_PREFILTER_BYTES - 1;
    }
    if ( strcmp( id, "t10.prefilter-too-large" ) == 0 )
    {
        if ( capacity < ROCKETNET_RECEIVE_CEILING + 1 )
            return -1;
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, standard_filter );
        memset( p, 0, (size_t) ( ROCKETNET_RECEIVE_CEILING + 1 - ROCKETNET_PREFILTER_BYTES ) );
        return ROCKETNET_RECEIVE_CEILING + 1;
    }
    if ( strcmp( id, "t10.prefilter-wrong-epoch" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, standard_filter );
        out[1] = 1;
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.prefilter-wrong-value" ) == 0 )
    {
        uint8_t * p = out;
        const uint8_t no_value[ROCKETNET_FILTER_VALUE_BYTES] = { 0, 0, 0, 0 };
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, no_value );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.reserved-type" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_FRAGMENT_PACKET + 1, standard_filter );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.server-wrong-direction" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_DENIED_PACKET, standard_filter );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.client-wrong-direction" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, standard_filter );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.client-session-wrong-size" ) == 0 )
    {
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, standard_filter );
        return ROCKETNET_PREFILTER_BYTES;
    }
    if ( strcmp( id, "t10.client-cookie-state-before-size" ) == 0 )
    {
        memset( out, 0, (size_t) ( ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES - 1 ) );
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_COOKIE_PACKET, standard_filter );
        return ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES - 1;
    }
    if ( strcmp( id, "t10.server-no-mapping" ) == 0 )
    {
        memset( out, 0, ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
        uint8_t * p = out;
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, standard_filter );
        return ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
    }
    if ( strcmp( id, "t10.client-wrong-state" ) == 0 ||
         strcmp( id, "t10.server-not-connected" ) == 0 ||
         strcmp( id, "t10.zero-sequence" ) == 0 ||
         strcmp( id, "t10.already-received" ) == 0 )
    {
        const int plaintext_bytes = standard_keep_alive_plaintext( plaintext );
        const uint8_t * key = strcmp( id, "t10.server-not-connected" ) == 0 ? standard_client_to_server_key : standard_server_to_client_key;
        int bytes = standard_seal( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1000, plaintext, plaintext_bytes, key, out );
        if ( strcmp( id, "t10.zero-sequence" ) == 0 )
            memset( out + ROCKETNET_PREFILTER_BYTES, 0, ROCKETNET_SEQUENCE_BYTES );
        return bytes;
    }
    if ( strcmp( id, "t10.did-not-open" ) == 0 )
    {
        const int plaintext_bytes = standard_keep_alive_plaintext( plaintext );
        return standard_seal( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1000, plaintext, plaintext_bytes,
                              standard_wrong_key, out );
    }
    if ( strcmp( id, "t10.bad-ack-block" ) == 0 )
    {
        const int plaintext_bytes = standard_bad_block_plaintext( plaintext );
        return standard_seal( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1000, plaintext, plaintext_bytes,
                              standard_server_to_client_key, out );
    }
    if ( strcmp( id, "t10.keep-alive-invalid-index" ) == 0 )
    {
        uint8_t * p = plaintext + standard_fresh_block( plaintext );
        rocketnet_write_uint32( &p, 4 );
        rocketnet_write_uint32( &p, 4 );
        return standard_seal( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1000, plaintext,
                              (int) ( p - plaintext ), standard_server_to_client_key, out );
    }
    if ( strcmp( id, "t10.wrong-opened-size" ) == 0 )
    {
        const int plaintext_bytes = standard_wrong_opened_size_plaintext( plaintext );
        return standard_seal( ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 1000, plaintext, plaintext_bytes,
                              standard_server_to_client_key, out );
    }
    if ( strcmp( id, "t10.fragment-zero-payload-id" ) == 0 )
    {
        const int plaintext_bytes = standard_fragment_plaintext( plaintext );
        return standard_seal( ROCKETNET_CONNECTION_FRAGMENT_PACKET, 1000, plaintext, plaintext_bytes,
                              standard_server_to_client_key, out );
    }
    return -1;
}

// the client end, on fixed keys and a chosen state: no handshake, no clock

struct standard_client_t
{
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t counters;
};

static void standard_client_setup( struct standard_client_t * fixture, int state )
{
    memset( &fixture->counters, 0, sizeof( fixture->counters ) );
    rocketnet_client_protocol_init( &fixture->client, &fixture->counters );
    fixture->client.state = state;
    fixture->client.signed_fields.protocol_id = standard_protocol_id;
    memcpy( fixture->client.connection_salt, standard_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    memcpy( fixture->client.keys.session_server_to_client, standard_server_to_client_key, ROCKETNET_KEY_BYTES );
    fixture->client.send_sequence_session = 1;
    fixture->client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, ROCKETNET_NUM_CLIENT_PACKETS / 2 );
    rocketnet_unit_check( fixture->client.reassembly != NULL );
    rocketnet_reassembly_set_begin_update( fixture->client.reassembly );
}

static void standard_client_teardown( struct standard_client_t * fixture )
{
    rocketnet_reassembly_set_destroy( fixture->client.reassembly );
}

static int standard_client_process( struct standard_client_t * fixture, const uint8_t * packet, int packet_bytes )
{
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int payload_bytes = 0;
    return rocketnet_client_protocol_process_packet( &fixture->client, packet, packet_bytes, standard_receive_time,
                                                     payload, (int) sizeof( payload ), &payload_bytes );
}

// the server end: a mapping may hold the fixed client keys and a slot, or one
// of those may be absent, which is the condition under test

struct standard_server_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t from;
};

static void standard_server_setup( struct standard_server_t * fixture, bool add_mapping, bool connected )
{
    memset( &fixture->server, 0, sizeof( fixture->server ) );
    memset( &fixture->counters, 0, sizeof( fixture->counters ) );
    rocketnet_unit_check( rocketnet_address_parse( &fixture->from, "198.51.100.7:56789" ) );
    fixture->server.protocol_id = standard_protocol_id;
    memcpy( fixture->server.packet_filter, standard_filter, ROCKETNET_FILTER_VALUE_BYTES );
    fixture->server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    fixture->server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    fixture->server.counters = &fixture->counters;
    rocketnet_unit_check( fixture->server.encryption_map != NULL && fixture->server.client_slots != NULL );
    if ( add_mapping )
    {
        const int index = rocketnet_encryption_map_add( fixture->server.encryption_map, &fixture->from, 5000.0 );
        rocketnet_unit_check( index >= 0 );
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( fixture->server.encryption_map, index );
        memcpy( mapping->keys.session_client_to_server, standard_client_to_server_key, ROCKETNET_KEY_BYTES );
        memcpy( mapping->connection_salt, standard_salt, ROCKETNET_CONNECTION_SALT_BYTES );
        mapping->control->client_index = connected ? 0 : -1;
    }
}

static void standard_server_teardown( struct standard_server_t * fixture )
{
    rocketnet_encryption_map_destroy( fixture->server.encryption_map );
    rocketnet_client_slots_destroy( fixture->server.client_slots );
}

static void standard_server_process( struct standard_server_t * fixture, const uint8_t * packet, int packet_bytes )
{
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    rocketnet_server_protocol_process_packet( &fixture->server, &fixture->from, packet, packet_bytes,
                                              (uint64_t) standard_time_start, standard_receive_time,
                                              response, &response_bytes, payload, &payload_bytes, &payload_client_index );
}

// Each block's fixture is static: an unoptimised MSVC build gives every block
// its own stack slot, and fifteen protocol fixtures at once overflow the
// default 1 MB stack. Each setup resets its fixture in full, so a block never
// sees another's state.

static void standard_receive_processing_counters()
{
    uint8_t packet[RN_STANDARD_VECTOR_MAX_BYTES];

    // the prefilter's four refusals, through the shipped comparison
    {
        const struct
        {
            const char * id;
            int verdict;
            int counter;
        } rows[] = {
            { "t10.prefilter-too-short", ROCKETNET_PREFILTER_TOO_SHORT, ROCKETNET_COUNTER_PREFILTER_TOO_SHORT },
            { "t10.prefilter-too-large", ROCKETNET_PREFILTER_TOO_LARGE, ROCKETNET_COUNTER_PREFILTER_TOO_LARGE },
            { "t10.prefilter-wrong-epoch", ROCKETNET_PREFILTER_WRONG_EPOCH, ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH },
            { "t10.prefilter-wrong-value", ROCKETNET_PREFILTER_WRONG_VALUE, ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE },
        };
        for ( size_t i = 0; i < sizeof( rows ) / sizeof( rows[0] ); i++ )
        {
            const int bytes = standard_receive_processing_generate( rows[i].id, packet, (int) sizeof( packet ) );
            const int verdict = rocketnet_prefilter_check( packet, bytes, standard_filter );
            rocketnet_unit_check( verdict == rows[i].verdict );
            rocketnet_unit_check( rocketnet_prefilter_counter( verdict ) == rows[i].counter );
        }
    }

    // the type gates before decryption, at the server and at the client
    {
        static struct standard_server_t server;
        standard_server_setup( &server, false, false );
        int bytes = standard_receive_processing_generate( "t10.reserved-type", packet, (int) sizeof( packet ) );
        standard_server_process( &server, packet, bytes );
        rocketnet_unit_check( server.counters.values[ROCKETNET_COUNTER_RESERVED_TYPE] == 1 );
        standard_server_teardown( &server );
    }
    {
        static struct standard_server_t server;
        standard_server_setup( &server, false, false );
        int bytes = standard_receive_processing_generate( "t10.server-wrong-direction", packet, (int) sizeof( packet ) );
        standard_server_process( &server, packet, bytes );
        rocketnet_unit_check( server.counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 1 );
        standard_server_teardown( &server );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.client-wrong-direction", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_WRONG_DIRECTION] == 1 );
        standard_client_teardown( &client );
    }

    // the wire size precedes the state gate and the mapping lookup
    {
        static struct standard_server_t server;
        standard_server_setup( &server, false, false );
        int bytes = standard_receive_processing_generate( "t10.server-no-mapping", packet, (int) sizeof( packet ) );
        standard_server_process( &server, packet, bytes );
        rocketnet_unit_check( server.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] == 1 );
        standard_server_teardown( &server );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.client-session-wrong-size", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_SIZE] == 1 );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN] == 0 );
        rocketnet_unit_check( client.client.integrity_failures == 0 );
        standard_client_teardown( &client );
    }

    // the connection-cookie state gate precedes its exact-size check
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.client-cookie-state-before-size", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_STATE] == 1 );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_WRONG_SIZE] == 0 );
        standard_client_teardown( &client );
    }

    // the state gate, before the sequence
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
        int bytes = standard_receive_processing_generate( "t10.client-wrong-state", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_STATE] == 1 );
        standard_client_teardown( &client );
    }

    // the server slot gate, after the open and before the replay mark
    {
        static struct standard_server_t server;
        standard_server_setup( &server, true, false );
        int bytes = standard_receive_processing_generate( "t10.server-not-connected", packet, (int) sizeof( packet ) );
        standard_server_process( &server, packet, bytes );
        rocketnet_unit_check( server.counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED] == 1 );
        standard_server_teardown( &server );
    }

    // the sequence reconstruction, the replay mark, the open, the ack block,
    // the opened size and the type data, at the client end
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.zero-sequence", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ZERO_SEQUENCE] == 1 );
        rocketnet_unit_check( client.client.integrity_failures == 0 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.already-received", packet, (int) sizeof( packet ) );
        rocketnet_unit_check( standard_client_process( &client, packet, bytes ) );
        rocketnet_unit_check( standard_client_process( &client, packet, bytes ) == 0 );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED] == 1 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.did-not-open", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_DID_NOT_OPEN] == 1 );
        rocketnet_unit_check( client.client.integrity_failures == 1 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.bad-ack-block", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_BAD_ACK_BLOCK] == 1 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.wrong-opened-size", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE] == 1 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.keep-alive-invalid-index", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_WRONG_OPENED_SIZE] == 1 );
        rocketnet_unit_check( client.client.integrity_failures == 0 );
        standard_client_teardown( &client );
    }
    {
        static struct standard_client_t client;
        standard_client_setup( &client, ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
        int bytes = standard_receive_processing_generate( "t10.fragment-zero-payload-id", packet, (int) sizeof( packet ) );
        standard_client_process( &client, packet, bytes );
        rocketnet_unit_check( client.counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_ZERO_PAYLOAD_ID] == 1 );
        standard_client_teardown( &client );
    }
}

static void test_standard_receive_processing_vectors()
{
    const int count = (int) ( sizeof( standard_receive_processing_table ) / sizeof( standard_receive_processing_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_receive_processing_table, count, standard_receive_processing_generate ) == count );

    standard_receive_processing_counters();
}

REGISTER_UNIT_TEST( test_standard_receive_processing_vectors );
