/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Round-trip invariants for every wire form: the input chooses field values,
// the real write path encodes them, the real read path decodes them, and the
// decoded values must be exactly the ones written. netcode's fuzz_read_packet
// and fuzz_parse_address carry the same oracle; the message records' codec is
// already held to it by rocketnet_fuzz_message (pack-back of every accepted
// payload), so this target covers everything below that layer.
//
// THE SELECTOR BYTE: input[0] % 8 chooses the form, the rest is its fields
// (short input is zero-filled).
//   0 = the connection request: every field at its offset, the prefilter
//       naming type 0, the fixed wire size
//   1 = the connection cookie: likewise, type 7
//   2 = a sealed packet of a fuzz-chosen sealed type (1-6, 8, 9) under a
//       fuzz-chosen key, salt, protocol id and sequence: the open returns the
//       plaintext and the clear session nonce exactly, the wire carries the
//       sequence's low 32 bits, reconstruction from any highest-opened within
//       2^30 below recovers the sequence, and a change to any sealed byte
//       makes the open refuse
//   3 = an ack block read from fuzz bytes: whatever the reader accepts, the
//       writer re-encodes to the same bytes
//   4 = an ack block built from a fuzz-chosen replay window: the reader
//       accepts it whole, and every sequence it names is one the window
//       holds, at or above the window's floor
//   5 = the fragment header: payload id, index and count survive
//   6 = the signed connect data and the private block: every field survives
//   7 = addresses: text -> parse -> to_string -> parse is the same address,
//       and an address from fuzz bytes -> to_string -> parse is itself
//
// No state survives an input; nothing reads a clock or draws randomness.

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_address.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#include "../portable_crt.h"

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

struct fuzz_reader_t
{
    const uint8_t * data;
    size_t size;
    size_t pos;
};

static uint8_t fuzz_u8( struct fuzz_reader_t * r )
{
    return r->pos < r->size ? r->data[r->pos++] : 0;
}

static uint64_t fuzz_u64( struct fuzz_reader_t * r )
{
    uint64_t v = 0;

    for ( int i = 0; i < 8; i++ )
    {
        v |= (uint64_t) fuzz_u8( r ) << ( 8 * i );
    }

    return v;
}

static void fuzz_bytes( struct fuzz_reader_t * r, uint8_t * out, size_t bytes )
{
    for ( size_t i = 0; i < bytes; i++ )
    {
        out[i] = fuzz_u8( r );
    }
}

static uint64_t fuzz_read_le64( const uint8_t * p )
{
    uint64_t v = 0;

    for ( int i = 0; i < 8; i++ )
    {
        v |= (uint64_t) p[i] << ( 8 * i );
    }

    return v;
}

static const uint8_t fuzz_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;

// -----------------------------------------------------------------------
//  0: the connection request
// -----------------------------------------------------------------------

