/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_timing_and_counters.cpp -- the vector twins of RN-1 t17
// (Timing and defaults) and t18 (Error handling and counters), regenerated
// through the implementation.

#include "test_unit_standard.h"

#include "rocketnet_counters.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_server.h"
#include "rocketnet_constants.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_address.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#define ROCKETNET_CONFORMANCE_DEFAULTS_ONLY
#include "rocketnet_server_transport.cpp"
#undef ROCKETNET_CONFORMANCE_DEFAULTS_ONLY

static const char * const counter_names[] = {
#define ROCKETNET_COUNTER_NAME_ROW( name, klass, family ) #name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_NAME_ROW )
#undef ROCKETNET_COUNTER_NAME_ROW
};

static const int16_t counter_families[] = {
#define ROCKETNET_COUNTER_FAMILY_ROW( name, klass, family ) ROCKETNET_COUNTER_##family,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_FAMILY_ROW )
#undef ROCKETNET_COUNTER_FAMILY_ROW
};

static uint32_t standard_fnv1a32( const char * text )
{
    uint32_t hash = 2166136261u;
    for ( const unsigned char * p = (const unsigned char *) text; *p; p++ )
    {
        hash ^= *p;
        hash *= 16777619u;
    }

    return hash;
}

static void standard_put_u16le( uint8_t * out, int & at, uint16_t value )
{
    out[at++] = (uint8_t) ( value & 0xff );
    out[at++] = (uint8_t) ( ( value >> 8 ) & 0xff );
}

static void standard_put_u64le( uint8_t * out, int & at, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
    {
        out[at++] = (uint8_t) ( ( value >> ( 8 * i ) ) & 0xff );
    }
}

static int standard_timing_and_counters_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t17.timing-defaults" ) == 0 )
    {
        static const uint64_t defaults[] = {
            (uint64_t) ROCKETNET_DEFAULT_SEND_RATE,
            (uint64_t) ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS,
            (uint64_t) ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_LIFETIME_SECONDS,
            (uint64_t) ROCKETNET_DEFAULT_COOKIE_ROTATION_SECONDS,
            (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS,
            (uint64_t) ROCKETNET_ACK_DEBT_THRESHOLD,
            (uint64_t) ROCKETNET_DEFAULT_SERVER_HANDSHAKES_PER_UPDATE,
            (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET,
            (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS,
        };
        const int count = (int) ( sizeof( defaults ) / sizeof( defaults[0] ) );
        if ( capacity < count * 8 )
            return -1;

        int at = 0;
        for ( int i = 0; i < count; i++ )
            standard_put_u64le( out, at, defaults[i] );
        return at;
    }

    if ( strcmp( id, "t18.counter-registry" ) == 0 )
    {
        const int count = ROCKETNET_NUM_COUNTERS;
        if ( capacity < count * 7 )
            return -1;

        int at = 0;
        for ( int i = 0; i < count; i++ )
        {
            const uint32_t hash = standard_fnv1a32( counter_names[i] );
            out[at++] = (uint8_t) ( hash & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 8 ) & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 16 ) & 0xff );
            out[at++] = (uint8_t) ( ( hash >> 24 ) & 0xff );
            out[at++] = (uint8_t) rocketnet_counter_class( i );
            standard_put_u16le( out, at, (uint16_t) ( counter_families[i] + 1 ) );
        }

        return at;
    }

    return -1;
}

