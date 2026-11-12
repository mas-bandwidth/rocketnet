/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_transport_appendices.cpp -- RN-1 tc
// (standard/TRANSPORT.md, Appendix C), and properties of RN-1 that cross
// sections, each check naming the section that states it.
//
// The appendix has no known-answer vector block, so the twin table stays
// empty and the walk still accepts that. The checks below are the note and
// those properties: a broken constant or rule fails this case.

#include "test_unit_standard.h"
#include "test_env_source.h"

#include "rocketnet_allocator.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_keys.h"
#include "rocketnet_layers.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_frame.h"

#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

#include <stdio.h>
#include <string.h>

static int standard_transport_appendices_generate( const char * id, uint8_t * out, int capacity )
{
    (void) id;
    (void) out;
    (void) capacity;
    return -1;
}

static uint64_t appendix_align64( uint64_t value )
{
    const uint64_t alignment = 64;
    return ( value + alignment - 1 ) & ~( alignment - 1 );
}

static const struct rocketnet_config_field_t * server_field( const struct rocketnet_config_field_t * fields, int num_fields, const char * name )
{
    for ( int i = 0; i < num_fields; i++ )
    {
        if ( strcmp( fields[i].name, name ) == 0 )
        {
            return &fields[i];
        }
    }

    return NULL;
}

static int server_config_int( const struct rocketnet_server_transport_config_t * config, const struct rocketnet_config_field_t * field )
{
    int value = 0;
    memcpy( &value, (const uint8_t *) config + field->offset, sizeof( value ) );
    return value;
}

static void server_config_set_int( struct rocketnet_server_transport_config_t * config, const struct rocketnet_config_field_t * field, int value )
{
    memcpy( (uint8_t *) config + field->offset, &value, sizeof( value ) );
}

static int append_transport_name( char * list, int capacity, const char * name )
{
    const int used = (int) strlen( list );
    if ( used >= capacity )
    {
        return -1;
    }

    const int wrote = snprintf( list + used, (size_t) ( capacity - used ), "%s%s", used == 0 ? "" : ",", name );
    if ( wrote < 0 || wrote >= capacity - used )
    {
        return -1;
    }

    return 0;
}

// Appendix C create-time reserve, from the literals in that note:
// R = ceil(kbps * 125 * delay_ms / 1000), the larger of the two budgets;
// Q adds ceil(R/128); the 128/512/1152 pools add ceil(R/128), ceil(R/512)
// and ceil(R/1024), each chunk carrying a four-byte link, aligned to 64;
// the span adds R plus 15 bytes per 1153 payload bytes, aligned to 64.
static void check_resend_reserve( int client_kbps, int server_kbps, int delay_ms, int queue, int receive_bytes, int send_bytes )
{
    struct rocketnet_message_config_t config;
    memset( &config, 0, sizeof( config ) );
    config.client_send_kbps = client_kbps;
    config.server_send_kbps = server_kbps;
    config.message_resend_delay_max_ms = delay_ms;
    config.send_queue_messages = queue;
    config.receive_message_buffer_bytes = receive_bytes;
    config.send_message_buffer_bytes = send_bytes;

    const int kbps = client_kbps > server_kbps ? client_kbps : server_kbps;
    const uint64_t reserve = kbps > 0 ? ( (uint64_t) kbps * 125u * (uint64_t) delay_ms + 999u ) / 1000u : 0u;
    const uint64_t descriptors = (uint64_t) queue + ( reserve + 128u - 1u ) / 128u;
    const uint64_t span = (uint64_t) receive_bytes + appendix_align64( reserve + ( ( reserve + 1152u ) / 1153u ) * 15u );
    const uint32_t pool_bytes[3] = { 128u, 512u, 1152u };
    const uint32_t typical_payload[3] = { 128u, 512u, 1024u };
    uint32_t chunks[3];
    uint64_t store = 0;

    rocketnet_unit_check( RN_STORE_NUM_CLASSES == 3 );
    for ( int c = 0; c < 3; c++ )
    {
        const uint64_t count = (uint64_t) rn_send_store_chunks_for( (uint64_t) send_bytes, c ) + ( reserve + typical_payload[c] - 1u ) / typical_payload[c];
        chunks[c] = (uint32_t) count;
        store += appendix_align64( count * ( (uint64_t) pool_bytes[c] + 4u ) );
    }

    if ( store < (uint64_t) send_bytes )
    {
        store = (uint64_t) send_bytes;
    }

    struct rn_region_reserve_sizes_t sizes;
    memset( &sizes, 0, sizeof( sizes ) );
    rocketnet_unit_check( rn_region_reserve_sizes( &config, &sizes ) );
    rocketnet_unit_check( sizes.Q == descriptors );
    rocketnet_unit_check( sizes.SR == span );
    rocketnet_unit_check( sizes.S == store );
    rocketnet_unit_check( sizes.chunks[RN_STORE_SMALL] == chunks[0] );
    rocketnet_unit_check( sizes.chunks[RN_STORE_MEDIUM] == chunks[1] );
    rocketnet_unit_check( sizes.chunks[RN_STORE_LARGE] == chunks[2] );
}

