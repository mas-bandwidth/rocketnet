/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_server_protocol.h"
#include "rocketnet_platform.h"
#include "rocketnet_read_write.h"
#include "rocketnet_keys.h"
#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet.h"
#include "rocketnet_crypto_tally.h"

#include <sodium.h>
#include <string.h>

// the failed-signature-verify budget: see rocketnet_server.h. the window
// rolls over when the clock passes it, clearing the global count and every
// per-source count together, so a source that exhausted the budget is refused
// for the rest of the window and no longer.

static void rocketnet_signature_verify_budget_decay( struct rocketnet_signature_verify_budget_t * budget,
                                                     uint64_t current_timestamp )
{
    if ( current_timestamp >= budget->window_start + (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS )
    {
        budget->window_start = current_timestamp;
        budget->failure_count = 0;
        memset( budget->sources, 0, sizeof( budget->sources ) );
    }
}

int rocketnet_signature_verify_budget_allow( struct rocketnet_signature_verify_budget_t * budget,
                                             const struct rocketnet_address_t * source,
                                             uint64_t current_timestamp )
{
    rocketnet_signature_verify_budget_decay( budget, current_timestamp );

    // the global budget is not spent: everyone verifies.
    if ( budget->failure_count < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET )
        return 1;

    // the budget is spent: skip a source that has already failed this window,
    // and note whether a free slot remains for a source that has not.
    int empty_slot = -1;
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count == 0 )
            empty_slot = i;
        else if ( rocketnet_address_equal( &budget->sources[i].address, source ) )
            return 0;
    }

    // a first-time source is verified while a free slot can remember it. when
    // the table is full, verifying one would fail and be forgotten, letting
    // one cookie re-verify forever by churning fresh addresses, so refuse it
    // instead.
    if ( empty_slot == -1 )
        return 0;

    return 1;
}

void rocketnet_signature_verify_budget_record_failure( struct rocketnet_signature_verify_budget_t * budget,
                                                       const struct rocketnet_address_t * source,
                                                       uint64_t current_timestamp )
{
    rocketnet_signature_verify_budget_decay( budget, current_timestamp );

    budget->failure_count++;

    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count > 0 &&
             rocketnet_address_equal( &budget->sources[i].address, source ) )
        {
            budget->sources[i].fail_count++;
            return;
        }
    }

    // a source's count is durable for the window: it is only ever reset by
    // the window rolling over, never displaced. when the table is full the
    // failure is already counted in the global total above but the source is
    // not tracked, and the gate has refused a first-time source it cannot
    // remember, so a counted source cannot be re-verified by churn.
    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count == 0 )
        {
            budget->sources[i].address = *source;
            budget->sources[i].fail_count = 1;
            return;
        }
    }
}

// the cookie mac generations the request ladder has paid on this thread.
// the gate-cost test reads the delta across one request: first contact --
// a zero return cookie, the flood's shape -- must pay one mac, the fresh
// cookie the refusal sends, and none for a verification a zero field can
// never pass. thread_local because the request path runs per receive
// thread; like the rest of the dataplane, no shared state between queues.

thread_local uint64_t rocketnet_server_cookie_mac_calls;