// Re-pin t17 because the ACK-debt threshold now records one eliciting packet,
// and again for the handshake work budget's default of 16.
static const rn_standard_vector_t standard_timing_and_counters_table[] = {
    RN_STANDARD_VECTOR( "t17.timing-defaults", "0a0000000000000005000000000000000a000000000000000300000000000000"
                                               "3c000000000000000a000000000000000a000000000000000100000000000000"
                                               "10000000000000005a000000000000000a00000000000000" ),
    RN_STANDARD_VECTOR( "t18.counter-registry",
                        "8472d40203000064f915d0000000ed4bcc7f000000d0a5f5ee000000a0f3313d000000b6f1b51a00"
                        "0000c4cd1cf70000007b0a16790300009f6c53a8000800f4318ea9010800e9bb27a7010800e2b441"
                        "47000800a6f4da6d0008008fbe1858000800078a62f3000800381e6d9d00080074ad3d54000800d9"
                        "133a06000800867d97ae010800f2c0a831000800b054ce7a000800765c65f4000800c63a5e6b0008"
                        "0083ac9718000800392db38a000800cab35227000800d44833550008002d7185d40008009e6881e9"
                        "000800bc8fa1e80108003c90b3080008008d404106030000dda2ee3d002000785d386600200044c9"
                        "d6310020000e509b5200200012666f95012000104e2af9002000c38d493f0020002dcb1de3012000"
                        "142d6274030000dd093874012900a718c3610029007d7e1478002900239fe39f002900e4d57b2200"
                        "29000ead34f4012900edc0b2220300005882f200013000e45a4fd7003000a0c9d1a9003000eed15a"
                        "44003000036293d90030003236a2080130000f0d2e81030000eac25dfe013700a2a8d3d100370032"
                        "33cd4e003700ac2292170037004568ca46003700b7e22fb2013700aa41977f010000e816ddd10100"
                        "00ce68d7b201000082b49c08000000462ea274010000aa0cf9d400000083bb10c200000090a27292"
                        "0100004a5a6ef50100004114e7e901000005ef7afd03000041c7d87901480093c7db0b004800390d"
                        "a1710148004e9794f8030000c8f0edd4014c00d5c9a8e0004c00d1506eb9004c00ef2550d4004c00"
                        "e4bc5985014c004a2735c20300001427179100520081ecdf6c00520085605f2c00520028d57e9100"
                        "520002c694a60300009cf448b7015700590f01f8005700f36f06860057003d948096005700900e20"
                        "d4015700579ee834030000af3ffc5c015d000af39798005d002aaa4800005d00d4c63f68005d00ac"
                        "22c8ca015d0039ab6d3b030000e56b749901630080f25098006300bcfdcd35006300e2f890b50063"
                        "0093ae2e5f016300704aeeba002900ae736c4b002900bdbdd030003000298bcc0a003000bbaf99b0"
                        "0037007f4a291100370048125558030000ec98a5af006f00e982820e016f004bddb835006f0011fb"
                        "6fa7006f00afcad64a006f00c0d9e357006f001a2f7e36006f00ca9866b0016f00edf37c75010000"
                        "500c6ade000000fa02eca4005700c84be01e005700836c6012005d000732aa00005d008124966700"
                        "63004d51b5f9006300be45d6100300005852df2f0180000edc53ed008000a51ca687008000ef8c72"
                        "ea008000018c3c770080003c00083b008000549bf77f0180009fc827da01000032bd593400000020"
                        "8d29eb0029001f85bd3d003000952e8fa2003700146d25e0006f00da2fc91f005700fd2fafea005d"
                        "009bd7db8e006300364c15f1008000b81100f30000003bc4a0c50000007ed194cb000000c935a956"
                        "000000958442e900480087ed875f0000006130e850000000c9577e8600080072480e32000800e3c7"
                        "97df00000041af4dad0000001205760d0200005c34ae7c000000dd1ac85c000000ea22624a020000"
                        "6e2d52bb0000006859e828000000a1b5721800000055a4ca83000000d28aa748000000fea38fb702"
                        "0000fd92bd5f020000bc1109050000002a62770c000000286577120100009fad6596020000c22fdb"
                        "2c0020006bc6a44102000088edb156000000d692db95000000d76a4df9000000eb037f4900000011"
                        "1a37e10000009c475ee70000008c5c0526010000445bfe0f000000ff64f7fa010000754f7d3a0000"
                        "00b1eb078c000000e9539e77000000b9172c0700000034b485d8000000b0cb42af010000bdb0385c"
                        "0100009f8d1dfd000000bea9a8b8004c0022203fb10052006ac070be0008008b6d57dc000000bf7a"
                        "1aaf0100009b861b950100007987fe4c010000f984f06e010000251195dc000000dfffa197010000"
                        "7e014fa10000001d39f161010000ab9fe25c0000007d984f4e00000058b60b73000000f925ca2400"
                        "0000509a50f50000000a0426b7000000dd1a0477000000ee4e501b00000037af52db0000007a3193"
                        "2200000079b815e7000000395ff812000000a458a7b300000098214ffa00200049b0ba620008003a"
                        "68d424000000da288aa6000000" ),
};

