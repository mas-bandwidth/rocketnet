/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_connect_token.h"
#include "rocketnet.h"
#include "rocketnet_address.h"

#include <sodium.h>
#include <string.h>

void rocketnet_write_prefilter_header( uint8_t ** p, uint8_t packet_type, const uint8_t * title_tag )
{
    rocketnet_write_uint8( p, packet_type );
    rocketnet_write_bytes( p, title_tag, ROCKETNET_TITLE_TAG_BYTES );
}

int rocketnet_prefilter_check( const uint8_t * packet_data, int packet_bytes, const uint8_t * title_tag )
{
    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
        return -1;

    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
        return -1;

    if ( packet_data[1] != title_tag[0] || packet_data[2] != title_tag[1] || packet_data[3] != title_tag[2] )
        return -1;

    return packet_data[0];
}

static int rocketnet_packet_type_has_session_nonce( uint8_t packet_type )
{
    return packet_type == ROCKETNET_CONNECTION_DENIED_PACKET || packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET;
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
    rocketnet_write_uint32( &p, ROCKETNET_VERSION );
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

int rocketnet_write_sealed_packet( uint8_t * packet_data,
                                   uint8_t packet_type,
                                   const uint8_t * title_tag,
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
    rocketnet_assert( title_tag );
    rocketnet_assert( connection_salt );
    rocketnet_assert( key );

    if ( packet_type < ROCKETNET_CONNECTION_DENIED_PACKET || packet_type > ROCKETNET_CONNECTION_DISCONNECT_PACKET )
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
    rocketnet_write_prefilter_header( &p, packet_type, title_tag );
    rocketnet_write_uint32( &p, (uint32_t) ( sequence & 0xFFFFFFFF ) );

    if ( session_nonce )
        rocketnet_write_bytes( &p, session_nonce, ROCKETNET_SESSION_NONCE_BYTES );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, protocol_id, packet_type, connection_salt, client_ephemeral_public_key );

    uint8_t nonce[12];
    rocketnet_sequence_nonce( nonce, sequence );

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

    if ( packet_type < ROCKETNET_CONNECTION_DENIED_PACKET || packet_type > ROCKETNET_CONNECTION_DISCONNECT_PACKET )
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

    if ( out_session_nonce )
        memcpy( out_session_nonce, packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES, ROCKETNET_SESSION_NONCE_BYTES );

    uint8_t associated_data[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    int associated_data_bytes = rocketnet_write_packet_associated_data( associated_data, protocol_id, packet_type, connection_salt, client_ephemeral_public_key );

    uint8_t nonce[12];
    rocketnet_sequence_nonce( nonce, sequence );

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
                                                const uint8_t * title_tag,
                                                const uint8_t * return_cookie,
                                                const uint8_t * connection_salt,
                                                const uint8_t * client_ephemeral_public_key,
                                                const uint8_t * signed_connect_data,
                                                const uint8_t * signature )
{
    rocketnet_assert( packet_data );
    rocketnet_assert( title_tag );
    rocketnet_assert( connection_salt );
    rocketnet_assert( client_ephemeral_public_key );
    rocketnet_assert( signed_connect_data );
    rocketnet_assert( signature );

    uint8_t * p = packet_data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_REQUEST_PACKET, title_tag );
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
                                               const uint8_t * title_tag,
                                               const uint8_t * connection_salt,
                                               uint64_t cookie_expire_timestamp,
                                               const uint8_t * cookie_mac )
{
    rocketnet_assert( packet_data );
    rocketnet_assert( title_tag );
    rocketnet_assert( connection_salt );
    rocketnet_assert( cookie_mac );

    uint8_t * p = packet_data;
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_COOKIE_PACKET, title_tag );
    rocketnet_write_bytes( &p, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
    rocketnet_write_uint64( &p, cookie_expire_timestamp );
    rocketnet_write_bytes( &p, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES );
    rocketnet_assert( p - packet_data == ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES );
}

uint64_t rocketnet_reconstruct_sequence( uint32_t wire_sequence, uint64_t highest_opened )
{
    // candidates share the wire value in their low 32 bits. take the one
    // nearest to expected = highest opened + 1; on an exact tie, the larger.

    uint64_t expected = highest_opened + 1;

    uint64_t base = expected & 0xFFFFFFFF00000000ULL;

    uint64_t candidate = base | wire_sequence;

    uint64_t best = candidate;
    uint64_t best_distance = candidate > expected ? candidate - expected : expected - candidate;

    if ( base >= 0x100000000ULL )
    {
        uint64_t below = ( base - 0x100000000ULL ) | wire_sequence;
        uint64_t distance = below > expected ? below - expected : expected - below;
        if ( distance < best_distance || ( distance == best_distance && below > best ) )
        {
            best = below;
            best_distance = distance;
        }
    }

    {
        uint64_t above = ( base + 0x100000000ULL ) | wire_sequence;
        uint64_t distance = above > expected ? above - expected : expected - above;
        if ( distance < best_distance || ( distance == best_distance && above > best ) )
        {
            best = above;
            best_distance = distance;
        }
    }

    return best;
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

    crypto_generichash( mac, ROCKETNET_COOKIE_MAC_BYTES, input, (size_t) input_bytes, cookie_key, ROCKETNET_COOKIE_KEY_BYTES );

    return 1;
}