static void fuzz_request( struct fuzz_reader_t * r )
{
    uint8_t cookie[ROCKETNET_COOKIE_FIELD_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    static uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    const bool with_cookie = ( fuzz_u8( r ) & 1 ) != 0;
    fuzz_bytes( r, cookie, sizeof( cookie ) );
    fuzz_bytes( r, salt, sizeof( salt ) );
    fuzz_bytes( r, ephemeral, sizeof( ephemeral ) );
    fuzz_bytes( r, signed_data, sizeof( signed_data ) );
    fuzz_bytes( r, signature, sizeof( signature ) );

    static uint8_t packet[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( packet, fuzz_filter, with_cookie ? cookie : NULL, salt, ephemeral, signed_data, signature );

    rocketnet_fuzz_require( rocketnet_prefilter_check( packet, (int) sizeof( packet ), fuzz_filter ) == ROCKETNET_CONNECTION_REQUEST_PACKET,
                            "a written request passes the prefilter as type 0" );
    const uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    uint8_t zero_cookie[ROCKETNET_COOKIE_FIELD_BYTES];
    memset( zero_cookie, 0, sizeof( zero_cookie ) );
    rocketnet_fuzz_require( memcmp( p, with_cookie ? cookie : zero_cookie, ROCKETNET_COOKIE_FIELD_BYTES ) == 0, "the request carries its cookie, or zeros" );
    p += ROCKETNET_COOKIE_FIELD_BYTES;
    rocketnet_fuzz_require( memcmp( p, salt, sizeof( salt ) ) == 0, "the request carries its salt" );
    p += sizeof( salt );
    rocketnet_fuzz_require( memcmp( p, ephemeral, sizeof( ephemeral ) ) == 0, "the request carries its ephemeral key" );
    p += sizeof( ephemeral );
    rocketnet_fuzz_require( memcmp( p, signed_data, sizeof( signed_data ) ) == 0, "the request carries the signed data verbatim" );
    p += sizeof( signed_data );
    rocketnet_fuzz_require( memcmp( p, signature, sizeof( signature ) ) == 0, "the request carries the signature" );
    p += sizeof( signature );
    rocketnet_fuzz_require( p == packet + ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, "the request is its fixed wire size" );
}

// -----------------------------------------------------------------------
//  1: the connection cookie
// -----------------------------------------------------------------------

static void fuzz_cookie( struct fuzz_reader_t * r )
{
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t mac[ROCKETNET_COOKIE_MAC_BYTES];
    fuzz_bytes( r, salt, sizeof( salt ) );

    const uint64_t expire = fuzz_u64( r );

    fuzz_bytes( r, mac, sizeof( mac ) );

    uint8_t packet[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    rocketnet_write_connection_cookie_packet( packet, fuzz_filter, salt, expire, mac );
    rocketnet_fuzz_require( rocketnet_prefilter_check( packet, (int) sizeof( packet ), fuzz_filter ) == ROCKETNET_CONNECTION_COOKIE_PACKET,
                            "a written cookie passes the prefilter as type 7" );
    const uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    rocketnet_fuzz_require( memcmp( p, salt, sizeof( salt ) ) == 0, "the cookie carries the salt" );
    p += sizeof( salt );
    rocketnet_fuzz_require( fuzz_read_le64( p ) == expire, "the cookie carries its expire, little-endian" );
    p += 8;
    rocketnet_fuzz_require( memcmp( p, mac, sizeof( mac ) ) == 0, "the cookie carries its mac" );
}

// -----------------------------------------------------------------------
//  2: sealed packets
// -----------------------------------------------------------------------

static const uint8_t fuzz_sealed_types[] = {
    ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_CHALLENGE_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET,
    ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET,
    ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_FRAGMENT_PACKET
};

// the clear session nonce rides types 1 and 2; the client ephemeral key is
// in the associated data of types 2 and 3 (sdk/rocketnet_protocol.h)
static bool fuzz_has_nonce( uint8_t type ) { return type == ROCKETNET_CONNECTION_DENIED_PACKET || type == ROCKETNET_CONNECTION_CHALLENGE_PACKET; }
static bool fuzz_has_ephemeral( uint8_t type ) { return type == ROCKETNET_CONNECTION_CHALLENGE_PACKET || type == ROCKETNET_CONNECTION_RESPONSE_PACKET; }

static void fuzz_sealed( struct fuzz_reader_t * r )
{
    const uint8_t type = fuzz_sealed_types[fuzz_u8( r ) % sizeof( fuzz_sealed_types )];
    uint8_t key[ROCKETNET_KEY_BYTES];
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t nonce[ROCKETNET_SESSION_NONCE_BYTES];
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    fuzz_bytes( r, key, sizeof( key ) );
    fuzz_bytes( r, salt, sizeof( salt ) );
    fuzz_bytes( r, nonce, sizeof( nonce ) );
    fuzz_bytes( r, ephemeral, sizeof( ephemeral ) );

    const uint64_t protocol_id = fuzz_u64( r );
    const uint64_t sequence = fuzz_u64( r );
    const uint64_t behind = fuzz_u64( r ) & ( ( 1ULL << 30 ) - 1 );
    const uint16_t flip_at = (uint16_t) ( fuzz_u8( r ) | ( fuzz_u8( r ) << 8 ) );
    const uint8_t flip_mask = (uint8_t) ( fuzz_u8( r ) | 1 );

    // the rest is the plaintext, up to what one datagram carries

    const int header = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ( fuzz_has_nonce( type ) ? ROCKETNET_SESSION_NONCE_BYTES : 0 );
    const int max_plaintext = ROCKETNET_RECEIVE_CEILING - header - ROCKETNET_PACKET_TAG_BYTES;
    int plaintext_bytes = r->pos < r->size ? (int) ( r->size - r->pos ) : 0;
    if ( plaintext_bytes > max_plaintext )
    {
        plaintext_bytes = max_plaintext;
    }

    static uint8_t plaintext[ROCKETNET_RECEIVE_CEILING];
    fuzz_bytes( r, plaintext, (size_t) plaintext_bytes );

    static uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    const int bytes = rocketnet_write_sealed_packet( packet, type, fuzz_filter, sequence, fuzz_has_nonce( type ) ? nonce : NULL,
                                                     plaintext_bytes > 0 ? plaintext : NULL, plaintext_bytes, protocol_id, salt,
                                                     fuzz_has_ephemeral( type ) ? ephemeral : NULL, key );

    if ( sequence == 0 )
    {
        rocketnet_fuzz_require( bytes == 0, "sequence zero is refused at the seal" );
        return;
    }

    rocketnet_fuzz_require( bytes == header + plaintext_bytes + ROCKETNET_PACKET_TAG_BYTES, "the sealed size is header, plaintext and tag" );
    rocketnet_fuzz_require( rocketnet_prefilter_check( packet, bytes, fuzz_filter ) == type, "a sealed packet passes the prefilter as its type" );

    uint32_t wire_sequence = 0;

    for ( int i = 0; i < 4; i++ )
    {
        wire_sequence |= (uint32_t) packet[ROCKETNET_PREFILTER_BYTES + i] << ( 8 * i );
    }

    rocketnet_fuzz_require( wire_sequence == (uint32_t) sequence, "the wire carries the sequence's low 32 bits" );
    if ( sequence > behind )
        rocketnet_fuzz_require( rocketnet_reconstruct_sequence( wire_sequence, sequence - 1 - behind ) == sequence,
                                "reconstruction within 2^30 of the highest opened recovers the sequence" );

    static uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    uint8_t opened_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    const int opened_bytes = rocketnet_open_sealed_packet( opened, fuzz_has_nonce( type ) ? opened_nonce : NULL, (int) sizeof( opened ),
                                                           packet, bytes, type, sequence, protocol_id, salt,
                                                           fuzz_has_ephemeral( type ) ? ephemeral : NULL, key );
    rocketnet_fuzz_require( opened_bytes == plaintext_bytes, "the open returns the plaintext's length" );
    rocketnet_fuzz_require( plaintext_bytes == 0 || memcmp( opened, plaintext, (size_t) plaintext_bytes ) == 0, "the open returns the plaintext" );

    if ( fuzz_has_nonce( type ) )
    {
        rocketnet_fuzz_require( memcmp( opened_nonce, nonce, sizeof( nonce ) ) == 0, "the open returns the clear session nonce" );
    }

    // any change inside the sealed region -- the ciphertext and the tag --
    // must refuse the open

    const int sealed_start = header;
    const int position = sealed_start + flip_at % ( bytes - sealed_start );
    packet[position] ^= flip_mask;
    rocketnet_fuzz_require( rocketnet_open_sealed_packet( opened, fuzz_has_nonce( type ) ? opened_nonce : NULL, (int) sizeof( opened ),
                                                          packet, bytes, type, sequence, protocol_id, salt,
                                                          fuzz_has_ephemeral( type ) ? ephemeral : NULL, key ) < 0,
                            "a changed sealed byte refuses the open" );
}

// -----------------------------------------------------------------------
//  3 and 4: ack blocks
// -----------------------------------------------------------------------

static void fuzz_ack_read_write( struct fuzz_reader_t * r )
{
    const uint64_t highest_sent = fuzz_u64( r );
    const int bytes = r->pos < r->size ? (int) ( r->size - r->pos ) : 0;
    if ( bytes <= 0 )
        return;

    // a slice of its own, so ASan bounds the reader at the declared size

    uint8_t * slice = (uint8_t *) malloc( (size_t) bytes );
    if ( !slice )
    {
        abort();
    }

    memcpy( slice, r->data + r->pos, (size_t) bytes );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );

    const int read = rocketnet_ack_block_read( &block, slice, bytes, highest_sent );
    if ( read > 0 )
    {
        rocketnet_fuzz_require( read == ROCKETNET_ACK_BLOCK_MIN_BYTES + 4 * block.num_ranges && read <= bytes, "an accepted block's length is its geometry" );

        uint8_t written[ROCKETNET_ACK_BLOCK_MAX_BYTES];
        const int write = rocketnet_ack_block_write( written, &block );
        rocketnet_fuzz_require( write == read && memcmp( written, slice, (size_t) read ) == 0, "an accepted ack block re-encodes to its own bytes" );
    }

    free( slice );
}

static bool fuzz_replay_bit( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    const uint64_t bit = sequence % ROCKETNET_REPLAY_WINDOW_BITS;
    return ( ( replay->window[bit / 64] >> ( bit % 64 ) ) & 1 ) != 0;
}

static void fuzz_ack_build_read( struct fuzz_reader_t * r )
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );

    // a highest opened anywhere in 64 bits, but often small, so the floor at
    // sequence one is reached

    const uint8_t shape = fuzz_u8( r );
    uint64_t highest = fuzz_u64( r );
    if ( shape & 1 )
    {
        highest &= 0xFFFF;
    }

    replay.highest_opened = highest;
    fuzz_bytes( r, (uint8_t *) replay.window, sizeof( replay.window ) );

    if ( highest != 0 )
    {
        // the window always holds the highest opened sequence itself

        const uint64_t bit = highest % ROCKETNET_REPLAY_WINDOW_BITS;
        replay.window[bit / 64] |= 1ULL << ( bit % 64 );
    }

    const int capacity = ROCKETNET_ACK_BLOCK_MIN_BYTES + ( fuzz_u8( r ) | ( fuzz_u8( r ) << 8 ) ) % ( 4 * ROCKETNET_ACK_BLOCK_MAX_RANGES + 1 );

    uint8_t built[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int bytes = rocketnet_ack_block_build( built, capacity, &replay );
    rocketnet_fuzz_require( bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES && bytes <= capacity, "a built block fits its capacity" );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    rocketnet_fuzz_require( rocketnet_ack_block_read( &block, built, bytes, highest ) == bytes, "a built block reads back whole" );
    rocketnet_fuzz_require( block.largest == highest, "a built block names the highest opened" );
    if ( highest == 0 )
        return;

    // every sequence the block names received is in the window, at or above
    // the window's floor; the block never speaks below it

    const uint64_t floor_sequence = highest >= ROCKETNET_REPLAY_WINDOW_BITS ? highest - ROCKETNET_REPLAY_WINDOW_BITS + 1 : 1;
    uint64_t s = highest;
    for ( uint32_t i = 0; i < block.first_range_length; i++, s-- )
    {
        rocketnet_fuzz_require( s >= floor_sequence && fuzz_replay_bit( &replay, s ), "a first-range sequence is held in the window" );
    }

    for ( int k = 0; k < block.num_ranges; k++ )
    {
        s -= block.range_gap[k];
        for ( uint32_t i = 0; i < block.range_length[k]; i++, s-- )
        {
            rocketnet_fuzz_require( s >= floor_sequence && fuzz_replay_bit( &replay, s ), "a ranged sequence is held in the window" );
        }
    }
}

// -----------------------------------------------------------------------
//  5: the fragment header
// -----------------------------------------------------------------------

static void fuzz_fragment_header( struct fuzz_reader_t * r )
{
    uint32_t payload_id = (uint32_t) fuzz_u64( r );
    if ( payload_id == 0 )
    {
        payload_id = 1;
    }

    const int count = 2 + fuzz_u8( r ) % ( ROCKETNET_MAX_FRAGMENTS - 1 );
    const int index = fuzz_u8( r ) % count;

    uint8_t header[ROCKETNET_FRAGMENT_HEADER_BYTES];
    rocketnet_write_fragment_header( header, payload_id, index, count );

    uint32_t read_id = 0;
    int read_index = -1;
    int read_count = -1;
    rocketnet_read_fragment_header( header, &read_id, &read_index, &read_count );
    rocketnet_fuzz_require( read_id == payload_id && read_index == index && read_count == count, "a fragment header round-trips" );
}

// -----------------------------------------------------------------------
//  6: the connect token's two layouts
// -----------------------------------------------------------------------

static void fuzz_token_layouts( struct fuzz_reader_t * r )
{
    struct rocketnet_signed_connect_data_t s;
    memset( &s, 0, sizeof( s ) );
    s.version = (uint32_t) fuzz_u64( r );
    s.protocol_id = fuzz_u64( r );
    s.create_timestamp = fuzz_u64( r );
    s.expire_timestamp = fuzz_u64( r );
    s.timeout_seconds = (int32_t) (uint32_t) fuzz_u64( r );
    s.address_flags = fuzz_u8( r );
    fuzz_bytes( r, s.ipv4_address, sizeof( s.ipv4_address ) );
    fuzz_bytes( r, s.ipv6_address, sizeof( s.ipv6_address ) );
    fuzz_bytes( r, s.server_public_key, sizeof( s.server_public_key ) );
    fuzz_bytes( r, s.sealed_private_block, sizeof( s.sealed_private_block ) );

    static uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    rocketnet_write_signed_connect_data( &s, signed_data );

    struct rocketnet_signed_connect_data_t t;
    memset( &t, 0xA5, sizeof( t ) );
    rocketnet_read_signed_connect_data( &t, signed_data );
    rocketnet_fuzz_require( t.version == s.version && t.protocol_id == s.protocol_id && t.create_timestamp == s.create_timestamp &&
                                t.expire_timestamp == s.expire_timestamp && t.timeout_seconds == s.timeout_seconds && t.address_flags == s.address_flags &&
                                memcmp( t.ipv4_address, s.ipv4_address, sizeof( s.ipv4_address ) ) == 0 &&
                                memcmp( t.ipv6_address, s.ipv6_address, sizeof( s.ipv6_address ) ) == 0 &&
                                memcmp( t.server_public_key, s.server_public_key, sizeof( s.server_public_key ) ) == 0 &&
                                memcmp( t.sealed_private_block, s.sealed_private_block, sizeof( s.sealed_private_block ) ) == 0,
                            "the signed connect data round-trips every field" );

    struct rocketnet_connect_token_private_t p;
    memset( &p, 0, sizeof( p ) );
    p.client_id = fuzz_u64( r );
    p.protocol_id = fuzz_u64( r );
    p.expire_timestamp = fuzz_u64( r );
    fuzz_bytes( r, p.client_to_server_key, sizeof( p.client_to_server_key ) );
    fuzz_bytes( r, p.server_to_client_key, sizeof( p.server_to_client_key ) );
    fuzz_bytes( r, p.user_data, sizeof( p.user_data ) );

    uint8_t block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &p, block );

    struct rocketnet_connect_token_private_t q;
    memset( &q, 0xA5, sizeof( q ) );
    rocketnet_read_connect_token_private( &q, block );
    rocketnet_fuzz_require( q.client_id == p.client_id && q.protocol_id == p.protocol_id && q.expire_timestamp == p.expire_timestamp &&
                                memcmp( q.client_to_server_key, p.client_to_server_key, sizeof( p.client_to_server_key ) ) == 0 &&
                                memcmp( q.server_to_client_key, p.server_to_client_key, sizeof( p.server_to_client_key ) ) == 0 &&
                                memcmp( q.user_data, p.user_data, sizeof( p.user_data ) ) == 0,
                            "the private block round-trips every field" );
    sodium_memzero( &p, sizeof( p ) );
    sodium_memzero( &q, sizeof( q ) );
    sodium_memzero( block, sizeof( block ) );
}