static void test_standard_timing_and_counters_vectors()
{
    const int count = (int) ( sizeof( standard_timing_and_counters_table ) / sizeof( standard_timing_and_counters_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_timing_and_counters_table, count, standard_timing_and_counters_generate ) == count );

    // the registry is a registry: every name is non-empty and distinct, every
    // class is one of the four, and every family is none or a counter index

    const int counters = ROCKETNET_NUM_COUNTERS;
    rocketnet_unit_check( counters == (int) ( sizeof( counter_names ) / sizeof( counter_names[0] ) ) );
    rocketnet_unit_check( counters == (int) ( sizeof( counter_families ) / sizeof( counter_families[0] ) ) );
    for ( int i = 0; i < counters; i++ )
    {
        rocketnet_unit_check( counter_names[i] != NULL && counter_names[i][0] != 0 );

        const int klass = rocketnet_counter_class( i );
        rocketnet_unit_check( klass >= ROCKETNET_COUNTER_CLASS_REFUSAL && klass < ROCKETNET_NUM_COUNTER_CLASSES );
        rocketnet_unit_check( counter_families[i] == ROCKETNET_COUNTER_NONE ||
                              ( counter_families[i] >= 0 && counter_families[i] < counters ) );
        for ( int j = 0; j < i; j++ )
        {
            rocketnet_unit_check( strcmp( counter_names[i], counter_names[j] ) != 0 );
        }
    }

    // the walk's own refusals, as the skeleton's case checks them

    const uint8_t a[3] = { 1, 2, 3 };
    const uint8_t b[3] = { 1, 9, 3 };
    rocketnet_unit_check( rn_standard_first_difference( a, 3, a, 3 ) == -1 );
    rocketnet_unit_check( rn_standard_first_difference( a, 3, b, 3 ) == 1 );

    uint8_t scratch[2];
    rocketnet_unit_check( rn_standard_hex_decode( "0g", scratch, 2 ) == -1 );
    rocketnet_unit_check( standard_timing_and_counters_generate( "t17.no-such-vector", scratch, 2 ) == -1 );
}

// ---------------------------------------------------------------------------
// Behavioral t17: handshake retransmission at the send rate. The clock is an
// argument starting at 100.0; no socket, thread or wall clock is read.
// ---------------------------------------------------------------------------

struct timing_server_fixture_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    struct rocketnet_address_t client_address;
};

static void timing_fill_token_fields( struct rocketnet_signed_connect_data_t * signed_fields,
                                      struct rocketnet_connect_token_private_t * private_fields,
                                      uint64_t client_id )
{
    memset( signed_fields, 0, sizeof( *signed_fields ) );
    signed_fields->version = ROCKETNET_WIRE_VERSION;
    signed_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields->create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields->timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields->address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields->ipv4_address, server_address, 6 );
    memcpy( signed_fields->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( private_fields, 0, sizeof( *private_fields ) );
    private_fields->client_id = client_id;
    private_fields->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields->expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields->client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields->server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );
}

static void timing_mint_token( uint8_t * token_data, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    timing_fill_token_fields( &signed_fields, &private_fields, client_id );
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                         rocketnet_test_backend_secret_key, token_data ) );
}

static void timing_server_setup( struct timing_server_fixture_t * test )
{
    memset( test, 0, sizeof( *test ) );

    struct rocketnet_server_protocol_t * server = &test->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server->packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    rocketnet_unit_check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS;
    server->send_rate = ROCKETNET_DEFAULT_SEND_RATE;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = &test->counters;
    rocketnet_unit_check( server->encryption_map && server->used_token_history &&
                          server->client_slots && server->acks );

    rocketnet_server_protocol_update( server, 1000.0 );

    rocketnet_unit_check( rocketnet_address_parse( &test->client_address, "198.51.100.7:56789" ) );
}

static void timing_server_teardown( struct timing_server_fixture_t * test )
{
    rocketnet_encryption_map_destroy( test->server.encryption_map );
    rocketnet_used_token_history_destroy( test->server.used_token_history );
    rocketnet_client_slots_destroy( test->server.client_slots );
    rocketnet_server_acks_destroy( test->server.acks );
}

static int timing_server_process( struct timing_server_fixture_t * test,
                                  const uint8_t * packet, int packet_bytes,
                                  double current_time,
                                  uint8_t * response, int * response_bytes,
                                  uint8_t * payload, int * payload_bytes, int * payload_client_index )
{
    *response_bytes = 0;
    return rocketnet_server_protocol_process_packet( &test->server, &test->client_address,
                                                     packet, packet_bytes,
                                                     ROCKETNET_TEST_CREATE_TS, current_time,
                                                     response, response_bytes,
                                                     payload, payload_bytes, payload_client_index );
}

