/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The refusals of the shared sealing and cookie code (sdk/rocketnet_protocol.cpp),
// the failed-signature budget's table and window, the cookie reply bitmap's
// index refusals and the ack reference of a protocol built without ack
// bookkeeping (sdk/rocketnet_server_protocol.cpp), each called directly with
// the argument that must be refused. No sockets, no real time and no threads:
// timestamps are arguments and start at 1000.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_address.h"
#include "rocketnet_server.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_wire.h"
#include "rocketnet_constants.h"

#include <sodium.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static void r2hs_fault_if( bool fault, const char * text )
{
    if ( fault )
    {
        printf( "fault: %s\n", text );
    }
    check( !fault );
}

static const uint8_t r2hs_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x11, 0x22, 0x33, 0x44 };
static const uint8_t r2hs_salt[ROCKETNET_CONNECTION_SALT_BYTES] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint64_t r2hs_protocol_id = 0x1122334455667788ULL;

static void r2hs_key( uint8_t * key, uint8_t value )
{
    memset( key, value, ROCKETNET_KEY_BYTES );
}

// mutation gap: hunt 2/3391
// mutation gap: hunt 2/3392
//
// A session counter at 0 means no session: the take hands out 0 and leaves the
// counter at 0, so nothing is ever sealed under a sequence taken before the
// session began. A counter that is running hands out its value and moves on.

static void test_gaps_r2_hs_session_sequence_take_at_zero()
{
    uint64_t next = 0;
    const uint64_t taken = rocketnet_session_sequence_take( &next );
    r2hs_fault_if( taken != 0, "the take handed out a session sequence from a counter at 0 (no session)" );
    r2hs_fault_if( next != 0, "the take advanced a counter at 0 (no session)" );

    next = 5;
    check( rocketnet_session_sequence_take( &next ) == 5 );
    check( next == 6 );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_session_sequence_take_at_zero );

// mutation gap: hunt 2/3405
// mutation gap: hunt 2/3406
// mutation gap: hunt 2/3414
//
// The in-place payload sealer refuses sequence 0 (the nonce no peer accepts)
// and a payload of 0 bytes or one byte more than the send budget leaves room
// for beside the block, and a refusal writes nothing into the wire buffer.
// The same sealer seals a 4-byte payload under sequence 1.

static void test_gaps_r2_hs_sealed_payload_in_place_refusals()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    r2hs_key( key, 0x42 );

    uint8_t block[ROCKETNET_ACK_BLOCK_MIN_BYTES] = { 0 };

    const int frame_bytes = ROCKETNET_SEND_BUDGET + 64;
    uint8_t * wire = (uint8_t *) malloc( (size_t) frame_bytes );
    uint8_t * before = (uint8_t *) malloc( (size_t) frame_bytes );
    check( wire != NULL && before != NULL );
    memset( wire, 0xA5, (size_t) frame_bytes );
    memcpy( before, wire, (size_t) frame_bytes );

    int bytes = rocketnet_write_sealed_payload_in_place( wire, r2hs_filter, 0, block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 4,
                                                         r2hs_protocol_id, r2hs_salt, key );
    r2hs_fault_if( bytes != 0, "the in-place sealer sealed a payload under sequence 0" );
    r2hs_fault_if( memcmp( wire, before, (size_t) frame_bytes ) != 0, "the in-place sealer wrote the wire buffer for sequence 0" );

    bytes = rocketnet_write_sealed_payload_in_place( wire, r2hs_filter, 1, block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 0,
                                                     r2hs_protocol_id, r2hs_salt, key );
    r2hs_fault_if( bytes != 0, "the in-place sealer sealed a 0-byte payload" );
    r2hs_fault_if( memcmp( wire, before, (size_t) frame_bytes ) != 0, "the in-place sealer wrote the wire buffer for a 0-byte payload" );

    const int too_big = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_ACK_BLOCK_MIN_BYTES + 1;
    bytes = rocketnet_write_sealed_payload_in_place( wire, r2hs_filter, 1, block, ROCKETNET_ACK_BLOCK_MIN_BYTES, too_big,
                                                     r2hs_protocol_id, r2hs_salt, key );
    r2hs_fault_if( bytes != 0, "the in-place sealer sealed a payload one byte past the send budget" );
    r2hs_fault_if( memcmp( wire, before, (size_t) frame_bytes ) != 0, "the in-place sealer wrote the wire buffer for a payload past the send budget" );

    bytes = rocketnet_write_sealed_payload_in_place( wire, r2hs_filter, 1, block, ROCKETNET_ACK_BLOCK_MIN_BYTES, 4,
                                                     r2hs_protocol_id, r2hs_salt, key );
    check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 );

    free( before );
    free( wire );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_sealed_payload_in_place_refusals );

