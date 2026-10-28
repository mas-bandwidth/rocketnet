/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_read_write.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_fragment.h"
#include "rocketnet_ack.h"

#include <sodium.h>
#include <string.h>

#if ROCKETNET_DEV
thread_local struct rocketnet_crypto_tally_t rocketnet_crypto_tally;
#endif // #if ROCKETNET_DEV

// The packet header already carries the filter field a keyed check reads:
// the prefilter header (ROCKETNET_PREFILTER_BYTES in sdk/rocketnet_wire.h,
// written by rocketnet_write_prefilter_header) holds the packet type, a
// filter epoch byte and a ROCKETNET_FILTER_VALUE_BYTES filter value. v1.0 has
// no keyed filter-epoch check and writes ROCKETNET_FILTER_EPOCH_UNKEYED; the
// field stays exactly as it is, so a keyed filter can return later as a value
// change, never a format change.

void rocketnet_write_prefilter_header( uint8_t ** p, uint8_t packet_type, const uint8_t * packet_filter )
{
    rocketnet_write_uint8( p, packet_type );
    rocketnet_write_uint8( p, ROCKETNET_FILTER_EPOCH_UNKEYED );
    rocketnet_write_bytes( p, packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
}

// maps a refusal code to its counter, so both receive paths attribute the
// reason identically and neither invents its own mapping

int rocketnet_prefilter_counter( int refusal_code )
{
    switch ( refusal_code )
    {
        case ROCKETNET_PREFILTER_TOO_SHORT:   return ROCKETNET_COUNTER_PREFILTER_TOO_SHORT;
        case ROCKETNET_PREFILTER_TOO_LARGE:   return ROCKETNET_COUNTER_PREFILTER_TOO_LARGE;
        case ROCKETNET_PREFILTER_WRONG_EPOCH: return ROCKETNET_COUNTER_PREFILTER_WRONG_EPOCH;
        case ROCKETNET_PREFILTER_WRONG_VALUE: return ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE;
        default:                              break;
    }

    // every refusal code has a case above. a code arriving here is a caller
    // passing something that is not one -- loud in development, and landing on
    // a named counter rather than silently sharing a slot with a real reason,
    // which is the failure the observability rules name directly.

    rocketnet_assert( 0 );

    return ROCKETNET_COUNTER_PREFILTER_WRONG_VALUE;
}

int rocketnet_prefilter_check( const uint8_t * packet_data, int packet_bytes, const uint8_t * packet_filter )
{
    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
        return ROCKETNET_PREFILTER_TOO_SHORT;

    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return ROCKETNET_PREFILTER_TOO_LARGE;

    // the epoch decides what produced the filter value. 1.0 is unkeyed only:
    // any other generation names a magic this build does not hold, and is
    // refused on a comparison before the value is even examined.

    if ( packet_data[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
        return ROCKETNET_PREFILTER_WRONG_EPOCH;

    if ( packet_data[2] != packet_filter[0] || packet_data[3] != packet_filter[1] ||
         packet_data[4] != packet_filter[2] || packet_data[5] != packet_filter[3] )
        return ROCKETNET_PREFILTER_WRONG_VALUE;

    return packet_data[0];
}

static int rocketnet_packet_type_has_session_nonce( uint8_t packet_type )
{
    return packet_type == ROCKETNET_CONNECTION_DENIED_PACKET || packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET;
}

// the sealed types: 1 through 6, 8 and 9. type 0 (request) and type 7
// (cookie) are the two unsealed packets.

static int rocketnet_packet_type_is_sealed( uint8_t packet_type )
{
    return ( packet_type >= ROCKETNET_CONNECTION_DENIED_PACKET && packet_type <= ROCKETNET_CONNECTION_DISCONNECT_PACKET ) ||
           packet_type == ROCKETNET_CONNECTION_ACK_PACKET ||
           packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET;
}

static int rocketnet_packet_type_has_ephemeral_in_ad( uint8_t packet_type )
{
    return packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET || packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET;
}

int rocketnet_write_packet_associated_data( uint8_t * associated_data,
                                            uint64_t protocol_id,
                                            uint8_t packet_type,
                                            const uint8_t * connection_salt,
                                            const uint8_t * client_ephemeral_public_key )
{
    rocketnet_assert( associated_data );
    rocketnet_assert( connection_salt );

    uint8_t * p = associated_data;
    rocketnet_write_uint32( &p, ROCKETNET_WIRE_VERSION );
    rocketnet_write_uint64( &p, protocol_id );
    rocketnet_write_uint8( &p, packet_type );
    rocketnet_write_bytes( &p, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );

    if ( !client_ephemeral_public_key )
        return ROCKETNET_AD_BYTES;

    rocketnet_write_bytes( &p, client_ephemeral_public_key, ROCKETNET_KEY_BYTES );

    return ROCKETNET_AD_WITH_EPHEMERAL_BYTES;
}

static void rocketnet_sequence_nonce( uint8_t * nonce, uint64_t sequence )
{
    memset( nonce, 0, 4 );

    uint8_t * p = nonce + 4;
    rocketnet_write_uint64( &p, sequence );
}

// The in-place twin of rocketnet_write_sealed_packet for the payload type
// only: the payload is ALREADY at wire_start + 10 + the 7-byte minimum ack
// block, written there by the game through the acquire/commit surface, and
// with the minimum block -- everything received, the common case on a
// clean link -- it is sealed where it lies: the header and block land in
// front of it and the tag behind it, no staging copy. A block carrying
// loss ranges moves the payload up by the extra bytes first, bounded by
// the send budget. libsodium's chacha20poly1305_ietf encrypt is documented
// safe in place.

int rocketnet_write_sealed_payload_in_place( uint8_t * wire_start,
                                             const uint8_t * packet_filter,
                                             uint64_t sequence,
                                             const uint8_t * ack_block,
                                             int ack_block_bytes,
                                             int payload_bytes,
                                             uint64_t protocol_id,
                                             const uint8_t * connection_salt,
                                             const uint8_t * key )
{
    rocketnet_assert( wire_start );
    rocketnet_assert( packet_filter );
    rocketnet_assert( ack_block );
    rocketnet_assert( connection_salt );
    rocketnet_assert( key );

    if ( sequence == 0 )
        return 0;

    if ( ack_block_bytes < ROCKETNET_ACK_BLOCK_MIN_BYTES || ack_block_bytes > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        return 0;

    if ( payload_bytes < 1 ||
         payload_bytes > ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ack_block_bytes )
        return 0;

    uint8_t * p = wire_start;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, packet_filter );
    rocketnet_write_uint32( &p, (uint32_t) ( sequence & 0xFFFFFFFF ) );

    uint8_t * payload = wire_start + ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;

    if ( ack_block_bytes > ROCKETNET_ACK_BLOCK_MIN_BYTES )
    {
        memmove( payload + ( ack_block_bytes - ROCKETNET_ACK_BLOCK_MIN_BYTES ), payload, (size_t) payload_bytes );
    }

    memcpy( p, ack_block, (size_t) ack_block_bytes );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, protocol_id, ROCKETNET_CONNECTION_PAYLOAD_PACKET, connection_salt, NULL );

    uint8_t nonce[12];
    rocketnet_sequence_nonce( nonce, sequence );

    ROCKETNET_CRYPTO_TALLY( aead_seal );
    unsigned long long sealed_bytes = 0;
    if ( crypto_aead_chacha20poly1305_ietf_encrypt( p, &sealed_bytes, p,
                                                    (unsigned long long) ( ack_block_bytes + payload_bytes ),
                                                    associated_data, (unsigned long long) associated_data_bytes,
                                                    NULL, nonce, key ) != 0 )
        return 0;

    return (int) ( p - wire_start ) + (int) sealed_bytes;
}

int rocketnet_write_sealed_packet( uint8_t * packet_data,
                                   uint8_t packet_type,
                                   const uint8_t * packet_filter,
                                   uint64_t sequence,
                                   const uint8_t * session_nonce,
                                   const uint8_t * data,
                                   int data_bytes,
                                   uint64_t protocol_id,
                                   const uint8_t * connection_salt,
                                   const uint8_t * client_ephemeral_public_key,
                                   const uint8_t * key )
{
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_filter );
    rocketnet_assert( connection_salt );
    rocketnet_assert( key );

    if ( !rocketnet_packet_type_is_sealed( packet_type ) )
        return 0;

    if ( sequence == 0 )
        return 0;

    if ( rocketnet_packet_type_has_session_nonce( packet_type ) != ( session_nonce != NULL ) )
        return 0;

    if ( rocketnet_packet_type_has_ephemeral_in_ad( packet_type ) != ( client_ephemeral_public_key != NULL ) )
        return 0;

    if ( data_bytes < 0 || ( data_bytes == 0 && data != NULL ) || ( data_bytes > 0 && data == NULL ) )
        return 0;

    uint8_t * p = packet_data;
    rocketnet_write_prefilter_header( &p, packet_type, packet_filter );
    rocketnet_write_uint32( &p, (uint32_t) ( sequence & 0xFFFFFFFF ) );

    if ( session_nonce )
        rocketnet_write_bytes( &p, session_nonce, ROCKETNET_SESSION_NONCE_BYTES );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, protocol_id, packet_type, connection_salt, client_ephemeral_public_key );

    uint8_t nonce[12];
    rocketnet_sequence_nonce( nonce, sequence );

    ROCKETNET_CRYPTO_TALLY( aead_seal );
    unsigned long long sealed_bytes = 0;
    if ( crypto_aead_chacha20poly1305_ietf_encrypt( p, &sealed_bytes, data, (unsigned long long) data_bytes,
                                                    associated_data, (unsigned long long) associated_data_bytes,
                                                    NULL, nonce, key ) != 0 )
        return 0;

    p += sealed_bytes;

    return (int) ( p - packet_data );
}