static void timing_build_request( uint8_t * request, const struct rocketnet_address_t * address )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    timing_fill_token_fields( &signed_fields, &private_fields, ROCKETNET_TEST_CLIENT_ID );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_unit_check( rocketnet_write_connect_token( &signed_fields, &private_fields,
                                                         rocketnet_test_backend_secret_key, token_data ) );

    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    rocketnet_write_connection_request_packet( request, tag, NULL, rocketnet_test_salt,
                                               rocketnet_test_client_eph_public,
                                               token.signed_connect_data, token.signature );

    const uint64_t cookie_expire = ROCKETNET_TEST_CREATE_TS + 3;
    uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES];
    rocketnet_unit_check( rocketnet_generate_cookie_mac( cookie_mac, address, rocketnet_test_salt,
                                                         cookie_expire, rocketnet_test_cookie_key ) );

    uint8_t * cookie = request + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint64( &cookie, cookie_expire );
    memcpy( cookie, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
}

static void timing_check_family_sums( const struct rocketnet_counters_t * counters )
{
    for ( int received = 0; received < ROCKETNET_NUM_COUNTERS; received++ )
    {
        uint64_t outcomes = 0;
        int is_family = 0;
        for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        {
            if ( counter_families[i] == received )
            {
                outcomes += counters->values[i];
                is_family = 1;
            }
        }

        if ( is_family && counters->values[received] != outcomes )
        {
            printf( "%s received=%llu outcomes=%llu\n", counter_names[received],
                    (unsigned long long) counters->values[received], (unsigned long long) outcomes );
        }
        if ( is_family )
            rocketnet_unit_check( counters->values[received] == outcomes );
    }
}