// mutation gap: hunt 2/3451
// mutation gap: hunt 2/3465
// mutation gap: hunt 2/3466
//
// The sealer refuses a call whose client ephemeral key disagrees with the
// type (a response without one, a keep-alive with one) and a call whose data
// and length disagree (0 bytes with data, a negative length, data missing),
// writing nothing for any of them.

static void test_gaps_r2_hs_sealed_packet_argument_refusals()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    r2hs_key( key, 0x43 );
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    r2hs_key( ephemeral, 0x44 );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t before[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0xA5, sizeof( packet ) );
    memcpy( before, packet, sizeof( packet ) );

    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t padding[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };

    int bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_RESPONSE_PACKET, r2hs_filter, 1, NULL,
                                               padding, (int) sizeof( padding ), r2hs_protocol_id, r2hs_salt, NULL, key );
    r2hs_fault_if( bytes != 0, "the sealer sealed a response without the client ephemeral key its associated data binds" );

    bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, r2hs_filter, 1, NULL,
                                           data, (int) sizeof( data ), r2hs_protocol_id, r2hs_salt, ephemeral, key );
    r2hs_fault_if( bytes != 0, "the sealer sealed a keep-alive with a client ephemeral key its type does not bind" );
    r2hs_fault_if( memcmp( packet, before, sizeof( packet ) ) != 0, "the sealer wrote a packet whose ephemeral key disagrees with its type" );

    bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, r2hs_filter, 1, NULL,
                                           data, 0, r2hs_protocol_id, r2hs_salt, NULL, key );
    r2hs_fault_if( bytes != 0, "the sealer sealed 0 data bytes with a data pointer given" );

    bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, r2hs_filter, 1, NULL,
                                           data, -1, r2hs_protocol_id, r2hs_salt, NULL, key );
    r2hs_fault_if( bytes != 0, "the sealer sealed a negative data length" );

    bytes = rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, r2hs_filter, 1, NULL,
                                           NULL, 4, r2hs_protocol_id, r2hs_salt, NULL, key );
    r2hs_fault_if( bytes != 0, "the sealer sealed 4 data bytes from a NULL data pointer" );
    r2hs_fault_if( memcmp( packet, before, sizeof( packet ) ) != 0, "the sealer wrote a packet whose data and length disagree" );

    // the same calls with matching arguments seal

    check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_RESPONSE_PACKET, r2hs_filter, 1, NULL, padding,
                                          (int) sizeof( padding ), r2hs_protocol_id, r2hs_salt, ephemeral, key ) ==
           ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES );
    check( rocketnet_write_sealed_packet( packet, ROCKETNET_CONNECTION_DISCONNECT_PACKET, r2hs_filter, 1, NULL, NULL, 0,
                                          r2hs_protocol_id, r2hs_salt, NULL, key ) == ROCKETNET_PACKET_OVERHEAD_BYTES );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_sealed_packet_argument_refusals );

// Seals data by hand, the way rocketnet_write_sealed_packet does, under the
// associated data of any type byte and any ephemeral key: the open's refusals
// are then shown to refuse bytes that WOULD open if the refusal were gone.

static int r2hs_seal_by_hand( uint8_t * packet, uint8_t packet_type, uint64_t sequence, const uint8_t * data, int data_bytes,
                              const uint8_t * ephemeral, const uint8_t * key )
{
    memset( packet, 0, ROCKETNET_PREFILTER_BYTES );
    packet[0] = packet_type;
    uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    rocketnet_write_uint32( &p, (uint32_t) ( sequence & 0xFFFFFFFF ) );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    const int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, r2hs_protocol_id, packet_type,
                                                                              r2hs_salt, ephemeral );

    uint8_t nonce[12];
    memset( nonce, 0, 4 );
    uint8_t * q = nonce + 4;
    rocketnet_write_uint64( &q, sequence );

    unsigned long long sealed_bytes = 0;
    check( crypto_aead_chacha20poly1305_ietf_encrypt( p, &sealed_bytes, data, (unsigned long long) data_bytes, associated_data,
                                                      (unsigned long long) associated_data_bytes, NULL, nonce, key ) == 0 );
    return (int) ( p - packet ) + (int) sealed_bytes;
}