int rocketnet_open_sealed_packet( uint8_t * out_data,
                                  uint8_t * out_session_nonce,
                                  int out_capacity,
                                  const uint8_t * packet_data,
                                  int packet_bytes,
                                  uint8_t packet_type,
                                  uint64_t sequence,
                                  uint64_t protocol_id,
                                  const uint8_t * connection_salt,
                                  const uint8_t * client_ephemeral_public_key,
                                  const uint8_t * key )
{
    rocketnet_assert( out_data );
    rocketnet_assert( packet_data );
    rocketnet_assert( connection_salt );
    rocketnet_assert( key );

    if ( !rocketnet_packet_type_is_sealed( packet_type ) )
        return -1;

    if ( sequence == 0 )
        return -1;

    if ( rocketnet_packet_type_has_session_nonce( packet_type ) != ( out_session_nonce != NULL ) )
        return -1;

    if ( rocketnet_packet_type_has_ephemeral_in_ad( packet_type ) != ( client_ephemeral_public_key != NULL ) )
        return -1;

    int header_bytes = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES;
    if ( out_session_nonce )
        header_bytes += ROCKETNET_SESSION_NONCE_BYTES;

    if ( packet_bytes < header_bytes + ROCKETNET_PACKET_TAG_BYTES )
        return -1;

    // Refuse if the output buffer is too small for the plaintext

    int plaintext_bytes = packet_bytes - header_bytes - ROCKETNET_PACKET_TAG_BYTES;
    if ( out_capacity < plaintext_bytes )
        return -1;

    if ( out_session_nonce )
        memcpy( out_session_nonce, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, protocol_id, packet_type, connection_salt, client_ephemeral_public_key );

    uint8_t nonce[12];
    rocketnet_sequence_nonce( nonce, sequence );

    ROCKETNET_CRYPTO_TALLY( aead_open );
    unsigned long long opened_bytes = 0;
    if ( crypto_aead_chacha20poly1305_ietf_decrypt( out_data, &opened_bytes, NULL,
                                                    packet_data + header_bytes,
                                                    (unsigned long long) ( packet_bytes - header_bytes ),
                                                    associated_data, (unsigned long long) associated_data_bytes,
                                                    nonce, key ) != 0 )
        return -1;

    return (int) opened_bytes;
}

