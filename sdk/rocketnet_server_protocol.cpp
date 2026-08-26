/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_keys.h"
#include "rocketnet.h"

#include <sodium.h>
#include <string.h>

static int rocketnet_slot_view_address_connected( const struct rocketnet_server_slot_view_t * slots, const struct rocketnet_address_t * address )
{
    return slots->address_connected ? slots->address_connected( slots->context, address ) : 0;
}

static int rocketnet_slot_view_client_id_connected( const struct rocketnet_server_slot_view_t * slots, uint64_t client_id )
{
    return slots->client_id_connected ? slots->client_id_connected( slots->context, client_id ) : 0;
}

static int rocketnet_slot_view_num_free_slots( const struct rocketnet_server_slot_view_t * slots )
{
    return slots->num_free_slots ? slots->num_free_slots( slots->context ) : 1;
}

// the address fields of the signed connect data, encoded as the token
// encodes them, for comparison against this server's advertised addresses

static void rocketnet_encode_token_address( uint8_t * output, const struct rocketnet_address_t * address, int ipv6 )
{
    uint8_t * p = output;
    if ( !ipv6 )
    {
        rocketnet_write_bytes( &p, address->data.ipv4, 4 );
        rocketnet_write_uint16( &p, address->port );
    }
    else
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_write_uint16( &p, address->data.ipv6[i] );
        rocketnet_write_uint16( &p, address->port );
    }
}

static int rocketnet_server_process_opened_request( struct rocketnet_server_protocol_t * server,
                                                    const struct rocketnet_address_t * from,
                                                    const uint8_t * connection_salt,
                                                    const uint8_t * client_ephemeral_public_key,
                                                    const uint8_t * signed_connect_data,
                                                    const struct rocketnet_connect_token_private_t * private_data,
                                                    const uint8_t * token_identifier,
                                                    uint64_t token_expire_timestamp,
                                                    int32_t timeout_seconds,
                                                    uint64_t current_timestamp,
                                                    double current_time,
                                                    uint8_t * response_packet,
                                                    int * response_bytes );