// mutation gap: hunt 2/3482
// mutation gap: hunt 2/3483
// mutation gap: hunt 2/3484
// mutation gap: hunt 2/3501
// mutation gap: hunt 2/3502
//
// The open refuses with exactly -1 an unsealed type (a connection request)
// and a call whose ephemeral key disagrees with the type (a response opened
// without one), even when the bytes were sealed under exactly the associated
// data the refused call would build. The hand seal opens under its own type.

static void test_gaps_r2_hs_open_sealed_packet_refusals()
{
    uint8_t key[ROCKETNET_KEY_BYTES];
    r2hs_key( key, 0x45 );

    const uint8_t data[ROCKETNET_RESPONSE_PADDING_BYTES] = { 0 };
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t opened[ROCKETNET_RECEIVE_CEILING];

    // a "request" sealed under the request type's own associated data

    int packet_bytes = r2hs_seal_by_hand( packet, ROCKETNET_CONNECTION_REQUEST_PACKET, 7, data, (int) sizeof( data ), NULL, key );
    int result = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                               ROCKETNET_CONNECTION_REQUEST_PACKET, 7, r2hs_protocol_id, r2hs_salt, NULL, key );
    if ( result != -1 )
    {
        printf( "fault: the open of an unsealed type (connection request) returned %d, not -1\n", result );
    }
    check( result == -1 );

    // a response sealed under the 21-byte associated data, which binds no
    // ephemeral key, opened without the key a response must bind

    packet_bytes = r2hs_seal_by_hand( packet, ROCKETNET_CONNECTION_RESPONSE_PACKET, 7, data, (int) sizeof( data ), NULL, key );
    result = rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                           ROCKETNET_CONNECTION_RESPONSE_PACKET, 7, r2hs_protocol_id, r2hs_salt, NULL, key );
    if ( result != -1 )
    {
        printf( "fault: the open of a response without its client ephemeral key returned %d, not -1\n", result );
    }
    check( result == -1 );

    // a keep-alive sealed by hand opens: the hand seal is the sealer's

    packet_bytes = r2hs_seal_by_hand( packet, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 7, data, 15, NULL, key );
    check( rocketnet_open_sealed_packet( opened, NULL, (int) sizeof( opened ), packet, packet_bytes,
                                         ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, 7, r2hs_protocol_id, r2hs_salt, NULL, key ) == 15 );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_open_sealed_packet_refusals );

// mutation gap: hunt 2/3598
// mutation gap: hunt 2/3603
// mutation gap: hunt 2/3604
// mutation gap: hunt 2/3610
// mutation gap: hunt 2/3611
//
// An address that is neither IPv4 nor IPv6 has no encoding: its address bytes,
// its cookie mac input and its cookie mac are each refused with 0, and the
// mac is not written.