static int rocketnet_server_request_cookie_mac( uint8_t * mac,
                                                const struct rocketnet_address_t * from,
                                                const uint8_t * connection_salt,
                                                uint64_t cookie_expire_timestamp,
                                                const uint8_t * cookie_key )
{
    rocketnet_server_cookie_mac_calls++;
    return rocketnet_generate_cookie_mac( mac, from, connection_salt, cookie_expire_timestamp, cookie_key );
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

int rocketnet_server_protocol_process_connection_request( struct rocketnet_server_protocol_t * server,
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
    rocketnet_assert( server->client_slots );
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

    const uint8_t * connection_salt = packet_data + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES;

    // one lookup serves the next two rungs: a mapping with a slot is a
    // connected client, whose address never provokes anything from this
    // path; a mapping without one is a handshake in flight, answered with
    // its stored challenge byte-identical for as long as it lives -- a
    // mapping is created once and replaced never.

    {
        int existing = rocketnet_encryption_map_find( server->encryption_map, from );
        if ( existing != -1 )
        {
            const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, existing );
            if ( mapping->client_index != -1 )
            {
                rocketnet_debug( "server ignored connection request. a client is already connected from this address" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
            if ( memcmp( mapping->connection_salt, connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ) != 0 )
            {
                rocketnet_debug( "server ignored connection request. mapping salt does not match" );
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
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

    uint64_t token_create_timestamp;
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_CREATE;
        token_create_timestamp = rocketnet_read_uint64( &p );
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

        // Check max token lifetime: if max_token_lifetime_seconds > 0, refuse tokens
        // whose lifetime (expire - create) exceeds it. Also refuse if create is in
        // the future beyond a 5-second skew allowance.
        if ( server->max_token_lifetime_seconds > 0 )
        {
            if ( token_create_timestamp > current_timestamp + 5 )
            {
                rocketnet_debug( "server ignored connection request. connect token created in future" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }

            uint64_t lifetime = token_expire_timestamp > token_create_timestamp ?
                                token_expire_timestamp - token_create_timestamp : 0;
            if ( lifetime > (uint64_t) server->max_token_lifetime_seconds )
            {
                rocketnet_debug( "server ignored connection request. connect token lifetime exceeds maximum" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
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
        // cookies, and a forgery never hides under it. the zero field is
        // also the flood's shape, so it is tested first and the macs are
        // computed only for a cookie that could hold one: first contact
        // pays no verification at all, one hash for the fresh cookie below
        // and not one more.

        static const uint8_t zero_field[ROCKETNET_COOKIE_FIELD_BYTES] = { 0 };
        if ( memcmp( return_cookie, zero_field, ROCKETNET_COOKIE_FIELD_BYTES ) == 0 )
        {
            rocketnet_debug( "server sent connection cookie. request carried no cookie" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE]++;
        }
        else
        {
            int mac_matches = 0;
            {
                uint8_t expected_mac[ROCKETNET_COOKIE_MAC_BYTES];
                if ( rocketnet_server_request_cookie_mac( expected_mac, from, connection_salt, cookie_expire, server->cookie_key ) &&
                     sodium_memcmp( expected_mac, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 )
                {
                    mac_matches = 1;
                }
                else if ( server->has_previous_cookie_key &&
                          rocketnet_server_request_cookie_mac( expected_mac, from, connection_salt, cookie_expire, server->previous_cookie_key ) &&
                          sodium_memcmp( expected_mac, cookie_mac, ROCKETNET_COOKIE_MAC_BYTES ) == 0 )
                {
                    mac_matches = 1;
                }
            }

            if ( !mac_matches )
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
        }

        if ( !cookie_ok )
        {
            uint64_t fresh_expire = current_timestamp + (uint64_t) server->cookie_lifetime_seconds;
            uint8_t fresh_mac[ROCKETNET_COOKIE_MAC_BYTES];
            if ( !rocketnet_server_request_cookie_mac( fresh_mac, from, connection_salt, fresh_expire, server->cookie_key ) )
            {
                rocketnet_assert( 0 );      // the source address was parsed; a bad family is a caller bug
                rocketnet_debug( "server ignored connection request. internal error" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
            rocketnet_write_connection_cookie_packet( response_packet, server->filter_value, connection_salt, fresh_expire, fresh_mac );
            *response_bytes = ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES;
            return ROCKETNET_SERVER_PROCESS_SEND_COOKIE;
        }
    }

    // the handshake budget check: after the cookie validates, count and cap
    // post-cookie handshakes per update, per server. requests beyond the
    // budget are dropped before any asymmetric work runs and counted, so a
    // flood of staged attempts cannot spend the whole update on signature
    // verification.
    if ( server->counters->handshakes_processed_this_update >= ROCKETNET_HANDSHAKE_WORK_BUDGET_PER_UPDATE )
    {
        rocketnet_debug( "server dropped connection request. handshake budget exceeded" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED]++;
        return ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED;
    }
    server->counters->handshakes_processed_this_update++;

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

    // the authenticity check: the signature, and only the signature. the
    // per-source failed-verify budget gates it, so one valid cookie cannot
    // buy unlimited Ed25519 verifies from one address.

    if ( !rocketnet_signature_verify_budget_allow( &server->signature_verify_budget, from, current_timestamp ) )
    {
        rocketnet_debug( "server ignored connection request. too many signature failures from this source" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_VERIFY_LIMIT]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( !rocketnet_verify_signed_connect_data( signed_connect_data, signature, server->backend_public_key ) )
    {
        rocketnet_signature_verify_budget_record_failure( &server->signature_verify_budget, from, current_timestamp );
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

    if ( rocketnet_client_slots_find( server->client_slots, private_data->client_id ) != -1 )
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
        rocketnet_crypto_tally.keypair++;
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

        if ( rocketnet_client_slots_num_free( server->client_slots ) <= 0 )
        {
            int denied_bytes = rocketnet_write_sealed_packet( response_packet, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                              server->filter_value, 1, session_nonce, NULL, 0,
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
        mapping->last_send_time = current_time;
        mapping->last_keep_alive_send_time = current_time;

        sodium_memzero( &keys, sizeof(keys) );

        // build the challenge at this mapping's next handshake sequence and
        // store it, so a retransmitted request is answered byte-identical
        // for as long as the mapping lives

        int challenge_bytes = rocketnet_write_sealed_packet( mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                             server->filter_value, mapping->send_sequence_handshake,
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

// the connection response: the packet has already opened under this
// mapping's handshake client-to-server key, which is all it has to prove.
// the checks and the slot assignment follow the standard's order.

static int rocketnet_server_process_connection_response( struct rocketnet_server_protocol_t * server,
                                                         int mapping_index,
                                                         uint64_t current_timestamp,
                                                         double current_time,
                                                         uint8_t * response_packet,
                                                         int * response_bytes )
{
    uint64_t * counters = server->counters->values;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );

    (void) current_time;

    if ( mapping->client_index != -1 )
    {
        rocketnet_spam( "server ignored connection response. client is already connected" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( rocketnet_client_slots_find( server->client_slots, mapping->client_id ) != -1 )
    {
        rocketnet_debug( "server ignored connection response. a client with this id is already connected" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the slots check runs before the admitted-token set takes its entry: a
    // client denied for a full server has not connected, and its token must
    // stay usable for the retry. the set consumes the token only at the
    // moment it actually admits a connection. the set never evicts unexpired
    // entries, so a full set refuses -- fail closed -- with a denied the
    // client can open.

    int denied = 0;

    if ( rocketnet_client_slots_num_free( server->client_slots ) <= 0 )
    {
        rocketnet_debug( "server denied connection response. no client slots available" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS]++;
        denied = 1;
    }

    if ( !denied && !rocketnet_admitted_token_set_add( server->admitted_token_set, mapping->token_identifier, mapping->token_expire_timestamp, current_timestamp ) )
    {
        rocketnet_debug( "server denied connection response. admitted token set is full" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL]++;
        denied = 1;
    }

    if ( denied )
    {
        int denied_bytes = rocketnet_write_sealed_packet( response_packet, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                          server->filter_value, mapping->send_sequence_handshake,
                                                          mapping->session_nonce, NULL, 0,
                                                          server->protocol_id, mapping->connection_salt, NULL,
                                                          mapping->keys.handshake_server_to_client );
        if ( denied_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
        {
            rocketnet_assert( 0 );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
        mapping->send_sequence_handshake++;
        *response_bytes = denied_bytes;
        return ROCKETNET_SERVER_PROCESS_SEND_DENIED;
    }

    int client_index = rocketnet_client_slots_assign( server->client_slots, mapping->client_id, mapping_index, mapping->user_data );
    rocketnet_assert( client_index != -1 );     // num_free was checked above

    mapping->client_index = client_index;
    mapping->established = 1;

    mapping->last_send_time = current_time;

    if ( server->acks )
        rocketnet_server_acks_reset_slot( server->acks, client_index );

    if ( server->reassembly )
        rocketnet_reassembly_set_reset_connection( server->reassembly, client_index );

    rocketnet_debug( "client %d connected", client_index );
    counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED]++;
    counters[ROCKETNET_COUNTER_CLIENT_CONNECTED]++;

    // the first keep-alive: how the client learns its index and transitions
    // to connected

    int keep_alive_bytes = rocketnet_server_write_keep_alive( server, client_index, current_time, response_packet );
    if ( keep_alive_bytes == 0 )
    {
        rocketnet_assert( 0 );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
        return ROCKETNET_SERVER_PROCESS_ACCEPTED;
    }
    *response_bytes = keep_alive_bytes;

    return ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE;
}

// the sealed types through the reading order: size floor, mapping,
// sequence, replay, open, replay advance, ack block, then dispatch. the
// counter groups are per type, so the bookkeeping is table driven and the
// flow is one straight ladder.

// The AEAD integrity limit, per the standard's _Replay Protection_: one
// more sealed packet that did not open on this mapping, across both of its
// keys. On reaching ROCKETNET_INTEGRITY_LIMIT the connection is torn down
// the way a client-sent disconnect tears it down -- the slot released, the
// mapping removed with its keys zeroed, CLIENT_DISCONNECTED bumped when a
// slot was held -- except that nothing is sent, and INTEGRITY_LIMIT_REACHED
// emits once. The counter lands in the disconnected ledger, not the
// timed-out one: CLIENT_CONNECTED = CLIENT_DISCONNECTED + CLIENT_TIMED_OUT
// still holds, and INTEGRITY_LIMIT_REACHED says how many of the disconnects
// were this. A connection under 2^36 forgery attempts has nothing worth
// preserving, and the peer's own timeout ends its side. This is the read
// path, so the check runs in every build.

static void rocketnet_server_integrity_failure( struct rocketnet_server_protocol_t * server,
                                                int mapping_index,
                                                struct rocketnet_encryption_mapping_t * mapping )
{
    uint64_t * counters = server->counters->values;

    mapping->integrity_failures++;

    if ( mapping->integrity_failures < ROCKETNET_INTEGRITY_LIMIT )
        return;

    int client_index = mapping->client_index;
    rocketnet_debug( "server removed encryption mapping. integrity limit reached (client %d)", client_index );
    if ( client_index != -1 )
    {
        rocketnet_client_slots_release( server->client_slots, client_index );
        counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    }
    rocketnet_encryption_map_remove( server->encryption_map, mapping_index );
    counters[ROCKETNET_COUNTER_INTEGRITY_LIMIT_REACHED]++;
}

struct rocketnet_sealed_type_counters_t
{
    int received;
    int wrong_size;
    int no_mapping;
    int zero_sequence;
    int already_received;
    int did_not_open;
    int not_connected;
    int bad_ack_block;
    int wrong_opened_size;
};

static int rocketnet_server_process_sealed_packet( struct rocketnet_server_protocol_t * server,
                                                   const struct rocketnet_address_t * from,
                                                   uint8_t packet_type,
                                                   const uint8_t * packet_data,
                                                   int packet_bytes,
                                                   uint64_t current_timestamp,
                                                   double current_time,
                                                   uint8_t * response_packet,
                                                   int * response_bytes,
                                                   uint8_t * out_payload,
                                                   int * out_payload_bytes,
                                                   int * out_payload_client_index )
{
    uint64_t * counters = server->counters->values;

    static const struct rocketnet_sealed_type_counters_t table[6] = {
        { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING,
          ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE, -1,
          ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN, -1, -1, -1 },
        { ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED, ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE, ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING,
          ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE, ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED,
          ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN, ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED, ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK,
          ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE },
        { ROCKETNET_COUNTER_PAYLOAD_RECEIVED, ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE, ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING,
          ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE, ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED,
          ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN, ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED, ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK,
          ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE },
        { ROCKETNET_COUNTER_DISCONNECT_RECEIVED, ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE, ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING,
          ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE, ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED,
          ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN, ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED, ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK,
          ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE },
        { ROCKETNET_COUNTER_ACK_RECEIVED, ROCKETNET_COUNTER_ACK_WRONG_SIZE, ROCKETNET_COUNTER_ACK_NO_MAPPING,
          ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE, ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED,
          ROCKETNET_COUNTER_ACK_DID_NOT_OPEN, ROCKETNET_COUNTER_ACK_NOT_CONNECTED, ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK,
          ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE },
        { ROCKETNET_COUNTER_FRAGMENT_RECEIVED, ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE, ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING,
          ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE, ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED,
          ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN, ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED, ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK,
          ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE },
    };
    const int table_index = packet_type == ROCKETNET_CONNECTION_ACK_PACKET ? 4 :
                            packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET ? 5 :
                            packet_type - ROCKETNET_CONNECTION_RESPONSE_PACKET;
    const struct rocketnet_sealed_type_counters_t * type_counters = &table[table_index];

    counters[type_counters->received]++;

    // the size table's per-type floor, before any other per-type work: a
    // packet below it is WRONG_SIZE, never a later refusal. the ceiling was
    // enforced by the prefilter; the response's exact size is proven by its
    // open, as before.

    int min_wire_bytes = 0;
    switch ( packet_type )
    {
        case ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET: min_wire_bytes = ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES; break;
        case ROCKETNET_CONNECTION_PAYLOAD_PACKET:    min_wire_bytes = ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;    break;
        case ROCKETNET_CONNECTION_DISCONNECT_PACKET: min_wire_bytes = ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES; break;
        case ROCKETNET_CONNECTION_ACK_PACKET:        min_wire_bytes = ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES;        break;
        case ROCKETNET_CONNECTION_FRAGMENT_PACKET:   min_wire_bytes = ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES;   break;
        default: break;
    }

    if ( min_wire_bytes != 0 && packet_bytes < min_wire_bytes )
    {
        rocketnet_spam( "server ignored sealed packet. below the type's wire floor" );
        counters[type_counters->wrong_size]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET && packet_bytes != ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES )
    {
        rocketnet_spam( "server ignored sealed packet. response wrong size" );
        counters[type_counters->wrong_size]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    int mapping_index = rocketnet_encryption_map_find( server->encryption_map, from );
    if ( mapping_index == -1 )
    {
        rocketnet_spam( "server ignored sealed packet. no mapping for this address" );
        counters[type_counters->no_mapping]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );

    // the phase: the response rides the handshake keys; everything after
    // rides the session keys. handshake types are exempt from replay
    // marking; the session types check before the open and advance after it.

    int session_phase = packet_type != ROCKETNET_CONNECTION_RESPONSE_PACKET;
    const uint8_t * key = session_phase ? mapping->keys.session_client_to_server : mapping->keys.handshake_client_to_server;
    struct rocketnet_replay_protection_t * replay = session_phase ? &mapping->replay_session : &mapping->replay_handshake;

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), replay->highest_opened );

    if ( sequence == 0 )
    {
        rocketnet_spam( "server ignored sealed packet. zero sequence" );
        counters[type_counters->zero_sequence]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( session_phase && rocketnet_replay_already_received( replay, sequence ) )
    {
        rocketnet_spam( "server ignored sealed packet. already received" );
        counters[type_counters->already_received]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // type 3 binds the client ephemeral into its associated data; the
    // session types carry the minimal 21 bytes

    const uint8_t * ephemeral = packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET ? mapping->client_ephemeral_public_key : NULL;

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, ROCKETNET_RECEIVE_CEILING, packet_data, packet_bytes, packet_type,
                                                     sequence, server->protocol_id, mapping->connection_salt,
                                                     ephemeral, key );
    if ( opened_bytes < 0 )
    {
        rocketnet_spam( "server ignored sealed packet. did not open" );
        counters[type_counters->did_not_open]++;
        rocketnet_server_integrity_failure( server, mapping_index, mapping );
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the response proves 24 bytes of zero padding -- required, checked, the
    // anti-amplification floor -- and branches off before the session
    // machinery: handshake types carry no ack block

    if ( packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
    {
        if ( opened_bytes != ROCKETNET_RESPONSE_PADDING_BYTES )
        {
            rocketnet_spam( "server ignored connection response. wrong size" );
            counters[type_counters->did_not_open]++;
            rocketnet_server_integrity_failure( server, mapping_index, mapping );
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
        for ( int i = 0; i < ROCKETNET_RESPONSE_PADDING_BYTES; i++ )
        {
            if ( opened[i] != 0 )
            {
                rocketnet_spam( "server ignored connection response. padding is not zero" );
                counters[type_counters->did_not_open]++;
                rocketnet_server_integrity_failure( server, mapping_index, mapping );
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
        }

        if ( sequence > replay->highest_opened )
            replay->highest_opened = sequence;

        mapping->last_receive_time = current_time;

        return rocketnet_server_process_connection_response( server, mapping_index, current_timestamp, current_time, response_packet, response_bytes );
    }

    // session types: the slot gate runs before the replay-window mark, per
    // STANDARD.md _Reading packets_ -- a session packet whose mapping has no
    // assigned slot is ignored after the open and before the mark -- and
    // _Processing session packets at the server_. The NoAdvanceBeforeConnect
    // invariant of tla/ReplayWindow.tla: the session window of a mapping
    // without a slot never moves.

    // session traffic for a mapping with no slot is a lost-response race or
    // an injection; the retransmitted response repairs the former

    if ( mapping->client_index == -1 )
    {
        rocketnet_spam( "server ignored sealed packet. not connected" );
        counters[type_counters->not_connected]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    rocketnet_replay_advance( replay, sequence );

    int client_index = mapping->client_index;

    // the ack block at the front of the opened data, validated on the
    // reconstructed 64-bit largest against this end's own highest sent
    // session sequence: a malformed block refuses the whole packet, and the
    // type data behind it is never examined

    struct rocketnet_ack_block_t block;
    int block_bytes = rocketnet_ack_block_read( &block, opened, opened_bytes, mapping->send_sequence_session - 1 );
    if ( block_bytes == 0 )
    {
        rocketnet_spam( "server ignored sealed packet. bad ack block" );
        counters[type_counters->bad_ack_block]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the opened size, less the block, must match the type

    const uint8_t * type_data = opened + block_bytes;
    int type_data_bytes = opened_bytes - block_bytes;

    if ( ( packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET && type_data_bytes != 8 ) ||
         ( packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET && type_data_bytes < 1 ) ||
         ( packet_type == ROCKETNET_CONNECTION_DISCONNECT_PACKET && type_data_bytes != 0 ) ||
         ( packet_type == ROCKETNET_CONNECTION_ACK_PACKET && type_data_bytes != 0 ) ||
         ( packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET && type_data_bytes < ROCKETNET_FRAGMENT_HEADER_BYTES + 1 ) )
    {
        rocketnet_spam( "server ignored sealed packet. wrong opened size" );
        counters[type_counters->wrong_opened_size]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // both checks passed: only now does the packet keep the connection
    // alive and confirm the slot, the block's authenticated delivery
    // information applies -- newly acknowledged packets marked and
    // surfaced, rtt sampled from the first ack of each sequence -- and
    // eliciting types add to the ack debt that obliges a block back. type 8
    // never elicits: that exemption is what makes the ack conversation
    // terminate.

    mapping->last_receive_time = current_time;

    if ( !rocketnet_client_slots_confirmed( server->client_slots, client_index ) )
        rocketnet_client_slots_confirm( server->client_slots, client_index );

    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];

        rocketnet_ack_endpoint_record_received( endpoint, sequence, packet_bytes, current_time );

        int before = server->acks->num_acked;
        rocketnet_ack_endpoint_process_block( endpoint, &block, current_time,
                                              server->acks->acked_packet_ids, server->acks->acked_capacity, &server->acks->num_acked,
                                              &counters[ROCKETNET_COUNTER_ACK_UNKNOWN_SEQUENCE],
                                              &counters[ROCKETNET_COUNTER_ACKED_BATCH_FULL] );
        for ( int i = before; i < server->acks->num_acked; i++ )
            server->acks->acked_client_index[i] = client_index;

        if ( packet_type != ROCKETNET_CONNECTION_ACK_PACKET )
            endpoint->ack_debt++;
    }

    if ( packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET )
    {
        rocketnet_spam( "server received keep alive from client %d", client_index );
        counters[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED]++;
        return ROCKETNET_SERVER_PROCESS_ACCEPTED;
    }

    if ( packet_type == ROCKETNET_CONNECTION_ACK_PACKET )
    {
        rocketnet_spam( "server received standalone ack from client %d", client_index );
        counters[ROCKETNET_COUNTER_ACK_ACCEPTED]++;
        return ROCKETNET_SERVER_PROCESS_ACCEPTED;
    }

    if ( packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
    {
        rocketnet_spam( "server received payload from client %d (%d bytes)", client_index, type_data_bytes );
        counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED]++;
        memcpy( out_payload, type_data, (size_t) type_data_bytes );
        *out_payload_bytes = type_data_bytes;
        *out_payload_client_index = client_index;
        return ROCKETNET_SERVER_PROCESS_ACCEPTED;
    }

    if ( packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET )
    {
        // fragmentation's own self-contained bookkeeping, per the
        // standard: the ack block above applied like any session packet's
        // -- delivery information does not become false because the bytes
        // behind it are refused -- and the fragment's checks run here,
        // each refusal on its named counter. a completed payload sits in
        // the reassembly's delivered arena, reported through the
        // completed fields in arrival position, attributed to its slot.

        if ( !server->reassembly )
        {
            // reassembly is created wherever fragments are processed; a
            // server built without it cannot reassemble, and the accepted
            // count keeps the arrival invariant honest in release
            rocketnet_assert( 0 );
            counters[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED]++;
            return ROCKETNET_SERVER_PROCESS_ACCEPTED;
        }

        static const struct rocketnet_reassembly_counters_t fragment_counters = {
            ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
            ROCKETNET_COUNTER_FRAGMENT_STALE,
            ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
            ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
            ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
            ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
            ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
            ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
            ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
            ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
            ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
            ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

        int accepted = rocketnet_reassembly_process_fragment( server->reassembly, client_index,
                                                              type_data, type_data_bytes,
                                                              counters, &fragment_counters );

        if ( server->reassembly->completed_bytes > 0 )
            *out_payload_client_index = client_index;

        return accepted ? ROCKETNET_SERVER_PROCESS_ACCEPTED : ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the disconnect: release the slot and destroy the mapping immediately,
    // never answered -- the sender's redundant copies are the delivery
    // mechanism

    rocketnet_debug( "client %d disconnected", client_index );
    counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED]++;
    counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    rocketnet_client_slots_release( server->client_slots, client_index );
    rocketnet_encryption_map_remove( server->encryption_map, mapping_index );

    return ROCKETNET_SERVER_PROCESS_ACCEPTED;
}

int rocketnet_server_protocol_process_packet( struct rocketnet_server_protocol_t * server,
                                     const struct rocketnet_address_t * from,
                                     const uint8_t * packet_data,
                                     int packet_bytes,
                                     uint64_t current_timestamp,
                                     double current_time,
                                     uint8_t * response_packet,
                                     int * response_bytes,
                                     uint8_t * out_payload,
                                     int * out_payload_bytes,
                                     int * out_payload_client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( response_packet );
    rocketnet_assert( response_bytes );
    rocketnet_assert( out_payload );
    rocketnet_assert( out_payload_bytes );
    rocketnet_assert( out_payload_client_index );

    uint64_t * counters = server->counters->values;

    counters[ROCKETNET_COUNTER_PACKETS_RECEIVED]++;

    *response_bytes = 0;
    *out_payload_bytes = 0;
    *out_payload_client_index = -1;

    if ( server->reassembly )
    {
        server->reassembly->completed_payload = NULL;
        server->reassembly->completed_bytes = 0;
    }

    int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, server->filter_value );
    if ( packet_type < 0 )
    {
        rocketnet_spam( "server dropped packet. failed prefilter" );
        counters[ rocketnet_prefilter_counter( packet_type ) ]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // types 10 through 255 are reserved and ignored by a conforming
    // receiver

    if ( packet_type > ROCKETNET_CONNECTION_FRAGMENT_PACKET )
    {
        rocketnet_spam( "server dropped packet. reserved type" );
        counters[ROCKETNET_COUNTER_RESERVED_TYPE]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( packet_type == ROCKETNET_CONNECTION_REQUEST_PACKET )
        return rocketnet_server_protocol_process_connection_request( server, from, packet_data, packet_bytes,
                                                            current_timestamp, current_time,
                                                            response_packet, response_bytes );

    // a server never processes what only a server sends -- denied,
    // challenge, cookie arriving here are reflections or junk

    if ( packet_type == ROCKETNET_CONNECTION_DENIED_PACKET ||
         packet_type == ROCKETNET_CONNECTION_CHALLENGE_PACKET ||
         packet_type == ROCKETNET_CONNECTION_COOKIE_PACKET )
    {
        rocketnet_spam( "server dropped packet. wrong direction" );
        counters[ROCKETNET_COUNTER_WRONG_DIRECTION]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    return rocketnet_server_process_sealed_packet( server, from, (uint8_t) packet_type, packet_data, packet_bytes,
                                                   current_timestamp, current_time,
                                                   response_packet, response_bytes,
                                                   out_payload, out_payload_bytes, out_payload_client_index );
}

void rocketnet_server_protocol_update( struct rocketnet_server_protocol_t * server,
                              double current_time )
{
    rocketnet_assert( server );
    rocketnet_assert( server->encryption_map );
    rocketnet_assert( server->client_slots );
    rocketnet_assert( server->counters );

    // reset this server's per-update handshake budget for the next update
    server->counters->handshakes_processed_this_update = 0;

    uint64_t * counters = server->counters->values;

    int iterator = -1;
    while ( rocketnet_encryption_map_next( server->encryption_map, &iterator ) )
    {
        const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, iterator );

        if ( !mapping->established )
        {
            if ( current_time >= mapping->handshake_deadline )
            {
                rocketnet_debug( "server removed encryption mapping. handshake deadline expired" );
                rocketnet_encryption_map_remove( server->encryption_map, iterator );
                counters[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED]++;
            }
            continue;
        }

        if ( mapping->timeout_seconds > 0 && current_time >= mapping->last_receive_time + mapping->timeout_seconds )
        {
            int client_index = mapping->client_index;
            rocketnet_debug( "client %d timed out", client_index );
            if ( client_index != -1 )
                rocketnet_client_slots_release( server->client_slots, client_index );
            rocketnet_encryption_map_remove( server->encryption_map, iterator );
            counters[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT]++;
            counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT]++;
        }
    }
}

static struct rocketnet_encryption_mapping_t * rocketnet_server_connected_mapping( struct rocketnet_server_protocol_t * server,
                                                                                   int client_index )
{
    if ( !rocketnet_client_slots_connected( server->client_slots, client_index ) )
        return NULL;

    // A connected slot USUALLY has a live mapping behind it, and must never be
    // assumed to. rocketnet_client_slots_assign accepts -1 as a mapping index,
    // so a connected slot without one is a state this API permits rather than
    // an invariant violation -- and the only thing that stood here was an
    // assert, which is absent from every shipping build. Release then indexed
    // the mapping array at -1 and wrote eight bytes in front of it.

    int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        // in the shipped protocol this cannot happen: the only production
        // assignment passes an index that came from a live mapping. it is
        // counted rather than merely handled because the consequence of a
        // future regression producing it is a client that quietly stops
        // receiving keep-alives and times out -- a silent failure needs a
        // name, or the first person to hit it has nothing to look at.

        server->counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING]++;
        return NULL;
    }

    return rocketnet_encryption_map_entry( server->encryption_map, mapping_index );
}

// Builds one sealed session packet for a mapping: the sealed plaintext
// opens with the ack block built from this connection's receive state, and
// the per-packet type data follows it. Every session send passes through
// here or through the payload's in-place twin, so sending any packet sends
// a block, and sending a block resets the ack debt. The sent packet is
// recorded in the delivery bookkeeping -- these sequences carry no packet
// id and surface nothing when acknowledged.

static int rocketnet_server_write_session_packet( struct rocketnet_server_protocol_t * server,
                                                  struct rocketnet_encryption_mapping_t * mapping,
                                                  int client_index,
                                                  uint8_t packet_type,
                                                  const uint8_t * type_data,
                                                  int type_data_bytes,
                                                  double current_time,
                                                  uint8_t * out_packet,
                                                  uint64_t * out_sequence )
{
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 8];
    rocketnet_assert( type_data_bytes <= 8 );

    // the block truncates to the budget's remaining room -- the capacity
    // arithmetic is what keeps the packet inside the send budget, whatever
    // the replay window's depth

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - type_data_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &mapping->replay_session );
    if ( type_data_bytes > 0 )
        memcpy( plaintext + block_bytes, type_data, (size_t) type_data_bytes );

    uint64_t sequence = mapping->send_sequence_session;

    int bytes = rocketnet_write_sealed_packet( out_packet, packet_type,
                                               server->filter_value, sequence,
                                               NULL, plaintext, block_bytes + type_data_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );

    mapping->send_sequence_session++;

    // the sequence this packet actually carried, reported to the caller
    // rather than left to be re-derived from the counter. re-reading
    // send_sequence_session - 1 is only correct while nothing else can send
    // on this mapping in between, which is a contract stated in another
    // file; a caller that needs the sequence gets it from here.

    if ( out_sequence )
        *out_sequence = sequence;

    if ( server->acks && client_index >= 0 )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        if ( !rocketnet_ack_endpoint_record_sent( endpoint, sequence, bytes, 0, 0, 0, current_time ) )
        {
            rocketnet_platform_atomic_add_u64( &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW], 1 );
        }
        endpoint->ack_debt = 0;
    }

    mapping->last_send_time = current_time;
    return bytes;
}

int rocketnet_server_write_keep_alive( struct rocketnet_server_protocol_t * server,
                                       int client_index,
                                       double current_time,
                                       uint8_t * out_packet )
{
    rocketnet_assert( server );
    rocketnet_assert( out_packet );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    uint8_t keep_alive_data[8];
    uint8_t * p = keep_alive_data;
    rocketnet_write_uint32( &p, (uint32_t) client_index );
    rocketnet_write_uint32( &p, (uint32_t) rocketnet_client_slots_max_clients( server->client_slots ) );

    uint64_t keep_alive_sequence = 0;
    int bytes = rocketnet_server_write_session_packet( server, mapping, client_index,
                                                       ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,
                                                       keep_alive_data, 8, current_time, out_packet,
                                                       &keep_alive_sequence );

    // a failed seal is not a send, and MUST NOT stamp the clock below: the
    // assert vanishes in release, and stamping there would buy a full send
    // interval of silence on the exact field the unacknowledged pacing
    // depends on. same convention as write_session_packet's own zero return.

    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes >= ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES );

    // the keep-alive's own clock, which only keep-alives move. it is what
    // paces generation to a client that has not acknowledged one, where
    // ordinary traffic must not suppress -- see the due predicate below.

    mapping->last_keep_alive_send_time = current_time;

    // and the tag that makes this packet's acknowledgment mean something:
    // write_session_packet recorded the sequence just above and reported it
    // back, and an ack naming it is this client's claim to have received a
    // keep-alive.

    if ( server->acks && client_index >= 0 )
        rocketnet_ack_endpoint_mark_keep_alive( &server->acks->endpoints[client_index], keep_alive_sequence );

    rocketnet_spam( "server sent keep alive to client %d", client_index );

    return bytes;
}

int rocketnet_server_protocol_write_ack_if_due( struct rocketnet_server_protocol_t * server,
                                                int client_index,
                                                double current_time,
                                                uint8_t * out_packet )
{
    rocketnet_assert( server );
    rocketnet_assert( out_packet );

    if ( !server->acks )
        return 0;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    if ( server->acks->endpoints[client_index].ack_debt < ROCKETNET_ACK_DEBT_THRESHOLD )
        return 0;

    int bytes = rocketnet_server_write_session_packet( server, mapping, client_index,
                                                       ROCKETNET_CONNECTION_ACK_PACKET,
                                                       NULL, 0, current_time, out_packet, NULL );

    rocketnet_spam( "server sent standalone ack to client %d", client_index );

    return bytes;
}

int rocketnet_server_protocol_write_keep_alive_if_due( struct rocketnet_server_protocol_t * server,
                                              int client_index,
                                              double current_time,
                                              uint8_t * out_packet )
{
    rocketnet_assert( server );
    rocketnet_assert( out_packet );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    // Which clock the suppression reads is the whole of this rule, and it
    // turns on one question: HAS THIS CLIENT ACKNOWLEDGED A KEEP-ALIVE?
    // Not whether it received one -- the server cannot know that -- but
    // whether it has said so.
    //
    // UNTIL IT HAS, generation runs off the keep-alive's own clock and
    // ordinary traffic does not suppress it. Reading last_send_time there is
    // what let a continuous payload stream starve establishment forever
    // (#90): every payload restamped the clock the repair was waiting on,
    // and the loss walkthrough's recurrence guarantee became false. (That
    // walkthrough then read "at most a send interval away"; this PR replaced
    // the numeric bound, which the discrete check cannot support, with the
    // recurrence the invariant actually needs.) The keep-alive is the only
    // packet carrying the client index, and a client that lacks its index
    // cannot re-provoke one, so this is the one repair that must not be
    // suppressible.
    //
    // ONCE IT HAS, the client has claimed to hold its index and be
    // connected. If the claim is true nothing is waiting on a keep-alive,
    // and ordinary traffic is the keep-alive -- last_send_time, as before.
    // What happens when it is false is the second failure direction below.
    //
    // THE SIGNAL IS THE ACK, and it is a CLAIM rather than a proof. A
    // keep-alive carries the client index, and processing a keep-alive is
    // the client's ONLY transition to _connected_, so a client that
    // acknowledges one is claiming to have received the packet that would
    // have connected it. The server cannot verify the claim -- an ack block
    // is the peer's statement about its own receive state -- and does not
    // need to; see below.
    //
    // WHY THIS TEST AND NOT THE CONFIRMED FLAG: the defect class is "a
    // client that has not established is starved of the packet that would
    // establish it", so the test must not be satisfiable by a client that is
    // behaving correctly but has not established. The confirmed flag was: it
    // is set by ANY sealed packet, and _Acks_ invites a client in _sending
    // connection response_ to send a standalone ack, so a CONFORMANT client
    // confirmed itself before it had its index and stranded itself. This
    // test is of course satisfied by every client that acknowledges a
    // keep-alive -- that IS the design. The restriction that matters is
    // AMONG CLIENTS THAT HAVE NOT ESTABLISHED: there it is satisfiable only
    // by one naming a sequence it never opened, which _Acks_ forbids when
    // building a block. So no correct client trips it BEFORE it holds its
    // index, which is what the confirmed flag could not promise.
    //
    // BOTH FAILURE DIRECTIONS. Lost or delayed acks leave the latch unset
    // and unsuppressed keep-alives continue -- more, never fewer, bounded by
    // the send rate. An over-claiming client satisfies the test without its
    // index and strands ITSELF: the block rides inside a packet sealed under
    // that connection's own session keys and applies only to that slot, so
    // it cannot be produced by anyone else for it and no other client's
    // pacing is affected.

    double send_rate = server->send_rate > 0.0 ? server->send_rate : 10.0;

    // no ack bookkeeping means the claim can never be observed; fall back to
    // the unsuppressed clock, which is the safe direction

    int acknowledged = server->acks && client_index >= 0
                        ? (int) server->acks->endpoints[client_index].keep_alive_acked
                        : 0;

    double last = acknowledged ? mapping->last_send_time : mapping->last_keep_alive_send_time;

    if ( current_time - last < 1.0 / send_rate )
        return 0;

    return rocketnet_server_write_keep_alive( server, client_index, current_time, out_packet );
}

// The fill-phase pair behind the acquire/commit send surface. Any number of
// game threads run these between update and the flush; the phase contract is
// what makes them safe -- mappings are created and torn down only by update,
// which never runs during a fill, so the lookup is over stable state and the
// only shared words touched are the sequence (an atomic add) and the send
// timestamp (an atomic store).

int rocketnet_server_protocol_start_payload( struct rocketnet_server_protocol_t * server,
                                             int client_index,
                                             struct rocketnet_address_t * to,
                                             uint64_t * sequence )
{
    rocketnet_assert( server );
    rocketnet_assert( to );
    rocketnet_assert( sequence );

    if ( !rocketnet_client_slots_connected( server->client_slots, client_index ) )
        return 0;

    int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        server->counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING]++;
        return 0;
    }

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );

    *to = *rocketnet_encryption_map_address( server->encryption_map, mapping_index );
    *sequence = rocketnet_platform_atomic_add_u64( &mapping->send_sequence_session, 1 );

    return 1;
}

int rocketnet_server_protocol_seal_payload_frame( struct rocketnet_server_protocol_t * server,
                                                  int client_index,
                                                  uint64_t sequence,
                                                  uint64_t packet_id,
                                                  uint8_t * wire_start,
                                                  int payload_bytes,
                                                  double current_time )
{
    rocketnet_assert( server );
    rocketnet_assert( wire_start );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return 0;

    // the application's bytes come first: the block truncates to the
    // budget's remaining room, oldest entries dropped first. the 7-byte
    // minimum always fits.

    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    int ack_block_bytes = rocketnet_ack_block_build( ack_block, block_capacity, &mapping->replay_session );

    int wire_bytes = rocketnet_write_sealed_payload_in_place( wire_start, server->filter_value, sequence,
                                                              ack_block, ack_block_bytes,
                                                              payload_bytes, server->protocol_id,
                                                              mapping->connection_salt,
                                                              mapping->keys.session_server_to_client );
    if ( wire_bytes == 0 )
        return 0;


    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        if ( !rocketnet_ack_endpoint_record_sent( endpoint, sequence, wire_bytes, packet_id, 1, 0, current_time ) )
        {
            rocketnet_platform_atomic_add_u64( &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW], 1 );
        }
        rocketnet_platform_atomic_store_u64( &endpoint->ack_debt, 0 );      // fill-phase threads reset concurrently
    }

    rocketnet_platform_atomic_store_double( &mapping->last_send_time, current_time );
    return wire_bytes;
}

int rocketnet_server_protocol_write_payload( struct rocketnet_server_protocol_t * server,
                                    int client_index,
                                    const uint8_t * payload_data,
                                    int payload_bytes,
                                    double current_time,
                                    uint8_t * out_packet )
{
    rocketnet_assert( server );
    rocketnet_assert( payload_data );
    rocketnet_assert( out_packet );

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PAYLOAD_BYTES )
        return 0;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_MAX_PAYLOAD_BYTES];

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - payload_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &mapping->replay_session );
    memcpy( plaintext + block_bytes, payload_data, (size_t) payload_bytes );

    uint64_t sequence = mapping->send_sequence_session;

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                               server->filter_value, sequence,
                                               NULL, plaintext, block_bytes + payload_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;

    mapping->send_sequence_session++;

    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        if ( !rocketnet_ack_endpoint_record_sent( endpoint, sequence, bytes, 0, 0, 0, current_time ) )
        {
            rocketnet_platform_atomic_add_u64( &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW], 1 );
        }
        endpoint->ack_debt = 0;
    }

    rocketnet_spam( "server sent payload to client %d (%d bytes)", client_index, payload_bytes );

    mapping->last_send_time = current_time;
    return bytes;
}

int rocketnet_server_protocol_client_address( struct rocketnet_server_protocol_t * server,
                                              int client_index,
                                              struct rocketnet_address_t * out_address )
{
    rocketnet_assert( server );
    rocketnet_assert( out_address );

    if ( !rocketnet_client_slots_connected( server->client_slots, client_index ) )
        return 0;

    int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        server->counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING]++;
        return 0;
    }

    *out_address = *rocketnet_encryption_map_address( server->encryption_map, mapping_index );

    return 1;
}

// The fragmented send path, the client protocol's mirror against the
// client's mapping. The payload id and the sequence block are each one
// atomic add, so any number of fill threads split payloads concurrently.

uint32_t rocketnet_server_protocol_next_fragment_payload_id( struct rocketnet_server_protocol_t * server, int client_index )
{
    rocketnet_assert( server );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    // the counter is 64-bit so the fold to the wire's u32 can skip zero
    // forever -- zero is what an uninitialized sender produces

    uint64_t raw = rocketnet_platform_atomic_add_u64( &mapping->send_fragment_payload_id, 1 );
    return (uint32_t) ( 1 + ( raw - 1 ) % 0xFFFFFFFFULL );
}

int rocketnet_server_protocol_start_fragments( struct rocketnet_server_protocol_t * server, int client_index, int fragment_count, uint64_t * base_sequence )
{
    rocketnet_assert( server );
    rocketnet_assert( base_sequence );
    rocketnet_assert( fragment_count >= 1 );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    *base_sequence = rocketnet_platform_atomic_add_u64( &mapping->send_sequence_session, (uint64_t) fragment_count );

    return 1;
}

int rocketnet_server_protocol_seal_fragment( struct rocketnet_server_protocol_t * server,
                                             int client_index,
                                             uint64_t sequence,
                                             uint32_t payload_id,
                                             int fragment_index,
                                             int fragment_count,
                                             const uint8_t * fragment_data,
                                             int fragment_bytes,
                                             uint64_t aggregate_id,
                                             double current_time,
                                             uint8_t * out_packet )
{
    rocketnet_assert( server );
    rocketnet_assert( fragment_data );
    rocketnet_assert( out_packet );

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    if ( payload_id == 0 ||
         fragment_count < 2 || fragment_count > ROCKETNET_MAX_FRAGMENTS ||
         fragment_index < 0 || fragment_index >= fragment_count ||
         fragment_bytes < 1 || fragment_bytes > ROCKETNET_MAX_FRAGMENT_BYTES )
        return 0;

    // the fragment bytes come first: the stride is chosen against the
    // minimum block, so a current block with ranges truncates to the
    // remaining room. the 7-byte minimum always fits.

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_FRAGMENT_BYTES];

    int block_capacity = ROCKETNET_SEND_BUDGET - ROCKETNET_PACKET_OVERHEAD_BYTES - ROCKETNET_FRAGMENT_HEADER_BYTES - fragment_bytes;
    if ( block_capacity > ROCKETNET_ACK_BLOCK_MAX_BYTES )
        block_capacity = ROCKETNET_ACK_BLOCK_MAX_BYTES;
    int block_bytes = rocketnet_ack_block_build( plaintext, block_capacity, &mapping->replay_session );

    rocketnet_write_fragment_header( plaintext + block_bytes, payload_id, fragment_index, fragment_count );
    memcpy( plaintext + block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES, fragment_data, (size_t) fragment_bytes );

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                               server->filter_value, sequence,
                                               NULL, plaintext, block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );


    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        if ( !rocketnet_ack_endpoint_record_sent( endpoint, sequence, bytes, 0, 0, aggregate_id, current_time ) )
        {
            rocketnet_platform_atomic_add_u64( &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW], 1 );
        }
        rocketnet_platform_atomic_store_u64( &endpoint->ack_debt, 0 );      // fill-phase threads reset concurrently
    }

    rocketnet_platform_atomic_store_double( &mapping->last_send_time, current_time );
    return bytes;
}

int rocketnet_server_protocol_write_fragments( struct rocketnet_server_protocol_t * server,
                                               int client_index,
                                               const uint8_t * payload_data,
                                               int payload_bytes,
                                               double current_time,
                                               uint8_t * out_packets,
                                               int * out_packet_bytes,
                                               int max_packets )
{
    rocketnet_assert( server );
    rocketnet_assert( payload_data );
    rocketnet_assert( out_packets );
    rocketnet_assert( out_packet_bytes );

    int fragment_count, fragment_stride;
    if ( !rocketnet_fragment_plan( payload_bytes, &fragment_count, &fragment_stride ) )
        return 0;

    if ( fragment_count > max_packets )
        return 0;

    uint32_t payload_id = rocketnet_server_protocol_next_fragment_payload_id( server, client_index );
    if ( payload_id == 0 )
        return 0;

    uint64_t base_sequence;
    if ( !rocketnet_server_protocol_start_fragments( server, client_index, fragment_count, &base_sequence ) )
        return 0;

    for ( int i = 0; i < fragment_count; i++ )
    {
        const int fragment_bytes = i < fragment_count - 1 ? fragment_stride : payload_bytes - fragment_stride * ( fragment_count - 1 );
        int wire_bytes = rocketnet_server_protocol_seal_fragment( server, client_index, base_sequence + (uint64_t) i, payload_id,
                                                                  i, fragment_count,
                                                                  payload_data + i * fragment_stride, fragment_bytes,
                                                                  0, current_time,
                                                                  out_packets + i * ROCKETNET_SEND_BUDGET );
        rocketnet_assert( wire_bytes > 0 );         // connected, and every argument in range by the plan
        out_packet_bytes[i] = wire_bytes;
    }

    rocketnet_spam( "server sent fragmented payload %u to client %d (%d bytes in %d fragments)", payload_id, client_index, payload_bytes, fragment_count );

    return fragment_count;
}

void rocketnet_server_protocol_disconnect_client( struct rocketnet_server_protocol_t * server,
                                         int client_index,
                                         uint8_t * out_packets,
                                         int * out_packet_count,
                                         int * out_packet_bytes )
{
    rocketnet_assert( server );
    rocketnet_assert( out_packets );
    rocketnet_assert( out_packet_count );
    rocketnet_assert( out_packet_bytes );

    uint64_t * counters = server->counters->values;

    *out_packet_count = 0;
    *out_packet_bytes = 0;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return;

    // the same redundant burst the client sends, sealed under the session
    // keys at consecutive sequences, each opening with the same ack block --
    // the receive state does not move between them -- so every packet in
    // the burst shares one wire size. the copies are the delivery
    // mechanism, and nothing is retransmitted afterwards because the
    // mapping is gone.

    int burst_bytes = 0;

    for ( int i = 0; i < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS; i++ )
    {
        int bytes = rocketnet_server_write_session_packet( server, mapping, client_index,
                                                           ROCKETNET_CONNECTION_DISCONNECT_PACKET,
                                                           NULL, 0, mapping->last_send_time,
                                                           out_packets + i * ROCKETNET_SEND_BUDGET, NULL );
        rocketnet_assert( bytes >= ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
        rocketnet_assert( i == 0 || bytes == burst_bytes );
        burst_bytes = bytes;
    }

    *out_packet_count = ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS;
    *out_packet_bytes = burst_bytes;

    // release the slot and destroy the mapping immediately, exactly as when
    // a client-sent disconnect arrives

    rocketnet_debug( "server disconnected client %d", client_index );
    counters[ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT]++;
    counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;

    int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );
    rocketnet_client_slots_release( server->client_slots, client_index );
    rocketnet_encryption_map_remove( server->encryption_map, mapping_index );
}