void rocketnet_write_connection_request_packet( uint8_t * packet_data,
                                                const uint8_t * packet_filter,
                                                const uint8_t * return_cookie,
                                                const uint8_t * connection_salt,
                                                const uint8_t * client_ephemeral_public_key,
                                                const uint8_t * signed_connect_data,
                                                const uint8_t * signature )
{
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_filter );
    rocketnet_assert( connection_salt );
    rocketnet_assert( client_ephemeral_public_key );
    rocketnet_assert( signed_connect_data );
    rocketnet_assert( signature );

    uint8_t * p = packet_data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, packet_filter );
    if ( return_cookie )
        rocketnet_write_bytes( &p, return_cookie, ROCKETNET_COOKIE_FIELD_BYTES );
    else
        rocketnet_write_zero_bytes( &p, ROCKETNET_COOKIE_FIELD_BYTES );
    rocketnet_write_bytes( &p, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    rocketnet_write_bytes( &p, client_ephemeral_public_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    rocketnet_write_bytes( &p, signature, ROCKETNET_SIGNATURE_BYTES );
    rocketnet_assert( p - packet_data == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
}

void rocketnet_write_connection_cookie_packet( uint8_t * packet_data,
                                               const uint8_t * packet_filter,
                                               const uint8_t * connection_salt,
                                               uint64_t cookie_expire_timestamp,
                                               const uint8_t * cookie_mac )
{
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_filter );
    rocketnet_assert( connection_salt );
    rocketnet_assert( cookie_mac );

    uint8_t * p = packet_data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_COOKIE_PACKET, packet_filter );
    rocketnet_write_bytes( &p, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    rocketnet_write_uint64( &p, cookie_expire_timestamp );
    rocketnet_write_bytes( &p, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
    rocketnet_assert( p - packet_data == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
}

uint64_t rocketnet_reconstruct_near( uint32_t wire_value, uint64_t expected )
{
    // candidates share the wire value in their low 32 bits. take the one
    // nearest to expected; on an exact tie, the larger.

    uint64_t base = expected & 0xFFFFFFFF00000000ULL;

    uint64_t candidate = base | wire_value;

    uint64_t best = candidate;
    uint64_t best_distance = candidate > expected ? candidate - expected : expected - candidate;

    if ( base >= 0x100000000ULL )
    {
        uint64_t below = ( base - 0x100000000ULL ) | wire_value;
        uint64_t distance = below > expected ? below - expected : expected - below;
        if ( distance < best_distance || ( distance == best_distance && below > best ) )
        {
            best = below;
            best_distance = distance;
        }
    }

    {
        uint64_t above = ( base + 0x100000000ULL ) | wire_value;
        uint64_t distance = above > expected ? above - expected : expected - above;
        if ( distance < best_distance || ( distance == best_distance && above > best ) )
            best = above;
    }

    return best;
}

int rocketnet_session_min_wire_bytes( uint8_t packet_type )
{
    switch ( packet_type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET: return ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:    return ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET: return ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_ACK_PACKET:        return ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:   return ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES;
        default:                                     return 0;
    }
}

bool rocketnet_session_type_data_fits( uint8_t packet_type, int type_data_bytes )
{
    switch ( packet_type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET: return type_data_bytes == 8;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:    return type_data_bytes >= 1 && type_data_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET: return type_data_bytes == 0;
        case ROCKETNET_CONNECTION_ACK_PACKET:        return type_data_bytes == 0;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:   return type_data_bytes >= ROCKETNET_FRAGMENT_HEADER_BYTES + 1;
        default:                                     return false;
    }
}

bool rocketnet_session_ack_eligible_on_open( uint8_t packet_type )
{
    return packet_type != ROCKETNET_CONNECTION_PAYLOAD_PACKET && packet_type != ROCKETNET_CONNECTION_FRAGMENT_PACKET;
}

bool rocketnet_session_elicits_ack( uint8_t packet_type )
{
    return packet_type != ROCKETNET_CONNECTION_ACK_PACKET && rocketnet_session_ack_eligible_on_open( packet_type );
}

int rocketnet_session_ack_block_build( uint8_t * output, int other_bytes, const struct rocketnet_replay_protection_t * accepted )
{
    int capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - other_bytes;
    if ( capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    return rocketnet_ack_block_build( output, capacity, accepted );
}

uint64_t rocketnet_reconstruct_sequence( uint32_t wire_sequence, uint64_t highest_opened )
{
    return rocketnet_reconstruct_near( wire_sequence, highest_opened + 1 );
}

int rocketnet_write_address_bytes( uint8_t * output, const struct rocketnet_address_t * address )
{
    rocketnet_assert( output );
    rocketnet_assert( address );

    uint8_t * p = output;

    if ( address->type == ROCKETNET_ADDRESS_IPV4 )
    {
        rocketnet_write_uint8( &p, 1 );
        rocketnet_write_bytes( &p, address->data.ipv4, 4 );
    }
    else if ( address->type == ROCKETNET_ADDRESS_IPV6 )
    {
        rocketnet_write_uint8( &p, 2 );
        for ( int i = 0; i < 8; i++ )
            rocketnet_write_uint16( &p, address->data.ipv6[i] );
    }
    else
    {
        return 0;
    }

    rocketnet_write_uint16( &p, address->port );

    return (int) ( p - output );
}

int rocketnet_write_cookie_mac_input( uint8_t * input,
                                      const struct rocketnet_address_t * address,
                                      const uint8_t * connection_salt,
                                      uint64_t cookie_expire_timestamp )
{
    rocketnet_assert( input );
    rocketnet_assert( connection_salt );

    int address_bytes = rocketnet_write_address_bytes( input, address );
    if ( address_bytes == 0 )
        return 0;

    uint8_t * p = input + address_bytes;
    rocketnet_write_bytes( &p, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    rocketnet_write_uint64( &p, cookie_expire_timestamp );

    return (int) ( p - input );
}

int rocketnet_generate_cookie_mac( uint8_t * mac,
                                   const struct rocketnet_address_t * address,
                                   const uint8_t * connection_salt,
                                   uint64_t cookie_expire_timestamp,
                                   const uint8_t * cookie_key )
{
    rocketnet_assert( mac );
    rocketnet_assert( cookie_key );

    uint8_t input[ROCKETNET_COOKIE_MAC_INPUT_IPV6_BYTES];
    int input_bytes = rocketnet_write_cookie_mac_input( input, address, connection_salt, cookie_expire_timestamp );
    if ( input_bytes == 0 )
        return 0;

    crypto_generichash( mac, ROCKETNET_COOKIE_MAC_BYTES, input, (unsigned long long) input_bytes, cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

    return 1;
}