static void test_gaps_r2_hs_none_address_has_no_cookie_mac()
{
    struct rocketnet_address_t none;
    memset( &none, 0, sizeof( none ) );
    none.type = ROCKETNET_ADDRESS_NONE;
    none.port = 40000;

    uint8_t output[ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES + 16];
    memset( output, 0xA5, sizeof( output ) );
    int bytes = rocketnet_write_address_bytes( output, &none );
    r2hs_fault_if( bytes != 0, "a NONE address was given an address encoding" );

    bytes = rocketnet_write_cookie_mac_input( output, &none, r2hs_salt, 1003 );
    r2hs_fault_if( bytes != 0, "a NONE address was given a cookie mac input" );

    uint8_t cookie_key[ROCKETNET_COOKIE_KEY_BYTES];
    memset( cookie_key, 0x46, sizeof( cookie_key ) );
    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    uint8_t mac_before[ROCKETNET_COOKIE_MAC_BYTES];
    memset( mac, 0xA5, sizeof( mac ) );
    memcpy( mac_before, mac, sizeof( mac ) );
    const int made = rocketnet_generate_cookie_mac( mac, &none, r2hs_salt, 1003, cookie_key );
    r2hs_fault_if( made != 0, "a cookie mac was generated for a NONE address" );
    r2hs_fault_if( memcmp( mac, mac_before, sizeof( mac ) ) != 0, "a cookie mac was written for a NONE address" );

    // an IPv4 address has all three

    struct rocketnet_address_t ipv4;
    check( rocketnet_address_parse( &ipv4, "198.51.100.7:56789" ) );
    check( rocketnet_write_address_bytes( output, &ipv4 ) == 7 );
    check( rocketnet_write_cookie_mac_input( output, &ipv4, r2hs_salt, 1003 ) == 7 + ROCKETNET_CONNECTION_SALT_BYTES + 8 );
    check( rocketnet_generate_cookie_mac( mac, &ipv4, r2hs_salt, 1003, cookie_key ) == 1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_none_address_has_no_cookie_mac );

// mutation gap: hunt 2/1099
// mutation gap: hunt 2/1100
// mutation gap: hunt 2/1105
// mutation gap: hunt 2/1106
//
// The cookie reply bitmap's index is -1 (the caller drops the request) for a
// bitmap whose word count is not a power of two and for an address with no
// encoding; a good address on a good bitmap lands inside it.

static void test_gaps_r2_hs_cookie_reply_index_refusals()
{
    struct rocketnet_server_protocol_t * server = (struct rocketnet_server_protocol_t *) calloc( 1, sizeof( struct rocketnet_server_protocol_t ) );
    check( server != NULL );
    uint64_t words[4] = { 0, 0, 0, 0 };
    server->cookie_reply_sources = words;
    memset( server->cookie_reply_source_key, 0x47, sizeof( server->cookie_reply_source_key ) );

    struct rocketnet_address_t ipv4;
    check( rocketnet_address_parse( &ipv4, "198.51.100.7:56789" ) );

    server->cookie_reply_source_words = 3;
    int index = rocketnet_server_protocol_cookie_reply_source_index( server, &ipv4 );
    if ( index != -1 )
    {
        printf( "fault: a cookie reply bitmap of 3 words (not a power of two) gave index %d, not -1\n", index );
    }
    check( index == -1 );

    server->cookie_reply_source_words = 4;
    struct rocketnet_address_t none;
    memset( &none, 0, sizeof( none ) );
    none.type = ROCKETNET_ADDRESS_NONE;
    index = rocketnet_server_protocol_cookie_reply_source_index( server, &none );
    if ( index != -1 )
    {
        printf( "fault: a NONE address gave cookie reply index %d, not -1\n", index );
    }
    check( index == -1 );

    index = rocketnet_server_protocol_cookie_reply_source_index( server, &ipv4 );
    check( index >= 0 && index < 4 * 64 );

    free( server );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_cookie_reply_index_refusals );

static void r2hs_host( struct rocketnet_address_t * address, int ordinal )
{
    char text[64];
    snprintf( text, sizeof( text ), "10.%d.%d.1:%d", ( ordinal >> 8 ) & 255, ordinal & 255, 50000 + ordinal );
    check( rocketnet_address_parse( address, text ) );
}

// mutation gap: hunt 2/1074
// mutation gap: hunt 2/1078
//
// A source's second failure is counted on its own entry: the entry in slot 0
// reads 2 and no second entry is made for the same host.

static void test_gaps_r2_hs_verify_budget_repeat_failure_counts_on_its_entry()
{
    struct rocketnet_signature_verify_budget_t * budget =
        (struct rocketnet_signature_verify_budget_t *) calloc( 1, sizeof( struct rocketnet_signature_verify_budget_t ) );
    check( budget != NULL );

    struct rocketnet_address_t source;
    r2hs_host( &source, 1 );

    rocketnet_signature_verify_budget_record_failure( budget, &source, 1000 );
    source.port++;      // the same host from another port is the same source
    rocketnet_signature_verify_budget_record_failure( budget, &source, 1000 );

    check( budget->failure_count == 2 );
    r2hs_fault_if( budget->sources[1].fail_count != 0, "a repeat failure from the source in slot 0 made a second entry for the same host" );
    r2hs_fault_if( budget->sources[0].fail_count != 2, "a repeat failure from the source in slot 0 was not counted on its entry" );

    free( budget );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_verify_budget_repeat_failure_counts_on_its_entry );

// mutation gap: hunt 2/1079
//
// With all ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS entries held, a failure
// from one more source is counted in the global total and tracked nowhere:
// in particular the bytes just past the table, inside the same allocation and
// invisible to ASan, are left as they were. The planted guard there looks
// like a free entry (fail count 0), the shape an over-run scan would take.

static void test_gaps_r2_hs_verify_budget_full_table_writes_nothing_past_it()
{
    const size_t past_offset = offsetof( struct rocketnet_signature_verify_budget_t, sources ) +
                               ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS * sizeof( struct rocketnet_signature_verify_source_t );
    const size_t total = past_offset + sizeof( struct rocketnet_signature_verify_source_t ) + 64;
    uint8_t * block = (uint8_t *) calloc( 1, total + sizeof( struct rocketnet_signature_verify_budget_t ) );
    check( block != NULL );
    struct rocketnet_signature_verify_budget_t * budget = (struct rocketnet_signature_verify_budget_t *) block;

    struct rocketnet_signature_verify_source_t guard;
    memset( &guard, 0xA5, sizeof( guard ) );
    guard.fail_count = 0;
    memset( block + past_offset, 0x5A, total - past_offset );
    memcpy( block + past_offset, &guard, sizeof( guard ) );

    uint8_t * saved = (uint8_t *) malloc( total - past_offset );
    check( saved != NULL );
    memcpy( saved, block + past_offset, total - past_offset );

    struct rocketnet_address_t source;
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        r2hs_host( &source, i );
        rocketnet_signature_verify_budget_record_failure( budget, &source, 1000 );
    }

    struct rocketnet_signature_verify_source_t table[ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS];
    memcpy( table, budget->sources, sizeof( table ) );

    r2hs_host( &source, ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS );
    rocketnet_signature_verify_budget_record_failure( budget, &source, 1000 );

    check( budget->failure_count == ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS + 1 );
    r2hs_fault_if( memcmp( block + past_offset, saved, total - past_offset ) != 0,
                   "a failure recorded with the source table full wrote past the table (sources[ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS])" );
    r2hs_fault_if( memcmp( table, budget->sources, sizeof( table ) ) != 0, "a failure recorded with the source table full changed an entry" );

    free( saved );
    free( block );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_verify_budget_full_table_writes_nothing_past_it );