static void test_standard_transport_appendices_vectors()
{
    const int count = rn_standard_vectors_walk( NULL, 0, standard_transport_appendices_generate );
    rocketnet_unit_check( count == 0 );

    uint8_t scratch[4];
    rocketnet_unit_check( standard_transport_appendices_generate( "tc.no-such-vector", scratch, (int) sizeof( scratch ) ) == -1 );

    // t2.2. Backend trust: Ed25519 public key on the server, seal is X25519, no shared secret.
    rocketnet_unit_check( ROCKETNET_SIGNATURE_BYTES == crypto_sign_BYTES );
    rocketnet_unit_check( sizeof( ( (struct rocketnet_server_transport_config_t *) 0 )->backend_public_key ) == (size_t) crypto_sign_PUBLICKEYBYTES );
    rocketnet_unit_check( sizeof( ( (struct rocketnet_server_transport_config_t *) 0 )->server_public_key ) == (size_t) crypto_scalarmult_BYTES );
    rocketnet_unit_check( ROCKETNET_SEALED_BLOCK_BYTES == ROCKETNET_PRIVATE_BLOCK_BYTES + crypto_box_SEALBYTES );
    rocketnet_unit_check( ROCKETNET_KEY_BYTES == crypto_box_PUBLICKEYBYTES );

    // t6. Token authenticity: detached Ed25519 over the signed block, private block sealed to the server.
    rocketnet_unit_check( crypto_sign_BYTES == 64 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES <= ROCKETNET_SIGNED_CONNECT_DATA_BYTES );

    // t2. Token scope: one server, one optional IPv4 and one optional IPv6, contiguous, no server list.
    rocketnet_unit_check( ROCKETNET_ADDRESS_FLAG_IPV4 == 0x01 );
    rocketnet_unit_check( ROCKETNET_ADDRESS_FLAG_IPV6 == 0x02 );
    rocketnet_unit_check( ( ROCKETNET_ADDRESS_FLAG_IPV4 & ROCKETNET_ADDRESS_FLAG_IPV6 ) == 0 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS == 32 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_IPV4 == 33 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_IPV6 == ROCKETNET_SIGNED_OFFSET_IPV4 + 6 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_SERVER_KEY == ROCKETNET_SIGNED_OFFSET_IPV6 + 18 );
    rocketnet_unit_check( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK == ROCKETNET_SIGNED_OFFSET_SERVER_KEY + ROCKETNET_KEY_BYTES );
    rocketnet_unit_check( sizeof( ( (struct rocketnet_signed_connect_data_t *) 0 )->ipv4_address ) == 6 );
    rocketnet_unit_check( sizeof( ( (struct rocketnet_signed_connect_data_t *) 0 )->ipv6_address ) == 18 );
    rocketnet_unit_check( sizeof( ( (struct rocketnet_signed_connect_data_t *) 0 )->server_public_key ) == 32 );

    // t12. Token binding: a token already in use is refused. One live connection.
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED ) == ROCKETNET_COUNTER_CLASS_REFUSAL );

    // t7. Packet keys: token keys only key a derivation. Handshake keys differ from them.
    // A small-order ephemeral public key refuses the whole derivation.
    struct rocketnet_connection_keys_t keys;
    memset( &keys, 0, sizeof( keys ) );
    rocketnet_unit_check( rocketnet_derive_connection_keys( &keys,
                                                            rocketnet_test_token_c2s_key,
                                                            rocketnet_test_token_s2c_key,
                                                            rocketnet_test_salt,
                                                            rocketnet_test_session_nonce,
                                                            rocketnet_test_client_eph_secret,
                                                            rocketnet_test_server_eph_public,
                                                            rocketnet_test_client_eph_public,
                                                            rocketnet_test_server_eph_public ) == 1 );
    rocketnet_unit_check( memcmp( keys.handshake_client_to_server, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( keys.handshake_server_to_client, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( keys.handshake_client_to_server, keys.handshake_server_to_client, ROCKETNET_KEY_BYTES ) != 0 );

    uint8_t zero_point[crypto_scalarmult_BYTES];
    memset( zero_point, 0, sizeof( zero_point ) );
    struct rocketnet_connection_keys_t refused;
    memset( &refused, 0, sizeof( refused ) );
    rocketnet_unit_check( rocketnet_derive_connection_keys( &refused,
                                                            rocketnet_test_token_c2s_key,
                                                            rocketnet_test_token_s2c_key,
                                                            rocketnet_test_salt,
                                                            rocketnet_test_session_nonce,
                                                            rocketnet_test_client_eph_secret,
                                                            zero_point,
                                                            rocketnet_test_client_eph_public,
                                                            rocketnet_test_server_eph_public ) == 0 );

    // t7. Forward secrecy: session keys are not the token keys and not the handshake keys.
    rocketnet_unit_check( memcmp( keys.session_client_to_server, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( keys.session_server_to_client, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( keys.session_client_to_server, keys.handshake_client_to_server, ROCKETNET_KEY_BYTES ) != 0 );
    rocketnet_unit_check( memcmp( keys.session_server_to_client, keys.handshake_server_to_client, ROCKETNET_KEY_BYTES ) != 0 );

    // t2. IPv6: dual stack, and an IPv6 header is present and larger than IPv4.
    rocketnet_unit_check( ROCKETNET_ADDRESS_IPV4 != ROCKETNET_ADDRESS_IPV6 );
    rocketnet_unit_check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ) > 0 );
    rocketnet_unit_check( rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV6 ) > rocketnet_wire_header_bytes( ROCKETNET_ADDRESS_IPV4 ) );

    // t5. Packet prefix: every packet begins with the prefilter.
    const int packet_bytes[] = {
        ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES,
        ROCKETNET_CONNECTION_DENIED_WIRE_BYTES,
        ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES,
        ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES,
        ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES,
        ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES,
        ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES,
        ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
    };
    rocketnet_unit_check( ROCKETNET_PREFILTER_BYTES > 0 );
    for ( int i = 0; i < (int) ( sizeof( packet_bytes ) / sizeof( packet_bytes[0] ) ); i++ )
    {
        rocketnet_unit_check( packet_bytes[i] >= ROCKETNET_PREFILTER_BYTES );
    }

    // t13. Payload to an unconfirmed client: no keep-alive prefix. The challenge is its own packet.
    rocketnet_unit_check( ROCKETNET_CONNECTION_PAYLOAD_PACKET != ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );
    rocketnet_unit_check( ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES < ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );
    rocketnet_unit_check( ROCKETNET_CONNECTION_CHALLENGE_PACKET != ROCKETNET_CONNECTION_PAYLOAD_PACKET );
    rocketnet_unit_check( ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES >= ROCKETNET_PREFILTER_BYTES );

    // t2.3. A zero timeout is outside the field and is refused. t2.3: default 60, range 1 to 3600.
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    const struct rocketnet_config_field_t * timeout = server_field( fields, num_fields, "max_timeout_seconds" );
    rocketnet_unit_check( timeout != NULL );
    rocketnet_unit_check( timeout->kind == ROCKETNET_CONFIG_KIND_INT );
    rocketnet_unit_check( timeout->min_value > 0 );
    rocketnet_unit_check( timeout->min_value == 1 );
    rocketnet_unit_check( timeout->max_value == 3600 );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
    rocketnet_unit_check( server_config_int( &server_config, timeout ) == 60 );
    rocketnet_unit_check( rocketnet_config_validate( "server", &server_config, fields, num_fields ) );

    server_config_set_int( &server_config, timeout, (int) timeout->min_value );
    rocketnet_unit_check( server_config_int( &server_config, timeout ) == (int) timeout->min_value );
    rocketnet_unit_check( rocketnet_config_validate( "server", &server_config, fields, num_fields ) );

    char env_name[128];
    rocketnet_unit_check( rocketnet_config_env_var_name_for( "server", "max_timeout_seconds", env_name, (int) sizeof( env_name ) ) );
    rocketnet_unit_check( strcmp( env_name, "ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS" ) == 0 );

    int parsed = 0;
    rocketnet_unit_check( rocketnet_config_parse_int_value( "60", &parsed ) );
    rocketnet_unit_check( parsed == 60 );
    rocketnet_unit_check( !rocketnet_config_parse_int_value( "nope", &parsed ) );

    rocketnet_unit_check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS", "120", 1 ) == 0 );
    rocketnet_unit_check( rocketnet_config_apply_env( "server", &server_config, fields, num_fields ) );
    rocketnet_unit_check( server_config_int( &server_config, timeout ) == 120 );

    rocketnet_unit_check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS", "nope", 1 ) == 0 );
    rocketnet_unit_check( !rocketnet_config_apply_env( "server", &server_config, fields, num_fields ) );
    rocketnet_unit_check( server_config_int( &server_config, timeout ) == 120 );
    rocketnet_unit_check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_MAX_TIMEOUT_SECONDS" ) == 0 );

    server_config_set_int( &server_config, timeout, 0 );
    rocketnet_unit_check( !rocketnet_config_validate( "server", &server_config, fields, num_fields ) );

    // tc. Transport selection: auto is the available order without pin-only transports.
    // An explicit list keeps its order and appends nothing. One applicable name is a pin.
    int available[ROCKETNET_NUM_TRANSPORTS];
    int num_available = 0;
    for ( int i = 0; i < ROCKETNET_NUM_TRANSPORTS; i++ )
    {
        available[num_available++] = i;
    }
    rocketnet_unit_check( num_available > 0 );
    rocketnet_unit_check( num_available <= ROCKETNET_MAX_TRANSPORT_CANDIDATES );

    int auto_ids[ROCKETNET_MAX_TRANSPORT_CANDIDATES];
    int auto_count = 0;
    for ( int i = 0; i < num_available; i++ )
    {
        if ( rocketnet_transport_pin_only( available[i], true ) )
        {
            continue;
        }
        auto_ids[auto_count++] = available[i];
    }
    rocketnet_unit_check( auto_count > 0 );

    struct rocketnet_transport_ladder_t from_null;
    struct rocketnet_transport_ladder_t from_empty;
    struct rocketnet_transport_ladder_t from_word;
    rocketnet_unit_check( rocketnet_transport_ladder_parse( NULL, available, num_available, &from_null, true ) );
    rocketnet_unit_check( rocketnet_transport_ladder_parse( "", available, num_available, &from_empty, true ) );
    rocketnet_unit_check( rocketnet_transport_ladder_parse( "auto", available, num_available, &from_word, true ) );
    rocketnet_unit_check( from_null.num_candidates == auto_count );
    rocketnet_unit_check( from_empty.num_candidates == auto_count );
    rocketnet_unit_check( from_word.num_candidates == auto_count );
    for ( int i = 0; i < auto_count; i++ )
    {
        rocketnet_unit_check( from_null.candidates[i] == auto_ids[i] );
        rocketnet_unit_check( from_empty.candidates[i] == auto_ids[i] );
        rocketnet_unit_check( from_word.candidates[i] == auto_ids[i] );
    }

    const char * pin_name = rocketnet_transport_name( auto_ids[0] );
    rocketnet_unit_check( pin_name != NULL );
    struct rocketnet_transport_ladder_t pinned;
    rocketnet_unit_check( rocketnet_transport_ladder_parse( pin_name, available, num_available, &pinned, true ) );
    rocketnet_unit_check( pinned.num_candidates == 1 );
    rocketnet_unit_check( pinned.candidates[0] == auto_ids[0] );

    char reversed[256];
    reversed[0] = '\0';
    for ( int i = auto_count - 1; i >= 0; i-- )
    {
        const char * name = rocketnet_transport_name( auto_ids[i] );
        rocketnet_unit_check( name != NULL );
        rocketnet_unit_check( append_transport_name( reversed, (int) sizeof( reversed ), name ) == 0 );
    }
    struct rocketnet_transport_ladder_t explicit_list;
    rocketnet_unit_check( rocketnet_transport_ladder_parse( reversed, available, num_available, &explicit_list, true ) );
    rocketnet_unit_check( explicit_list.num_candidates == auto_count );
    for ( int i = 0; i < auto_count; i++ )
    {
        rocketnet_unit_check( explicit_list.candidates[i] == auto_ids[auto_count - 1 - i] );
    }

    // tc. Kernel prefilter: path MTU at least 1500, and the named fragment and options counters.
    rocketnet_unit_check( ROCKETNET_LINK_MTU_BYTES >= 1500 );
    rocketnet_unit_check( ROCKETNET_HANDSHAKE_MAX_WIRE_BYTES <= ROCKETNET_LINK_MTU_BYTES - ROCKETNET_IPV6_HEADER_BYTES - ROCKETNET_UDP_HEADER_BYTES );
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_KERNEL_PASSED_FRAGMENT ) == ROCKETNET_COUNTER_CLASS_EXPECTED );
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_KERNEL_PASSED_IPV6_EXTENSION ) == ROCKETNET_COUNTER_CLASS_EXPECTED );

    // tc. Check placement: userspace still refuses a short prefilter when the kernel does too.
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_PREFILTER_TOO_SHORT ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    rocketnet_unit_check( rocketnet_counter_class( ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL ) == ROCKETNET_COUNTER_CLASS_REFUSAL );
    rocketnet_unit_check( ROCKETNET_COUNTER_PREFILTER_TOO_SHORT != ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL );

    // tc. Configuration surface: the env name is derived, an unparsable value is refused, and it is not clamped.
    // Covered with the timeout field above (default, then struct, then environment).

    // tc. Counters per execution context: a failed readout keeps the last total, and a good readout sums contexts.
    uint64_t last[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
    uint64_t published[ROCKETNET_SERVER_XDP_NUM_COUNTERS];
    memset( last, 0, sizeof( last ) );
    memset( published, 0, sizeof( published ) );
    last[0] = 5;
    bool failing = false;
    rocketnet_unit_check( rocketnet_xdp_stats_publish( false, NULL, 0, last, &failing, published ) );
    rocketnet_unit_check( failing );
    rocketnet_unit_check( published[0] == 5 );
    rocketnet_unit_check( !rocketnet_xdp_stats_publish( false, NULL, 0, last, &failing, published ) );
    rocketnet_unit_check( published[0] == 5 );

    struct rocketnet_server_xdp_stats_t cpus[2];
    memset( cpus, 0, sizeof( cpus ) );
    cpus[0].counters[0] = 3;
    cpus[1].counters[0] = 4;
    rocketnet_unit_check( !rocketnet_xdp_stats_publish( true, cpus, 2, last, &failing, published ) );
    rocketnet_unit_check( !failing );
    rocketnet_unit_check( published[0] == 7 );
    rocketnet_unit_check( last[0] == 7 );

    // tc. Create-time maximum-resend delay message reserve.
    rocketnet_unit_check( RN_STORE_SMALL_BYTES == 128 );
    rocketnet_unit_check( RN_STORE_MEDIUM_BYTES == 512 );
    rocketnet_unit_check( RN_STORE_LARGE_BYTES == 1152 );
    rocketnet_unit_check( ROCKETNET_MESSAGE_FRAGMENT_BYTES == 1152 );
    rocketnet_unit_check( RN_SPAN_MIN_BYTES == 1153 );
    rocketnet_unit_check( ROCKETNET_ALLOCATION_ALIGNMENT == 64 );
    rocketnet_unit_check( ROCKETNET_SEND_KBPS_UNLIMITED > ROCKETNET_SEND_BUDGET_MAX_KBPS );
    rocketnet_unit_check( !rocketnet_send_budget_kbps_valid( (uint32_t) ROCKETNET_SEND_KBPS_UNLIMITED ) );
    check_resend_reserve( 1000, 100, 2000, 5, 64, 0 );
    check_resend_reserve( 100, 1000, 2000, 5, 64, 0 );
    check_resend_reserve( 0, 0, 2000, 5, 64, 0 );
}

REGISTER_UNIT_TEST( test_standard_transport_appendices_vectors );