static void test_standard_t17_handshake_retransmits_at_send_rate()
{
    struct timing_server_fixture_t test;
    timing_server_setup( &test );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    timing_mint_token( token_data, ROCKETNET_TEST_CLIENT_ID );

    const double t0 = 100.0;
    rocketnet_unit_check( rocketnet_client_protocol_connect( &client, token_data, t0, ROCKETNET_TEST_CREATE_TS ) );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );
    rocketnet_unit_check( client.send_rate == ROCKETNET_DEFAULT_SEND_RATE );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // the first update sends immediately; updates inside one send interval
    // send nothing, so the cadence is elapsed time, not the update count

    rocketnet_client_protocol_update( &client, t0, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_unit_check( packet[0] == ROCKETNET_CONNECTION_REQUEST_PACKET );

    rocketnet_client_protocol_update( &client, t0 + 0.05, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, t0 + 0.099, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );

    // one send interval later the request is retransmitted

    rocketnet_client_protocol_update( &client, t0 + 0.101, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_unit_check( packet[0] == ROCKETNET_CONNECTION_REQUEST_PACKET );

    // the cookie moves the client to sending connection request; a request
    // inside the interval is suppressed, and at the interval it is retransmitted

    rocketnet_unit_check( timing_server_process( &test, packet, packet_bytes, t0 + 0.101,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_COOKIE );
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, response, response_bytes, t0 + 0.101,
                                                                   payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );

    rocketnet_client_protocol_update( &client, t0 + 0.152, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, t0 + 0.203, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    rocketnet_client_protocol_update( &client, t0 + 0.254, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, t0 + 0.305, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );

    // the challenge moves the client to sending connection response; the
    // stored response is retransmitted at the same send rate, byte-identical

    rocketnet_unit_check( timing_server_process( &test, packet, packet_bytes, t0 + 0.305,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( rocketnet_client_protocol_process_packet( &client, response, response_bytes, t0 + 0.305,
                                                                   payload, sizeof( payload ), &payload_bytes ) == 1 );
    rocketnet_unit_check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE );

    rocketnet_client_protocol_update( &client, t0 + 0.356, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, t0 + 0.407, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_unit_check( packet[0] == ROCKETNET_CONNECTION_RESPONSE_PACKET );

    uint8_t first_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];
    memcpy( first_response, packet, sizeof( first_response ) );

    rocketnet_client_protocol_update( &client, t0 + 0.458, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == 0 );
    rocketnet_client_protocol_update( &client, t0 + 0.509, packet, &packet_bytes );
    rocketnet_unit_check( packet_bytes == ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    rocketnet_unit_check( memcmp( first_response, packet, sizeof( first_response ) ) == 0 );

    timing_check_family_sums( &counters );
    timing_check_family_sums( &test.counters );

    timing_server_teardown( &test );
}

// ---------------------------------------------------------------------------
// Behavioral t17: the server handshake deadline is fixed when the mapping is
// created. A repeated matching request refreshes the half-open idle deadline
// but never the handshake deadline, so the mapping still leaves at creation
// plus the handshake deadline.
// ---------------------------------------------------------------------------

static void test_standard_t17_handshake_deadline_is_not_refreshed()
{
    struct timing_server_fixture_t test;
    timing_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    timing_build_request( request, &test.client_address );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;

    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ),
                                                                                ROCKETNET_TEST_CREATE_TS, 1000.0,
                                                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    int index = rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address );
    rocketnet_unit_check( index != -1 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    rocketnet_unit_check( mapping->control->established == 0 );
    rocketnet_unit_check( mapping->control->handshake_deadline == 1000.0 + ROCKETNET_DEFAULT_HANDSHAKE_DEADLINE_SECONDS );
    rocketnet_unit_check( mapping->control->last_receive_time == 1000.0 );

    const double created_deadline = mapping->control->handshake_deadline;

    // a matching request four seconds in refreshes the idle clock, not the
    // handshake deadline

    rocketnet_unit_check( rocketnet_server_protocol_process_connection_request( &test.server, &test.client_address,
                                                                                request, sizeof( request ),
                                                                                ROCKETNET_TEST_CREATE_TS, 1004.0,
                                                                                response, &response_bytes ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );

    mapping = rocketnet_encryption_map_entry( test.server.encryption_map, index );
    rocketnet_unit_check( mapping->control->last_receive_time == 1004.0 );
    rocketnet_unit_check( mapping->control->handshake_deadline == created_deadline );

    // before the fixed deadline the mapping still lives, even though the idle
    // deadline would now be four seconds later

    rocketnet_server_protocol_update( &test.server, 1004.9 );
    rocketnet_unit_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == index );

    // at the fixed deadline it leaves, counted as the handshake expiry

    rocketnet_server_protocol_update( &test.server, created_deadline );
    rocketnet_unit_check( rocketnet_encryption_map_find( test.server.encryption_map, &test.client_address ) == -1 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED] == 1 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT] == 0 );

    timing_check_family_sums( &test.counters );

    timing_server_teardown( &test );
}

// ---------------------------------------------------------------------------
// Behavioral t18: every packet type's RECEIVED counter equals the sum of its
// family's outcome counters. One request is accepted, one is answered from the
// stored challenge, one is refused for size, and each sealed type is refused
// for size; the families then balance across the whole registry.
// ---------------------------------------------------------------------------

static void test_standard_t18_received_equals_family_outcomes()
{
    struct timing_server_fixture_t test;
    timing_server_setup( &test );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    timing_build_request( request, &test.client_address );

    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int payload_bytes = 0;
    int payload_client_index = -1;

    rocketnet_unit_check( timing_server_process( &test, request, sizeof( request ), 1000.0,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( timing_server_process( &test, request, sizeof( request ), 1000.5,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE );
    rocketnet_unit_check( timing_server_process( &test, request, ROCKETNET_PREFILTER_BYTES, 1001.0,
                                                 response, &response_bytes,
                                                 payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );

    const uint8_t sealed_types[] = {
        ROCKETNET_CONNECTION_RESPONSE_PACKET,
        ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
        ROCKETNET_CONNECTION_PAYLOAD_PACKET,
        ROCKETNET_CONNECTION_DISCONNECT_PACKET,
        ROCKETNET_CONNECTION_ACK_PACKET,
    };
    for ( int i = 0; i < (int) ( sizeof( sealed_types ) / sizeof( sealed_types[0] ) ); i++ )
    {
        uint8_t short_packet[ROCKETNET_PREFILTER_BYTES];
        uint8_t * p = short_packet;
        rocketnet_write_prefilter_header( &p, sealed_types[i], test.server.packet_filter );

        rocketnet_unit_check( timing_server_process( &test, short_packet, sizeof( short_packet ), 1002.0,
                                                     response, &response_bytes,
                                                     payload, &payload_bytes, &payload_client_index ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    }

    // the request family: one sent challenge, one challenge resent, one size
    // refusal, against three arrivals

    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == 3 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE] == 1 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] == 1 );
    rocketnet_unit_check( test.counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] == 1 );

    timing_check_family_sums( &test.counters );

    timing_server_teardown( &test );
}

REGISTER_UNIT_TEST( test_standard_timing_and_counters_vectors );
REGISTER_UNIT_TEST( test_standard_t17_handshake_retransmits_at_send_rate );
REGISTER_UNIT_TEST( test_standard_t17_handshake_deadline_is_not_refreshed );
REGISTER_UNIT_TEST( test_standard_t18_received_equals_family_outcomes );