// -----------------------------------------------------------------------
//  7: addresses
// -----------------------------------------------------------------------

static void fuzz_address( struct fuzz_reader_t * r )
{
    const uint8_t mode = fuzz_u8( r );
    char text[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    char again[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];

    if ( ( mode & 1 ) == 0 )
    {
        // text first: whatever parses formats to text that parses back to it

        size_t n = 0;
        while ( n + 1 < sizeof( text ) && r->pos < r->size )
        {
            const uint8_t c = fuzz_u8( r );
            if ( c == 0 )
                break;
            text[n++] = (char) c;
        }

        text[n] = '\0';

        struct rocketnet_address_t a;
        if ( !rocketnet_address_parse( &a, text ) )
            return;
        rocketnet_address_to_string( &a, again );

        struct rocketnet_address_t b;
        rocketnet_fuzz_require( rocketnet_address_parse( &b, again ), "a formatted address parses" );
        rocketnet_fuzz_require( rocketnet_address_equal( &a, &b ), "parse(to_string(parse(text))) is parse(text)" );
        return;
    }

    // value first: an address from bytes formats and parses back to itself

    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ( mode & 2 ) ? ROCKETNET_ADDRESS_IPV6 : ROCKETNET_ADDRESS_IPV4;
    if ( a.type == ROCKETNET_ADDRESS_IPV4 )
    {
        fuzz_bytes( r, a.data.ipv4, sizeof( a.data.ipv4 ) );
    }
    else
        for ( int i = 0; i < 8; i++ )
        {
            a.data.ipv6[i] = (uint16_t) ( fuzz_u8( r ) | ( fuzz_u8( r ) << 8 ) );
        }

    a.port = (uint16_t) ( fuzz_u8( r ) | ( fuzz_u8( r ) << 8 ) );
    rocketnet_address_to_string( &a, text );

    struct rocketnet_address_t b;
    rocketnet_fuzz_require( rocketnet_address_parse( &b, text ), "a formatted address parses" );
    rocketnet_fuzz_require( rocketnet_address_equal( &a, &b ), "parse(to_string(address)) is the address" );
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir> writes one seed per
//  form, each built from real values by the real write paths: an ack block
//  the builder made, addresses the formatter printed, a sealed packet's
//  fields at sequence one. The replay then continues as usual.
// -----------------------------------------------------------------------

static void fuzz_write_seed( const char * dir, const char * name, uint8_t selector, const uint8_t * body, size_t bytes )
{
    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
        abort(); // a truncated path would write some other file

    FILE * f = rocketnet_test_fopen( path, "wb" );
    if ( !f )
    {
        abort();
    }

    fwrite( &selector, 1, 1, f );

    if ( bytes )
    {
        fwrite( body, 1, bytes, f );
    }

    fclose( f );
}

static void fuzz_write_seeds( const char * dir )
{
    static uint8_t body[2048];
    memset( body, 0, sizeof( body ) );

    // 0, 1, 5, 6: the fields at their defaults

    body[0] = 1;
    fuzz_write_seed( dir, "request_with_cookie", 0, body, 1 + ROCKETNET_COOKIE_FIELD_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_KEY_BYTES + ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES );
    fuzz_write_seed( dir, "cookie_fields", 1, body, ROCKETNET_CONNECTION_SALT_BYTES + 8 + ROCKETNET_COOKIE_MAC_BYTES );
    fuzz_write_seed( dir, "fragment_header_fields", 5, body, 10 );
    fuzz_write_seed( dir, "token_layout_fields", 6, body, 900 );

    // 2: every sealed type at sequence one with a 16-byte plaintext

    for ( int t = 0; t < (int) sizeof( fuzz_sealed_types ); t++ )
    {
        memset( body, 0, sizeof( body ) );
        body[0] = (uint8_t) t;

        const int sequence_at = 1 + ROCKETNET_KEY_BYTES + ROCKETNET_CONNECTION_SALT_BYTES + ROCKETNET_SESSION_NONCE_BYTES + ROCKETNET_KEY_BYTES + 8;
        body[sequence_at] = 1;

        const int plaintext_at = sequence_at + 8 + 8 + 3;

        for ( int i = 0; i < 16; i++ )
        {
            body[plaintext_at + i] = (uint8_t) i;
        }

        char name[64];
        snprintf( name, sizeof( name ), "sealed_type_%d", (int) fuzz_sealed_types[t] );
        fuzz_write_seed( dir, name, 2, body, (size_t) ( plaintext_at + 16 ) );
    }

    // 3: a block the builder made from a window with holes, read back at
    // its own largest
    {
        struct rocketnet_replay_protection_t replay;
        memset( &replay, 0, sizeof( replay ) );
        replay.highest_opened = 1000;
        for ( uint64_t q = 900; q <= 1000; q++ )
            if ( q % 7 != 0 )
            {
                replay.window[( q % ROCKETNET_REPLAY_WINDOW_BITS ) / 64] |= 1ULL << ( q % 64 );
            }

        uint8_t built[ROCKETNET_ACK_BLOCK_MAX_BYTES];
        const int bytes = rocketnet_ack_block_build( built, (int) sizeof( built ), &replay );

        memset( body, 0, sizeof( body ) );

        uint64_t highest = replay.highest_opened;

        for ( int i = 0; i < 8; i++ )
        {
            body[i] = (uint8_t) ( highest >> ( 8 * i ) );
        }

        memcpy( body + 8, built, (size_t) bytes );
        fuzz_write_seed( dir, "ack_block_built_with_holes", 3, body, (size_t) ( 8 + bytes ) );
    }

    // 4: a window with holes, small and large highest

    memset( body, 0, sizeof( body ) );
    body[0] = 1;
    body[1] = 0xE8;
    body[2] = 0x03;

    for ( int i = 0; i < 64; i++ )
    {
        body[9 + i] = (uint8_t) ( 0xB6 ^ i );
    }

    body[9 + sizeof( ( (struct rocketnet_replay_protection_t *) 0 )->window )] = 0xFF;
    fuzz_write_seed( dir, "ack_window_small_highest", 4, body, 9 + sizeof( ( (struct rocketnet_replay_protection_t *) 0 )->window ) + 2 );

    // 7: addresses as the formatter prints them

    const char * addresses[] = { "127.0.0.1:40000", "203.0.113.10", "[::1]:40000", "2001:db8::1" };

    for ( int i = 0; i < 4; i++ )
    {
        struct rocketnet_address_t a;
        if ( !rocketnet_address_parse( &a, addresses[i] ) )
        {
            abort();
        }

        char text[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
        rocketnet_address_to_string( &a, text );
        memset( body, 0, sizeof( body ) );
        body[0] = 0;

        const size_t n = strlen( text );
        memcpy( body + 1, text, n );

        char name[64];
        snprintf( name, sizeof( name ), "address_text_%d", i );
        fuzz_write_seed( dir, name, 7, body, 1 + n );
    }
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    if ( sodium_init() < 0 )
    {
        abort();
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        fuzz_write_seeds( seed_dir );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 1 )
        return 0;

    struct fuzz_reader_t r = { data, size, 1 };

    switch ( data[0] % 8 )
    {
        case 0: fuzz_request( &r ); break;
        case 1: fuzz_cookie( &r ); break;
        case 2: fuzz_sealed( &r ); break;
        case 3: fuzz_ack_read_write( &r ); break;
        case 4: fuzz_ack_build_read( &r ); break;
        case 5: fuzz_fragment_header( &r ); break;
        case 6: fuzz_token_layouts( &r ); break;
        case 7: fuzz_address( &r ); break;
    }

    return 0;
}