int rocketnet_server_process_connection_request( struct rocketnet_server_protocol_t * server,
                                                 const struct rocketnet_address_t * from,
                                                 const uint8_t * packet_data,
                                                 int packet_bytes,
                                                 uint64_t current_timestamp,
                                                 double current_time,
                                                 uint8_t * response_packet,
                                                 int * response_bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( response_packet );
    rocketnet_assert( response_bytes );
    rocketnet_assert( server->encryption_map );
    rocketnet_assert( server->used_token_history );
    rocketnet_assert( server->admitted_token_set );
    rocketnet_assert( server->counters );

    uint64_t * counters = server->counters->values;

    counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED]++;

    *response_bytes = 0;

    // exact size, before anything reads past the prefilter header

    if ( packet_bytes != ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES )
    {
        rocketnet_debug( "server ignored connection request. wrong packet size" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // one lookup, no crypto: an established client's address never provokes
    // anything from this path

    if ( rocketnet_slot_view_address_connected( &server->slots, from ) )
    {
        rocketnet_debug( "server ignored connection request. a client is already connected from this address" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // a mapping already exists: the ordinary retransmitted request whose
    // challenge was lost. resend the stored challenge byte-identical and
    // stop -- a mapping is created once, answered identically for as long as
    // it lives, and replaced never.

    {
        int existing = rocketnet_encryption_map_find( server->encryption_map, from );
        if ( existing != -1 )
        {
            const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, existing );
            memcpy( response_packet, mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
            *response_bytes = ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES;
            rocketnet_debug( "server resent stored connection challenge" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT]++;
            return ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE;
        }
    }

    // fixed-offset comparisons against constants, in the standard's order.
    // the request after the prefilter header: return cookie (24), salt (8),
    // client ephemeral public key (32), signed connect data (576),
    // signature (64).

    const uint8_t * return_cookie = packet_data + ROCKETNET_PREFILTER_BYTES;
    const uint8_t * connection_salt = return_cookie + ROCKETNET_COOKIE_FIELD_BYTES;
    const uint8_t * client_ephemeral_public_key = connection_salt + ROCKETNET_CONNECTION_SALT_BYTES;
    const uint8_t * signed_connect_data = client_ephemeral_public_key + ROCKETNET_KEY_BYTES;
    const uint8_t * signature = signed_connect_data + ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_VERSION;
        if ( rocketnet_read_uint32( &p ) != ROCKETNET_VERSION )
        {
            rocketnet_debug( "server ignored connection request. wrong version" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
    }

    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID;
        if ( rocketnet_read_uint64( &p ) != server->protocol_id )
        {
            rocketnet_debug( "server ignored connection request. wrong protocol id" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
    }

    uint64_t token_expire_timestamp;
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_EXPIRE;
        token_expire_timestamp = rocketnet_read_uint64( &p );
        if ( token_expire_timestamp <= current_timestamp )
        {
            rocketnet_debug( "server ignored connection request. connect token expired" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
    }

    int32_t timeout_seconds;
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_TIMEOUT;
        timeout_seconds = (int32_t) rocketnet_read_uint32( &p );
        if ( timeout_seconds == 0 || ( timeout_seconds < 0 && !server->allow_negative_timeout ) )
        {
            rocketnet_debug( "server ignored connection request. invalid timeout" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
        if ( timeout_seconds > 0 && server->max_timeout_seconds > 0 && timeout_seconds > server->max_timeout_seconds )
            timeout_seconds = server->max_timeout_seconds;
    }

    uint8_t address_flags = signed_connect_data[ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS];
    if ( address_flags == 0 || ( address_flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) != 0 )
    {
        rocketnet_debug( "server ignored connection request. bad address flags" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // either advertised address is accepted, not only the first

    {
        int advertised = 0;
        if ( ( address_flags & ROCKETNET_ADDRESS_FLAG_IPV4 ) && server->advertised_ipv4.type == ROCKETNET_ADDRESS_IPV4 )
        {
            uint8_t encoded[6];
            rocketnet_encode_token_address( encoded, &server->advertised_ipv4, 0 );
            if ( memcmp( encoded, signed_connect_data + ROCKETNET_SIGNED_OFFSET_IPV4, 6 ) == 0 )
                advertised = 1;
        }
        if ( !advertised && ( address_flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) && server->advertised_ipv6.type == ROCKETNET_ADDRESS_IPV6 )
        {
            uint8_t encoded[18];
            rocketnet_encode_token_address( encoded, &server->advertised_ipv6, 1 );
            if ( memcmp( encoded, signed_connect_data + ROCKETNET_SIGNED_OFFSET_IPV6, 18 ) == 0 )
                advertised = 1;
        }
        if ( !advertised )
        {
            rocketnet_debug( "server ignored connection request. token names no address this server is advertised at" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
    }

    if ( sodium_memcmp( signed_connect_data + ROCKETNET_SIGNED_OFFSET_SERVER_KEY, server->server_public_key, ROCKETNET_KEY_BYTES ) != 0 )
    {
        rocketnet_debug( "server ignored connection request. token is sealed to a different server key" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the cookie gate: address validation before any asymmetric work. a
    // request that fails it is answered with a cookie packet -- the one
    // response on a rejection path, and every arrival re-provokes it, so a
    // lost cookie can never strand the client.

    {
        int cookie_ok = 0;
        const uint8_t * p = return_cookie;
        uint64_t cookie_expire = rocketnet_read_uint64( &p );
        const uint8_t * cookie_mac = p;

        // in the spec's order: zero field, then the mac, then expiry -- so
        // COOKIE_EXPIRED only ever counts genuine clients with stale
        // cookies, and a forgery never hides under it

        int mac_matches = 0;
        {
            uint8_t expected_mac[ROCKETNET_COOKIE_MAC_BYTES];
            if ( rocketnet_generate_cookie_mac( expected_mac, from, connection_salt, cookie_expire, server->cookie_key ) &&
                 sodium_memcmp( expected_mac, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 )
            {
                mac_matches = 1;
            }
            else if ( server->has_previous_cookie_key &&
                      rocketnet_generate_cookie_mac( expected_mac, from, connection_salt, cookie_expire, server->previous_cookie_key ) &&
                      sodium_memcmp( expected_mac, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 )
            {
                mac_matches = 1;
            }
        }

        static const uint8_t zero_field[ROCKETNET_COOKIE_FIELD_BYTES] = { 0 };
        if ( memcmp( return_cookie, zero_field, ROCKETNET_COOKIE_FIELD_BYTES ) == 0 )
        {
            rocketnet_debug( "server sent connection cookie. request carried no cookie" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE]++;
        }
        else if ( !mac_matches )
        {
            rocketnet_debug( "server sent connection cookie. cookie did not verify" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY]++;
        }
        else if ( cookie_expire <= current_timestamp )
        {
            rocketnet_debug( "server sent connection cookie. cookie expired" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED]++;
        }
        else
        {
            cookie_ok = 1;
        }

        if ( !cookie_ok )
        {
            uint64_t fresh_expire = current_timestamp + (uint64_t) server->cookie_lifetime_seconds;
            uint8_t fresh_mac[ROCKETNET_COOKIE_MAC_BYTES];
            if ( !rocketnet_generate_cookie_mac( fresh_mac, from, connection_salt, fresh_expire, server->cookie_key ) )
            {
                rocketnet_assert( 0 );      // the source address was parsed; a bad family is a caller bug
                rocketnet_debug( "server ignored connection request. internal error" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
            rocketnet_write_connection_cookie_packet( response_packet, server->title_tag, connection_salt, fresh_expire, fresh_mac );
            *response_bytes = ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES;
            return ROCKETNET_SERVER_PROCESS_SEND_COOKIE;
        }
    }

    // everything below the cookie is expensive; nothing below runs for an
    // address that has not answered

    uint8_t token_identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    rocketnet_connect_token_identifier( token_identifier, signed_connect_data );

    if ( !rocketnet_used_token_history_check( server->used_token_history, token_identifier, from, current_timestamp ) )
    {
        rocketnet_debug( "server ignored connection request. connect token already used by a different address" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the authenticity check: the signature, and only the signature

    if ( !rocketnet_verify_signed_connect_data( signed_connect_data, signature, server->backend_public_key ) )
    {
        rocketnet_debug( "server ignored connection request. signature did not verify" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    struct rocketnet_connect_token_private_t private_data;
    if ( !rocketnet_open_connect_token_private( &private_data, signed_connect_data, server->server_public_key, server->server_secret_key ) )
    {
        rocketnet_debug( "server ignored connection request. could not open sealed private block" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // everything past the open works with recovered secrets, so it lives in
    // its own early-return function and the scrub happens in exactly one
    // place, on every path out of it

    int result = rocketnet_server_process_opened_request( server, from, connection_salt, client_ephemeral_public_key,
                                                          signed_connect_data, &private_data, token_identifier,
                                                          token_expire_timestamp, timeout_seconds,
                                                          current_timestamp, current_time,
                                                          response_packet, response_bytes );

    sodium_memzero( &private_data, sizeof(private_data) );

    return result;
}

static int rocketnet_server_process_opened_request( struct rocketnet_server_protocol_t * server,
                                                    const struct rocketnet_address_t * from,
                                                    const uint8_t * connection_salt,
                                                    const uint8_t * client_ephemeral_public_key,
                                                    const uint8_t * signed_connect_data,
                                                    const struct rocketnet_connect_token_private_t * private_data,
                                                    const uint8_t * token_identifier,
                                                    uint64_t token_expire_timestamp,
                                                    int32_t timeout_seconds,
                                                    uint64_t current_timestamp,
                                                    double current_time,
                                                    uint8_t * response_packet,
                                                    int * response_bytes )
{
    uint64_t * counters = server->counters->values;

    if ( !rocketnet_connect_token_private_consistent( private_data, signed_connect_data ) )
    {
        rocketnet_debug( "server ignored connection request. sealed fields disagree with signed fields" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( rocketnet_slot_view_client_id_connected( &server->slots, private_data->client_id ) )
    {
        rocketnet_debug( "server ignored connection request. a client with this id is already connected" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( rocketnet_admitted_token_set_contains( server->admitted_token_set, token_identifier, current_timestamp ) )
    {
        rocketnet_debug( "server ignored connection request. connect token already admitted its connection" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    rocketnet_used_token_history_record( server->used_token_history, token_identifier, from, token_expire_timestamp, current_timestamp );

    {
        // the server's contribution to the key schedule: a fresh session
        // nonce and a fresh ephemeral keypair, both born with the mapping

        uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
        randombytes_buf( session_nonce, sizeof(session_nonce) );

        uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES];
        uint8_t server_ephemeral_secret_key[ROCKETNET_KEY_BYTES];
        crypto_box_keypair( server_ephemeral_public_key, server_ephemeral_secret_key );

        struct rocketnet_connection_keys_t keys;
        if ( !rocketnet_derive_connection_keys( &keys,
                                                private_data->client_to_server_key,
                                                private_data->server_to_client_key,
                                                connection_salt,
                                                session_nonce,
                                                server_ephemeral_secret_key,
                                                client_ephemeral_public_key,
                                                client_ephemeral_public_key,
                                                server_ephemeral_public_key ) )
        {
            sodium_memzero( server_ephemeral_secret_key, sizeof(server_ephemeral_secret_key) );
            rocketnet_debug( "server ignored connection request. client ephemeral key is a small order point" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

        sodium_memzero( server_ephemeral_secret_key, sizeof(server_ephemeral_secret_key) );

        // no slots: denied, sealed under the handshake keys just derived,
        // carrying the session nonce in the clear. no mapping is created, so
        // a retransmitted request runs this path again and is denied again.

        if ( rocketnet_slot_view_num_free_slots( &server->slots ) <= 0 )
        {
            int denied_bytes = rocketnet_write_sealed_packet( response_packet, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                              server->title_tag, 1, session_nonce, NULL, 0,
                                                              server->protocol_id, connection_salt, NULL,
                                                              keys.handshake_server_to_client );
            sodium_memzero( &keys, sizeof(keys) );
            if ( denied_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
            {
                rocketnet_assert( 0 );      // fixed arguments; a wrong size is a bug, not a condition
                rocketnet_debug( "server ignored connection request. internal error" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
            *response_bytes = denied_bytes;
            rocketnet_debug( "server denied connection request. no client slots available" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS]++;
            return ROCKETNET_SERVER_PROCESS_SEND_DENIED;
        }

        int index = rocketnet_encryption_map_add( server->encryption_map, from, current_time + server->handshake_deadline_seconds );
        if ( index == -1 )
        {
            sodium_memzero( &keys, sizeof(keys) );
            rocketnet_debug( "server ignored connection request. could not add encryption mapping" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING]++;
            counters[ROCKETNET_COUNTER_MAPPING_TABLE_FULL]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, index );
        mapping->keys = keys;
        memcpy( mapping->connection_salt, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES );
        memcpy( mapping->session_nonce, session_nonce, ROCKETNET_SESSION_NONCE_BYTES );
        memcpy( mapping->server_ephemeral_public_key, server_ephemeral_public_key, ROCKETNET_KEY_BYTES );
        memcpy( mapping->client_ephemeral_public_key, client_ephemeral_public_key, ROCKETNET_KEY_BYTES );
        mapping->client_id = private_data->client_id;
        memcpy( mapping->user_data, private_data->user_data, ROCKETNET_USER_DATA_BYTES );
        memcpy( mapping->token_identifier, token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
        mapping->token_expire_timestamp = token_expire_timestamp;
        mapping->timeout_seconds = timeout_seconds;
        mapping->last_receive_time = current_time;

        sodium_memzero( &keys, sizeof(keys) );

        // build the challenge at this mapping's next handshake sequence and
        // store it, so a retransmitted request is answered byte-identical
        // for as long as the mapping lives

        int challenge_bytes = rocketnet_write_sealed_packet( mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                             server->title_tag, mapping->send_sequence_handshake,
                                                             mapping->session_nonce,
                                                             mapping->server_ephemeral_public_key, ROCKETNET_KEY_BYTES,
                                                             server->protocol_id, mapping->connection_salt,
                                                             client_ephemeral_public_key,
                                                             mapping->keys.handshake_server_to_client );
        if ( challenge_bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
        {
            rocketnet_assert( 0 );          // fixed arguments; a wrong size is a bug, not a condition
            rocketnet_encryption_map_remove( server->encryption_map, index );
            rocketnet_debug( "server ignored connection request. internal error" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
        mapping->send_sequence_handshake++;

        memcpy( response_packet, mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES );
        *response_bytes = ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES;

        rocketnet_debug( "server sent connection challenge" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE]++;
        counters[ROCKETNET_COUNTER_MAPPING_CREATED]++;

        return ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE;
    }
}