// mutation gap: hunt 2/1052
//
// The window's rollover clears every source's count with the global one: a
// source that spent the budget in one window and has not failed since is
// verified in a later window, even after other sources spend that window's
// budget.

static void test_gaps_r2_hs_verify_budget_window_rollover_clears_sources()
{
    struct rocketnet_signature_verify_budget_t * budget =
        (struct rocketnet_signature_verify_budget_t *) calloc( 1, sizeof( struct rocketnet_signature_verify_budget_t ) );
    check( budget != NULL );

    struct rocketnet_address_t a;
    struct rocketnet_address_t b;
    r2hs_host( &a, 1 );
    r2hs_host( &b, 2 );

    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; i++ )
    {
        rocketnet_signature_verify_budget_record_failure( budget, &a, 1000 );
    }
    check( rocketnet_signature_verify_budget_allow( budget, &a, 1000 ) == 0 );

    const uint64_t later = 1000 + (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS;
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET; i++ )
    {
        rocketnet_signature_verify_budget_record_failure( budget, &b, later );
    }
    check( rocketnet_signature_verify_budget_allow( budget, &b, later ) == 0 );

    r2hs_fault_if( rocketnet_signature_verify_budget_allow( budget, &a, later ) != 1,
                   "a source's failures from an earlier window survived the rollover: it is refused in a window where it never failed" );

    free( budget );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_verify_budget_window_rollover_clears_sources );

// mutation gap: hunt 2/1760
// mutation gap: hunt 2/1762
// mutation gap: hunt 2/1763
// mutation gap: hunt 2/1764
//
// A protocol built without ack bookkeeping reads an inbound block against the
// highest session sequence taken: the next sequence less one, and 0 when the
// counter is at 0 (no session) or 1 (nothing taken yet).

static void test_gaps_r2_hs_highest_sent_without_acks()
{
    struct rocketnet_server_protocol_t * server = (struct rocketnet_server_protocol_t *) calloc( 1, sizeof( struct rocketnet_server_protocol_t ) );
    struct rocketnet_encryption_mapping_t * mapping = (struct rocketnet_encryption_mapping_t *) calloc( 1, sizeof( struct rocketnet_encryption_mapping_t ) );
    check( server != NULL && mapping != NULL );
    check( server->acks == NULL );

    mapping->send_sequence_session = 5;
    uint64_t highest = rocketnet_server_protocol_highest_sent_session( server, 0, mapping );
    if ( highest != 4 )
    {
        printf( "fault: with next session sequence 5 and no ack bookkeeping the ack reference is %llu, not 4\n", (unsigned long long) highest );
    }
    check( highest == 4 );

    mapping->send_sequence_session = 1;
    highest = rocketnet_server_protocol_highest_sent_session( server, 0, mapping );
    if ( highest != 0 )
    {
        printf( "fault: with next session sequence 1 and no ack bookkeeping the ack reference is %llu, not 0\n", (unsigned long long) highest );
    }
    check( highest == 0 );

    mapping->send_sequence_session = 0;
    highest = rocketnet_server_protocol_highest_sent_session( server, 0, mapping );
    if ( highest != 0 )
    {
        printf( "fault: with the session counter at 0 and no ack bookkeeping the ack reference is %llu, not 0\n", (unsigned long long) highest );
    }
    check( highest == 0 );

    free( mapping );
    free( server );
}

REGISTER_UNIT_TEST( test_gaps_r2_hs_highest_sent_without_acks );
