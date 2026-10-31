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
#include "rocketnet_internal.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_seams.h"

#include <sodium.h>
#include <string.h>

#if ROCKETNET_DEV
// Consume at the protocol entry that owns the writer, before any return. A
// fault is therefore one-shot even when the packet is refused before it
// reaches the selected writer.
static int rocketnet_server_test_take_writer_fault( struct rocketnet_server_protocol_t * server, int writer )
{
    const int fault = server->test_fail_fixed_writer;
    if ( fault != writer )
        return 0;
    server->test_fail_fixed_writer = ROCKETNET_SERVER_TEST_FAIL_WRITER_NONE;

    return 1;
}
#endif // #if ROCKETNET_DEV

// the connection ids, standard/MESSAGES.md m1.4 rule 5: a slot takes the next id from the
// server-wide counter when it is assigned and drops to 0 when it is released

uint64_t rocketnet_server_protocol_connection_id( const struct rocketnet_server_protocol_t * server, int client_index )
{
    if ( !server->connection_ids || client_index < 0 || client_index >= rocketnet_client_slots_max_clients( server->client_slots ) )
        return 0;
    return server->connection_ids[client_index];
}

int rocketnet_server_protocol_end_reason( const struct rocketnet_server_protocol_t * server, int client_index )
{
    if ( !server->end_reasons || client_index < 0 || client_index >= rocketnet_client_slots_max_clients( server->client_slots ) )
        return ROCKETNET_DISCONNECT_REASON_NONE;
    return server->end_reasons[client_index];
}

static void rocketnet_server_connection_id_assigned( struct rocketnet_server_protocol_t * server, int client_index )
{
    if ( !server->connection_ids || client_index < 0 || client_index >= rocketnet_client_slots_max_clients( server->client_slots ) )
        return;
    server->connection_ids[client_index] = ++server->next_connection_id;

    // proofs/tla/RNSlots.tla Connect: the new connection has not ended

    if ( server->end_reasons )
        server->end_reasons[client_index] = ROCKETNET_DISCONNECT_REASON_NONE;
}

// the failed-signature-verify budget: see rocketnet_server.h. the window
// rolls over when the clock passes it, clearing the global count and every
// per-source count together, so a source that exhausted the budget is refused
// for the rest of the window and no longer. a clock that steps back before
// the window's start (an NTP step, a VM resume) starts a fresh window too, so
// a recorded failure refuses its source for at most the window length of
// clock movement in either direction (proofs/tla/RNSecurityTables.tla
// FloodWindowBounded).

static void rocketnet_signature_verify_budget_decay( struct rocketnet_signature_verify_budget_t * budget,
                                                     uint64_t current_timestamp )
{
    if ( current_timestamp < budget->window_start ||
         current_timestamp >= budget->window_start + (uint64_t) ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS )
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

    struct rocketnet_address_t host = *source;
    host.port = 0;

    // the global budget is not spent: everyone verifies.

    if ( budget->failure_count < ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET )
        return 1;

    // the budget is spent: refuse a source that has already failed this
    // window. a source with no recorded failure is verified, whether or not
    // the table has a free slot to remember it: refusing it would refuse
    // every honest first-time client once forging sources fill the table
    // (proofs/tla/RNSecurityTables.tla HonestConnectsUnderFlood). the verifies such
    // sources cost are bounded by the per-update handshake work budget, which
    // runs before this gate, and each needs a valid cookie for its own address.

    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count > 0 &&
             rocketnet_address_equal( &budget->sources[i].address, &host ) )
            return 0;
    }

    return 1;
}

void rocketnet_signature_verify_budget_record_failure( struct rocketnet_signature_verify_budget_t * budget,
                                                       const struct rocketnet_address_t * source,
                                                       uint64_t current_timestamp )
{
    rocketnet_signature_verify_budget_decay( budget, current_timestamp );

    struct rocketnet_address_t host = *source;
    host.port = 0;

    budget->failure_count++;

    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count > 0 &&
             rocketnet_address_equal( &budget->sources[i].address, &host ) )
        {
            budget->sources[i].fail_count++;
            return;
        }
    }

    // a source's count is durable for the window: it is only ever reset by
    // the window rolling over, never displaced. when the table is full the
    // failure is counted in the global total above but the source is not
    // tracked, so that source is verified again on its next request; those
    // verifies are bounded by the per-update handshake work budget.

    for ( int i = 0; i < ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS; i++ )
    {
        if ( budget->sources[i].fail_count == 0 )
        {
            budget->sources[i].address = host;
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

#if ROCKETNET_DEV

thread_local uint64_t rocketnet_server_cookie_mac_calls;

// Read the receive thread's tally in the TU that owns its TLS definition.
// An external TLS read from the test object can leave a weak initializer
// unresolved in optimized sanitizer builds.
uint64_t rocketnet_server_cookie_mac_count()
{
    return rocketnet_server_cookie_mac_calls;
}

#endif // #if ROCKETNET_DEV

static int rocketnet_server_request_cookie_mac( uint8_t * mac,
                                                const struct rocketnet_address_t * from,
                                                const uint8_t * connection_salt,
                                                uint64_t cookie_expire_timestamp,
                                                const uint8_t * cookie_key )
{
#if ROCKETNET_DEV
    rocketnet_server_cookie_mac_calls++;
#endif // #if ROCKETNET_DEV
    return rocketnet_generate_cookie_mac( mac, from, connection_salt, cookie_expire_timestamp, cookie_key );
}

// whether an address is one this server advertises. on a dual-stack server
// -- bound to the IPv6 wildcard, so it also advertises its IPv4 wildcard --
// a v4-mapped token address normalises to native IPv4 and matches as the
// loopback or wildcard the IPv4 client addressed.

static int rocketnet_server_address_is_advertised( const struct rocketnet_server_protocol_t * server,
                                                   struct rocketnet_address_t * addr )
{
    // dual_stack: a server bound at the IPv6 wildcard ("[::]"), which
    // advertises the IPv6 wildcard beside the IPv4 wildcard. A server that
    // names both families (second_public_address) advertises the two named
    // addresses and is not dual_stack here.
    static const uint16_t ipv6_wildcard[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const int dual_stack = server->advertised_ipv4.type == ROCKETNET_ADDRESS_IPV4 &&
                           server->advertised_ipv6.type == ROCKETNET_ADDRESS_IPV6 &&
                           memcmp( server->advertised_ipv6.data.ipv6, ipv6_wildcard, sizeof( ipv6_wildcard ) ) == 0;

    if ( addr->type == ROCKETNET_ADDRESS_IPV6 && rocketnet_address_is_ipv4_in_ipv6( addr ) )
        rocketnet_address_convert_ipv6_to_ipv4( addr );

    if ( addr->type == ROCKETNET_ADDRESS_IPV6 )
        return server->advertised_ipv6.type == ROCKETNET_ADDRESS_IPV6 &&
               rocketnet_address_equal( addr, &server->advertised_ipv6 );

    if ( server->advertised_ipv4.type == ROCKETNET_ADDRESS_IPV4 &&
         rocketnet_address_equal( addr, &server->advertised_ipv4 ) )
        return 1;

    // on a dual-stack server the loopback is an IPv4 address the socket
    // serves too

    if ( dual_stack && addr->data.ipv4[0] == 127 && addr->data.ipv4[1] == 0 &&
         addr->data.ipv4[2] == 0 && addr->data.ipv4[3] == 1 &&
         addr->port == server->advertised_ipv4.port )
        return 1;

    return 0;
}

// The arrival family's token address (proofs/tla/ServerConnection.tla
// TokenAddressAdvertised). A v4-mapped source is canonicalised to IPv4
// first. The token's other-family address is not consulted. A token with
// no IPv4 flag whose IPv6 address is v4-mapped is that IPv4 address.

static void rocketnet_server_read_token_address( struct rocketnet_address_t * addr, int ipv6, const uint8_t * signed_connect_data )
{
    memset( addr, 0, sizeof( *addr ) );

    if ( ipv6 )
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_IPV6;
        addr->type = ROCKETNET_ADDRESS_IPV6;
        for ( int i = 0; i < 8; i++ )
            addr->data.ipv6[i] = rocketnet_read_uint16( &p );
        addr->port = rocketnet_read_uint16( &p );
        return;
    }

    const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_IPV4;
    addr->type = ROCKETNET_ADDRESS_IPV4;
    for ( int i = 0; i < 4; i++ )
        addr->data.ipv4[i] = *p++;
    addr->port = rocketnet_read_uint16( &p );
}

static int rocketnet_server_token_address_is_advertised( const struct rocketnet_server_protocol_t * server,
                                                         const struct rocketnet_address_t * from,
                                                         uint8_t address_flags,
                                                         const uint8_t * signed_connect_data )
{
    struct rocketnet_address_t arrival = *from;
    if ( arrival.type == ROCKETNET_ADDRESS_IPV6 && rocketnet_address_is_ipv4_in_ipv6( &arrival ) )
        rocketnet_address_convert_ipv6_to_ipv4( &arrival );

    struct rocketnet_address_t addr;

    if ( arrival.type == ROCKETNET_ADDRESS_IPV4 )
    {
        if ( address_flags & ROCKETNET_ADDRESS_FLAG_IPV4 )
        {
            rocketnet_server_read_token_address( &addr, 0, signed_connect_data );
            return rocketnet_server_address_is_advertised( server, &addr );
        }

        if ( address_flags & ROCKETNET_ADDRESS_FLAG_IPV6 )
        {
            rocketnet_server_read_token_address( &addr, 1, signed_connect_data );

            if ( addr.type == ROCKETNET_ADDRESS_IPV6 && rocketnet_address_is_ipv4_in_ipv6( &addr ) )
            {
                rocketnet_address_convert_ipv6_to_ipv4( &addr );
                return rocketnet_server_address_is_advertised( server, &addr );
            }
        }

        return 0;
    }

    if ( arrival.type != ROCKETNET_ADDRESS_IPV6 || ( address_flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) == 0 )
        return 0;
    rocketnet_server_read_token_address( &addr, 1, signed_connect_data );
    if ( rocketnet_address_is_ipv4_in_ipv6( &addr ) )
        return 0;
    return rocketnet_server_address_is_advertised( server, &addr );
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
                                                    double current_time,
                                                    int fail_denied_writer,
                                                    uint8_t * response_packet,
                                                    int * response_bytes );

// proofs/tla/ReceiveOrder.tla Next
// proofs/tla/ReceiveOrder.tla RefuseStep: a rung the request fails returns its
// refusal there, releasing a claim it wrote; AdvanceStep: a rung it passes
// moves it to the next, the cheap rungs before the crypto ones.
// proofs/tla/ServerConnection.tla ServerRecvRequest
// proofs/tla/RNSecurityTables.tla Request
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
    rocketnet_assert( server->client_slots );
    rocketnet_assert( server->counters );

    int fail_denied_writer = 0;
#if ROCKETNET_DEV
    fail_denied_writer = rocketnet_server_test_take_writer_fault( server, ROCKETNET_SERVER_TEST_FAIL_REQUEST_DENIED_WRITER );
#endif // #if ROCKETNET_DEV

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
            struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, existing );
            if ( mapping->control->client_index != -1 )
            {
                rocketnet_debug( "server ignored connection request. a client is already connected from this address" );
                counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }

            // standard/TRANSPORT.md t12 rule 3: a mapping answers every request from its address with its stored challenge, whatever the request's salt.
            // t12 rule 3 and t17 rule 4: only a request whose salt matches the mapping's refreshes the half-open idle deadline.

            if ( sodium_memcmp( connection_salt, mapping->connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ) == 0 )
                mapping->control->last_receive_time = current_time;
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
        if ( rocketnet_read_uint32( &p ) != ROCKETNET_WIRE_VERSION )
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

        // a token created more than the 30-second skew allowance after the
        // server's clock is refused, always: there is no maximum lifetime.
        // Subtract only after ordering so the allowance cannot wrap at UINT64_MAX.

        if ( token_create_timestamp > current_timestamp &&
             token_create_timestamp - current_timestamp > ROCKETNET_TOKEN_CREATION_SKEW_SECONDS )
        {
            rocketnet_debug( "server ignored connection request. connect token created in future" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }
    }

    int32_t timeout_seconds;
    {
        const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_TIMEOUT;
        timeout_seconds = (int32_t) rocketnet_read_uint32( &p );
        if ( timeout_seconds <= 0 )
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

    // TokenAddressAdvertised: the arrival family's token address is one
    // this server advertises. The other family in the token is not read.

    if ( !rocketnet_server_token_address_is_advertised( server, from, address_flags, signed_connect_data ) )
    {
        rocketnet_debug( "server ignored connection request. token names no address this server is advertised at" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( sodium_memcmp( signed_connect_data + ROCKETNET_SIGNED_OFFSET_SERVER_KEY, server->server_public_key, ROCKETNET_KEY_BYTES ) != 0 )
    {
        rocketnet_debug( "server ignored connection request. token is sealed to a different server key" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // Receives precede the transport sweep. Rotate before verifying or
    // issuing cookies so a delayed update cannot immediately discard a
    // cookie that this receive just issued.

    rocketnet_server_protocol_rotate_cookie_key_if_due( server, current_time );

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
                          current_time < server->previous_cookie_key_expire_time &&
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

            rocketnet_write_connection_cookie_packet( response_packet, server->packet_filter, connection_salt, fresh_expire, fresh_mac );
            *response_bytes = ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES;
            return ROCKETNET_SERVER_PROCESS_SEND_COOKIE;
        }
    }

    // the handshake budget check: after the cookie validates, count and cap
    // post-cookie handshakes per update, per server, at the configured
    // handshakes_per_update. requests beyond the budget are dropped before
    // any asymmetric work runs and counted, so a flood of staged attempts
    // cannot spend the whole update on signature verification; a dropped
    // client's next request is processed in a later update
    // (proofs/tla/RNSecurityTables.tla HonestRequest).

    if ( server->counters->handshakes_processed_this_update >= (uint64_t) server->handshakes_per_update )
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

    // the token-to-address claim, the map capacity and the claim capacity are
    // the remaining cheap rungs, all above the signature and all read-only.
    // the claim rung runs before the capacity rung, as proofs/tla/ReceiveOrder.tla's
    // Pos orders them: used, verifybudget, claim, capacity, claimfull
    // (standard/TRANSPORT.md t12 rule 9).
    // the first address to present a request whose signature verifies owns
    // the token until the handshake deadline or until its response binds the
    // token; a request from a second address is refused here, so one token
    // buys one half-open mapping and no asymmetric work (proofs/tla/ReceiveOrder.tla,
    // ClaimCheck, Capacity and ClaimWrite).

    if ( !rocketnet_used_token_history_claim_check( server->used_token_history, token_identifier, from, current_time ) )
    {
        rocketnet_debug( "server ignored connection request. connect token claimed by a different address" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( rocketnet_encryption_map_full( server->encryption_map ) )
    {
        rocketnet_debug( "server ignored connection request. could not add encryption mapping" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING]++;
        counters[ROCKETNET_COUNTER_MAPPING_TABLE_FULL]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the claim-capacity rung refuses here, read-only: the claim is written
    // only once the signature verifies, so a request that proves nothing
    // writes nothing (proofs/tla/ReceiveOrder.tla, ClaimCheck and ClaimWrite). a
    // full claim table is its own reason; the mapping table had room, so it
    // is not a mapping failure (standard/TRANSPORT.md t12 rule 9, step 5;
    // proofs/tla/ReceiveOrder.tla, the claimfull rung).

    if ( !rocketnet_used_token_history_claim_can_record( server->used_token_history, token_identifier, current_time ) )
    {
        rocketnet_debug( "server ignored connection request. could not claim connect token" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLAIM_TABLE_FULL]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    if ( !rocketnet_verify_signed_connect_data( signed_connect_data, signature, server->backend_public_key ) )
    {
        rocketnet_signature_verify_budget_record_failure( &server->signature_verify_budget, from, current_timestamp );
        rocketnet_debug( "server ignored connection request. signature did not verify" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // nothing between the capacity check and here touches the claim table,
    // so this record cannot fail; a failure is a bug, refused closed

    if ( !rocketnet_used_token_history_claim_record( server->used_token_history, token_identifier, from,
                                                     current_time + server->handshake_deadline_seconds, current_time ) )
    {
        rocketnet_assert( 0 );
        rocketnet_debug( "server ignored connection request. could not claim connect token" );
        counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLAIM_TABLE_FULL]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    struct rocketnet_connect_token_private_t private_data;
    // a sealed block that does not open is a refusal past the verify: the
    // claim just recorded is released (proofs/tla/RNSecurityTables.tla RequestRefused)

    if ( !rocketnet_open_connect_token_private( &private_data, signed_connect_data, server->server_public_key, server->server_secret_key ) )
    {
        rocketnet_used_token_history_claim_release( server->used_token_history, token_identifier, from );
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
                                                          current_time, fail_denied_writer, response_packet, response_bytes );

    sodium_memzero( &private_data, sizeof( private_data ) );

    // Only a live mapping retains the request claim. Every refusal, including
    // a sealed denial, releases it immediately instead of waiting for expiry
    // (proofs/tla/RNSecurityTables.tla RequestRefused: the claim recorded past the
    // verify is released in the same step and no mapping is made).

    if ( result != ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
        rocketnet_used_token_history_claim_release( server->used_token_history, token_identifier, from );

    return result;
}

// proofs/tla/RNSecurityTables.tla Request
// proofs/tla/ReceiveOrder.tla AcceptStep: past the last rung the request's address
// gets its mapping (rocketnet_encryption_map_add).
static int rocketnet_server_process_opened_request( struct rocketnet_server_protocol_t * server,
                                                    const struct rocketnet_address_t * from,
                                                    const uint8_t * connection_salt,
                                                    const uint8_t * client_ephemeral_public_key,
                                                    const uint8_t * signed_connect_data,
                                                    const struct rocketnet_connect_token_private_t * private_data,
                                                    const uint8_t * token_identifier,
                                                    uint64_t token_expire_timestamp,
                                                    int32_t timeout_seconds,
                                                    double current_time,
                                                    int fail_denied_writer,
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

    // no used-token binding record here: the request already holds a claim
    // for its address until the handshake deadline, but a request proves
    // nothing about who holds the token's keys, so the binding that outlives
    // the deadline waits for a response that opens under the handshake key
    // (TRANSPORT.md t12 rule 13;
    // proofs/tla/RNSecurityTables.tla, BoundOnlyToOpener)

    {
        // the server's contribution to the key schedule: a fresh session
        // nonce and a fresh ephemeral keypair, both born with the mapping

        uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
        randombytes_buf( session_nonce, sizeof( session_nonce ) );

        // Cheap refusals come before cryptography (TRANSPORT.md t12 rule 9 and
        // t17 rule 13: the capacity and budget refusals run before any
        // signature, seal or keypair work; t12 rule 11: a request that finds
        // no free slot is denied, sealed under the handshake server-to-client
        // key; ReceiveOrder CheapRefusalsFirst). The handshake keys need
        // no ephemeral pair, so derive them alone and check the slots before
        // any keypair or scalar multiplication. A full server seals the same
        // denial as before (same packet, sequence one, nonce in the clear).

        struct rocketnet_connection_keys_t keys;
        rocketnet_derive_handshake_keys( &keys,
                                         private_data->client_to_server_key,
                                         private_data->server_to_client_key,
                                         connection_salt,
                                         session_nonce );

        if ( rocketnet_client_slots_num_free( server->client_slots ) <= 0 )
        {
            int denied_bytes = 0;
            if ( !fail_denied_writer )
                denied_bytes = rocketnet_write_sealed_packet( response_packet, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                              server->packet_filter, 1, session_nonce, NULL, 0,
                                                              server->protocol_id, connection_salt, NULL,
                                                              keys.handshake_server_to_client );
            sodium_memzero( &keys, sizeof( keys ) );

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

        uint8_t server_ephemeral_public_key[ROCKETNET_KEY_BYTES];
        uint8_t server_ephemeral_secret_key[ROCKETNET_KEY_BYTES];
        ROCKETNET_CRYPTO_TALLY( keypair );
        if ( crypto_box_keypair( server_ephemeral_public_key, server_ephemeral_secret_key ) != 0 )
        {
            // the create path's discipline: wipe what was written and refuse
            // the request; nothing here has touched the mapping table

            sodium_memzero( server_ephemeral_secret_key, sizeof( server_ephemeral_secret_key ) );
            sodium_memzero( server_ephemeral_public_key, sizeof( server_ephemeral_public_key ) );
            sodium_memzero( &keys, sizeof( keys ) );
            rocketnet_debug( "server ignored connection request. internal error" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

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
            sodium_memzero( server_ephemeral_secret_key, sizeof( server_ephemeral_secret_key ) );
            sodium_memzero( &keys, sizeof( keys ) );
            rocketnet_debug( "server ignored connection request. client ephemeral key is a small order point" );
            counters[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

        sodium_memzero( server_ephemeral_secret_key, sizeof( server_ephemeral_secret_key ) );

        int index = rocketnet_encryption_map_add( server->encryption_map, from, current_time + server->handshake_deadline_seconds );
        if ( index == -1 )
        {
            sodium_memzero( &keys, sizeof( keys ) );
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
        mapping->control->timeout_seconds = timeout_seconds;
        mapping->control->last_receive_time = current_time;
        mapping->last_send_time = current_time;
        mapping->last_keep_alive_send_time = current_time;

        sodium_memzero( &keys, sizeof( keys ) );

        // build the challenge at this mapping's next handshake sequence and
        // store it, so a retransmitted request is answered byte-identical
        // for as long as the mapping lives

        int challenge_bytes = rocketnet_write_sealed_packet( mapping->challenge_packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET,
                                                             server->packet_filter, mapping->send_sequence_handshake,
                                                             mapping->session_nonce,
                                                             mapping->server_ephemeral_public_key, ROCKETNET_KEY_BYTES,
                                                             server->protocol_id, mapping->connection_salt,
                                                             client_ephemeral_public_key,
                                                             mapping->keys.handshake_server_to_client );
        if ( challenge_bytes != ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES )
        {
            rocketnet_assert( 0 );          // fixed arguments; a wrong size is a bug, not a condition
            rocketnet_encryption_map_remove_current( server->encryption_map, index );
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

// proofs/tla/ServerConnection.tla ServerRecvResponse
// proofs/tla/RNSecurityTables.tla Response
static int rocketnet_server_process_connection_response( struct rocketnet_server_protocol_t * server,
                                                         int mapping_index,
                                                         uint64_t current_timestamp,
                                                         double current_time,
                                                         int fail_denied_writer,
                                                         uint8_t * response_packet,
                                                         int * response_bytes )
{
    uint64_t * counters = server->counters->values;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );

    // the binding: this address has proven it holds the token's keys, which a
    // request never proves, so a replayed request from another address binds
    // nothing. it replaces the request claim with an entry that lives until
    // the token expires; a no-op when the token is already bound, and an
    // expired token is not recorded (proofs/tla/RNSecurityTables.tla,
    // BoundOnlyToOpener; proofs/tla/MCReceiveOrder.tla, ResponseBind)

    if ( mapping->token_expire_timestamp > current_timestamp )
    {
        rocketnet_used_token_history_record( server->used_token_history, mapping->token_identifier,
                                             rocketnet_encryption_map_address( server->encryption_map, mapping_index ),
                                             mapping->token_expire_timestamp, current_timestamp );
    }

    rocketnet_used_token_history_claim_release( server->used_token_history, mapping->token_identifier,
                                                rocketnet_encryption_map_address( server->encryption_map, mapping_index ) );

    if ( mapping->control->client_index != -1 )
    {
        rocketnet_spam( "server ignored connection response. client is already connected" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // one token, one live connection (proofs/tla/RNSecurityTables.tla,
    // OneTokenOneLiveConnection; TRANSPORT.md t12 rule 13). a token carries one
    // client id, so with the binding at the response, when two addresses each
    // hold a mapping for one token, the first to be admitted connects and the
    // second is refused here while that connection lives. no admitted-token
    // set: once the connection closes, its slot is free and the token may
    // connect again inside its expiry.

    if ( rocketnet_client_slots_find( server->client_slots, mapping->client_id ) != -1 )
    {
        rocketnet_debug( "server ignored connection response. a client with this id is already connected" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the server denies a response only when it is full: a client denied has
    // not connected, and its token stays usable for the retry

    if ( rocketnet_client_slots_num_free( server->client_slots ) <= 0 )
    {
        int denied_bytes = 0;
        if ( !fail_denied_writer )
            denied_bytes = rocketnet_write_sealed_packet( response_packet, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                          server->packet_filter, mapping->send_sequence_handshake,
                                                          mapping->session_nonce, NULL, 0,
                                                          server->protocol_id, mapping->connection_salt, NULL,
                                                          mapping->keys.handshake_server_to_client );
        if ( denied_bytes != ROCKETNET_CONNECTION_DENIED_WIRE_BYTES )
        {
            rocketnet_assert( 0 );
            rocketnet_debug( "server ignored connection response. internal error" );
            counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

        mapping->send_sequence_handshake++;
        *response_bytes = denied_bytes;
        rocketnet_debug( "server denied connection response. no client slots available" );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS]++;
        return ROCKETNET_SERVER_PROCESS_SEND_DENIED;
    }

    // proofs/tla/RNXdpReplay.tla Connect: a new session takes a slot and its opened high water stays at zero.
    // proofs/tla/ReplayWindow.tla Connect: the mapping takes a slot and its replay window is left unchanged.
    // proofs/tla/RNSlots.tla Connect: the slot's connection owner becomes the new connection id.

    const uint64_t mapping_generation =
        rocketnet_encryption_map_owner_generation( server->encryption_map, mapping_index );
    int client_index = rocketnet_client_slots_assign( server->client_slots, mapping->client_id, mapping_index,
                                                      mapping_generation, mapping->user_data );
    rocketnet_assert( client_index != -1 );     // num_free was checked above
    rocketnet_server_connection_id_assigned( server, client_index );

    mapping->control->client_index = client_index;
    mapping->control->established = 1;

    mapping->last_send_time = current_time;

    rocketnet_debug( "client %d connected", client_index );
    counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED]++;
    counters[ROCKETNET_COUNTER_CLIENT_CONNECTED]++;

    // the first keep-alive: how the client learns its index and transitions
    // to connected

    int keep_alive_bytes = rocketnet_server_write_keep_alive( server, client_index, current_time, response_packet );
    if ( keep_alive_bytes == 0 )
    {
        rocketnet_assert( 0 );
        counters[ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR]++;
        return ROCKETNET_SERVER_PROCESS_ACCEPTED;
    }

    *response_bytes = keep_alive_bytes;

    return ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE;
}

// the sealed types through the reading order: size floor, mapping,
// sequence, replay, open, replay advance, ack block, then dispatch. the
// counter groups are per type, so the bookkeeping is table driven and the
// flow is one straight ladder.

// proofs/tla/MCReceiveOrder.tla IdleRemove: a half-open mapping can leave before its
// handshake deadline -- by the idle timeout or the integrity limit -- and its
// request claim leaves with it. The claim is released before the mapping is
// removed, so a claim never outlives the mapping that holds it and the claim
// window is min(handshake deadline, half-open idle deadline).

static void rocketnet_server_release_mapping_claim( struct rocketnet_server_protocol_t * server,
                                                    int mapping_index )
{
    const struct rocketnet_encryption_mapping_t * mapping =
        rocketnet_encryption_map_entry( server->encryption_map, mapping_index );
    rocketnet_used_token_history_claim_release( server->used_token_history, mapping->token_identifier,
                                                rocketnet_encryption_map_address( server->encryption_map, mapping_index ) );
}

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
// preserving, and the client's own timeout ends its side. This is the read
// path, so the check runs in every build.

static void rocketnet_server_integrity_failure( struct rocketnet_server_protocol_t * server,
                                                int mapping_index,
                                                struct rocketnet_encryption_mapping_t * mapping )
{
    uint64_t * counters = server->counters->values;

    mapping->integrity_failures++;

    if ( mapping->integrity_failures < ROCKETNET_INTEGRITY_LIMIT )
        return;

    int client_index = mapping->control->client_index;
    rocketnet_debug( "server removed encryption mapping. integrity limit reached (client %d)", client_index );

    // the reason is NONE: the integrity limit is neither the client's
    // goodbye nor a timeout

    if ( client_index != -1 )
    {
        rocketnet_server_connection_reset( server, client_index, ROCKETNET_DISCONNECT_REASON_NONE );
        counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    }
    else
    {
        // proofs/tla/MCReceiveOrder.tla IdleRemove: the slotless mapping leaves
        // alone, as the idle timeout does, and releases its token's claim
        rocketnet_server_release_mapping_claim( server, mapping_index );
        rocketnet_encryption_map_remove_current( server->encryption_map, mapping_index );
    }

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
    int padding_not_zero;
    int not_connected;
    int bad_ack_block;
    int wrong_opened_size;
};

// the established types count the same reasons; the handshake response counts fewer, so its row is spelled out
#define ROCKETNET_SEALED_TYPE_COUNTERS( type ) \
    { ROCKETNET_COUNTER_##type##_RECEIVED, ROCKETNET_COUNTER_##type##_WRONG_SIZE, ROCKETNET_COUNTER_##type##_NO_MAPPING, \
      ROCKETNET_COUNTER_##type##_ZERO_SEQUENCE, ROCKETNET_COUNTER_##type##_ALREADY_RECEIVED, \
      ROCKETNET_COUNTER_##type##_DID_NOT_OPEN, -1, ROCKETNET_COUNTER_##type##_NOT_CONNECTED, ROCKETNET_COUNTER_##type##_BAD_ACK_BLOCK, \
      ROCKETNET_COUNTER_##type##_WRONG_OPENED_SIZE }

// The sealed types. known_mapping_index is the caller's lookup for this
// address, or ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP. The packet opens into
// opened (ROCKETNET_RECEIVE_CEILING bytes, the caller's): an accepted
// payload is delivered where it opened, *out_payload pointing at its type
// data inside opened, so its plaintext is written once.
// proofs/tla/ServerConnection.tla ServerRecvSession: a sealed session packet opens
// only under its mapping's keys and only for a mapping with a slot; a
// keep-alive confirms the slot, a disconnect releases it with the mapping.
// proofs/tla/RNSecurityTables.tla Disconnect: that release frees the client's slot
// (rocketnet_server_connection_reset).
static int rocketnet_server_process_sealed_packet( struct rocketnet_server_protocol_t * server,
                                                   const struct rocketnet_address_t * from,
                                                   int known_mapping_index,
                                                   uint8_t packet_type,
                                                   const uint8_t * packet_data,
                                                   int packet_bytes,
                                                   uint64_t current_timestamp,
                                                   double current_time,
                                                   uint8_t * response_packet,
                                                   int * response_bytes,
                                                   uint8_t * opened,
                                                   int out_payload_capacity,
                                                   const uint8_t ** out_payload,
                                                   int * out_payload_bytes,
                                                   int * out_payload_client_index )
{
    uint64_t * counters = server->counters->values;

    int fail_response_denied_writer = 0;
#if ROCKETNET_DEV
    if ( packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET )
        fail_response_denied_writer = rocketnet_server_test_take_writer_fault( server, ROCKETNET_SERVER_TEST_FAIL_RESPONSE_DENIED_WRITER );
#endif // #if ROCKETNET_DEV

    static const struct rocketnet_sealed_type_counters_t table[6] = {
        { ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED, ROCKETNET_COUNTER_CONNECTION_RESPONSE_WRONG_SIZE, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING,
          ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE, -1,
          ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN, ROCKETNET_COUNTER_CONNECTION_RESPONSE_PADDING_NOT_ZERO, -1, -1, -1 },
        ROCKETNET_SEALED_TYPE_COUNTERS( KEEP_ALIVE ),
        ROCKETNET_SEALED_TYPE_COUNTERS( PAYLOAD ),
        ROCKETNET_SEALED_TYPE_COUNTERS( DISCONNECT ),
        ROCKETNET_SEALED_TYPE_COUNTERS( ACK ),
        ROCKETNET_SEALED_TYPE_COUNTERS( FRAGMENT ),
    };

#undef ROCKETNET_SEALED_TYPE_COUNTERS

    const int table_index = packet_type == ROCKETNET_CONNECTION_ACK_PACKET ? 4 : packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET ? 5
                                                                                                                                     : packet_type - ROCKETNET_CONNECTION_RESPONSE_PACKET;
    const struct rocketnet_sealed_type_counters_t * type_counters = &table[table_index];

    counters[type_counters->received]++;

    // the size table's per-type floor, before any other per-type work: a
    // packet below it is WRONG_SIZE, never a later refusal. the ceiling was
    // enforced by the prefilter; the response's exact size is proven by its
    // open, as before.

    const int min_wire_bytes = rocketnet_session_min_wire_bytes( packet_type );

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

    int mapping_index = known_mapping_index != ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP ? known_mapping_index : rocketnet_encryption_map_find( server->encryption_map, from );
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
    struct rocketnet_replay_protection_t * replay = session_phase ? &mapping->replay_session : NULL;
    const uint64_t highest_opened = session_phase ? mapping->replay_session.highest_opened : mapping->handshake_highest_opened;

    const uint8_t * p = packet_data + ROCKETNET_PREFILTER_BYTES;
    uint64_t sequence = rocketnet_reconstruct_sequence( rocketnet_read_uint32( &p ), highest_opened );

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

    // the slot gate, before any decryption, per TRANSPORT.md t10 step 12: a
    // session packet for a mapping with no assigned slot (a half-open
    // connection) is ignored unopened. The server acts on no session packet
    // before the slot exists, so an open here was only ever spent on a
    // packet dropped either way: a client that gives up in sending
    // connection response sends its disconnect burst to a mapping that dies
    // at its own deadline, and a lost response is repaired by the client's
    // retransmission, never by its session traffic. The drop is
    // NOT_CONNECTED, which says only that the mapping holds no slot: the
    // packet was never opened, so it is counted neither forged nor authentic,
    // it touches neither DID_NOT_OPEN nor the integrity-failure count, and
    // the session window never moves (proofs/tla/ReplayWindow.tla
    // NoAdvanceBeforeConnect and NoOpenBeforeConnect). The response is the
    // one sealed type a slotless mapping exists to receive, so it passes.

    if ( session_phase && mapping->control->client_index == -1 )
    {
        rocketnet_spam( "server ignored sealed packet. not connected" );
        counters[type_counters->not_connected]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // type 3 binds the client ephemeral into its associated data; the
    // session types carry the minimal 21 bytes

    const uint8_t * ephemeral = packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET ? mapping->client_ephemeral_public_key : NULL;

    int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, ROCKETNET_RECEIVE_CEILING, packet_data, packet_bytes, packet_type,
                                                     sequence, server->protocol_id, mapping->connection_salt,
                                                     ephemeral, key );
#if ROCKETNET_DEV
    if ( opened_bytes >= 0 && packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
        server->test_payload_writes++;
#endif // #if ROCKETNET_DEV
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
        // the wire size check above pins the response at
        // ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES, so an opened response
        // always holds exactly the padding
        static_assert( ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES == ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES + ROCKETNET_PACKET_TAG_BYTES + ROCKETNET_RESPONSE_PADDING_BYTES,
                       "a response's plaintext is its padding" );
        rocketnet_assert( opened_bytes == ROCKETNET_RESPONSE_PADDING_BYTES );

        for ( int i = 0; i < ROCKETNET_RESPONSE_PADDING_BYTES; i++ )
        {
            if ( opened[i] != 0 )
            {
                // the packet opened, so it is authentic: a refusal of its
                // contents, not a forgery, and the integrity-failure count
                // does not move (standard/TRANSPORT.md t9 rule 8).
                rocketnet_spam( "server ignored connection response. padding is not zero" );
                counters[type_counters->padding_not_zero]++;
                return ROCKETNET_SERVER_PROCESS_IGNORED;
            }
        }

        if ( sequence > mapping->handshake_highest_opened )
            mapping->handshake_highest_opened = sequence;

        // A response retransmission is not fresh session activity. Only a
        // half-open mapping may refresh its idle clock from a response.

        if ( mapping->control->client_index == -1 )
            mapping->control->last_receive_time = current_time;

        return rocketnet_server_process_connection_response( server, mapping_index, current_timestamp, current_time,
                                                             fail_response_denied_writer, response_packet, response_bytes );
    }

    // proofs/tla/RNXdpReplay.tla UserOpen: a genuine session packet opens and raises the opened high water.
    // session types: the mapping holds a slot (the gate above), so the
    // sequence is marked now, after the open (proofs/tla/ReplayWindow.tla
    // NoAdvanceBeforeConnect).

    rocketnet_replay_advance( replay, sequence );

    int client_index = mapping->control->client_index;

    // the ack block at the front of the opened data: the empty block on its
    // wire fields, every other block on the reconstructed 64-bit largest
    // against this end's own highest sent session sequence
    // (proofs/tla/AckBlockWrap.tla ReadBlock): a malformed block refuses the whole
    // packet, and the type data behind it is never examined

    struct rocketnet_ack_block_t block;
    int block_bytes = rocketnet_ack_block_read( &block, opened, opened_bytes, rocketnet_server_protocol_highest_sent_session( server, client_index, mapping ) );
    if ( block_bytes == 0 )
    {
        rocketnet_spam( "server ignored sealed packet. bad ack block" );
        counters[type_counters->bad_ack_block]++;
        return ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the opened size, less the block, must match the type

    const uint8_t * type_data = opened + block_bytes;
    int type_data_bytes = opened_bytes - block_bytes;

    if ( !rocketnet_session_type_data_fits( packet_type, type_data_bytes ) )
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
    // terminate. a payload (type 5) and a fragment (type 9) become eligible
    // later: the payload when it is accepted, the fragment when reassembly
    // accepts it (below).

    if ( rocketnet_session_ack_eligible_on_open( packet_type ) )
        rocketnet_replay_advance( &mapping->accepted_session, sequence );

    mapping->control->last_receive_time = current_time;

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
        // P9b: each acked id is tagged with the connection it was received
        // for, beside its client index, as it is appended

        const uint64_t connection_id = rocketnet_server_protocol_connection_id( server, client_index );

        for ( int i = before; i < server->acks->num_acked; i++ )
        {
            server->acks->acked_client_index[i] = client_index;
            if ( server->acked_connection_ids )
                server->acked_connection_ids[i] = connection_id;
        }

        if ( rocketnet_session_elicits_ack( packet_type ) )
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

        if ( type_data_bytes > out_payload_capacity )
        {
            rocketnet_spam( "server refused payload. %d bytes into a %d-byte buffer", type_data_bytes, out_payload_capacity );
            counters[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL]++;
            return ROCKETNET_SERVER_PROCESS_IGNORED;
        }

        *out_payload = type_data;
        *out_payload_bytes = type_data_bytes;
        counters[ROCKETNET_COUNTER_PAYLOAD_DELIVERED]++;
        *out_payload_client_index = client_index;
        server->payload_sequence = sequence;
        server->payload_connection_id = rocketnet_server_protocol_connection_id( server, client_index );     // P9b
        if ( !server->defer_payload_acks )
            rocketnet_server_protocol_accept_payload( server, client_index, server->payload_connection_id, sequence );
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
            rocketnet_replay_advance( &mapping->accepted_session, sequence );
            if ( server->acks )
                server->acks->endpoints[client_index].ack_debt++;
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
            ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL
        };

        int accepted = rocketnet_reassembly_process_fragment( server->reassembly, client_index,
                                                              type_data, type_data_bytes,
                                                              counters, &fragment_counters );

        // standard/TRANSPORT.md t15.3: a packet is ack-eligible only once
        // its type data passes validation, and a fragment's validation is
        // reassembly's. so the fragment's sequence is marked in
        // accepted_session, and adds to the ack debt, only when reassembly
        // accepts it: a refused fragment (stale, count or size mismatch,
        // payload too large, duplicate index) is never named in a block, so
        // the sender's aggregate never completes on it
        // (proofs/tla/FragmentsAndAcks.tla RecvData, NamedOnlyIfAccepted). an
        // accepted fragment that completes a payload later dropped as batch
        // full still acknowledges: reassembly accepted it.

        if ( accepted )
        {
            rocketnet_replay_advance( &mapping->accepted_session, sequence );
            if ( server->acks )
                server->acks->endpoints[client_index].ack_debt++;
        }

        if ( server->reassembly->completed_bytes > 0 )
        {
            *out_payload_client_index = client_index;
            server->payload_sequence = 0; // fragmented payload has no delivery ACK
            server->payload_connection_id = rocketnet_server_protocol_connection_id( server, client_index );     // P9b
        }

        return accepted ? ROCKETNET_SERVER_PROCESS_ACCEPTED : ROCKETNET_SERVER_PROCESS_IGNORED;
    }

    // the disconnect: release the slot and destroy the mapping immediately,
    // never answered -- the sender's redundant copies are the delivery
    // mechanism

    rocketnet_debug( "client %d disconnected", client_index );
    counters[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED]++;
    counters[ROCKETNET_COUNTER_CLIENT_DISCONNECTED]++;
    rocketnet_server_connection_reset( server, client_index, ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED );

    return ROCKETNET_SERVER_PROCESS_ACCEPTED;
}

int rocketnet_server_protocol_process_received( struct rocketnet_server_protocol_t * server,
                                                const struct rocketnet_address_t * from,
                                                int known_mapping_index,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                uint64_t current_timestamp,
                                                double current_time,
                                                uint8_t * response_packet,
                                                int * response_bytes,
                                                uint8_t * opened,
                                                int out_payload_capacity,
                                                const uint8_t ** out_payload,
                                                int * out_payload_bytes,
                                                int * out_payload_client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( from );
    rocketnet_assert( packet_data );
    rocketnet_assert( response_packet );
    rocketnet_assert( response_bytes );
    rocketnet_assert( opened );
    rocketnet_assert( out_payload );
    rocketnet_assert( out_payload_bytes );
    rocketnet_assert( out_payload_client_index );

    uint64_t * counters = server->counters->values;

    counters[ROCKETNET_COUNTER_PACKETS_RECEIVED]++;

    *response_bytes = 0;
    *out_payload = NULL;
    *out_payload_bytes = 0;
    *out_payload_client_index = -1;
    server->payload_connection_id = 0;

    if ( server->reassembly )
    {
        server->reassembly->completed_payload = NULL;
        server->reassembly->completed_bytes = 0;
    }

    int packet_type = rocketnet_prefilter_check( packet_data, packet_bytes, server->packet_filter );
    if ( packet_type < 0 )
    {
        rocketnet_spam( "server dropped packet. failed prefilter" );
        counters[rocketnet_prefilter_counter( packet_type )]++;
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

    return rocketnet_server_process_sealed_packet( server, from, known_mapping_index, (uint8_t) packet_type, packet_data, packet_bytes,
                                                   current_timestamp, current_time,
                                                   response_packet, response_bytes,
                                                   opened, out_payload_capacity, out_payload, out_payload_bytes, out_payload_client_index );
}

// The copying form, for a caller that owns no open buffer: the packet opens
// on the stack and an accepted payload is copied into out_payload, refused
// whole when it does not fit out_payload_capacity.

int rocketnet_server_protocol_process_packet_capacity( struct rocketnet_server_protocol_t * server,
                                                       const struct rocketnet_address_t * from,
                                                       const uint8_t * packet_data,
                                                       int packet_bytes,
                                                       uint64_t current_timestamp,
                                                       double current_time,
                                                       uint8_t * response_packet,
                                                       int * response_bytes,
                                                       uint8_t * out_payload,
                                                       int out_payload_capacity,
                                                       int * out_payload_bytes,
                                                       int * out_payload_client_index )
{
    rocketnet_assert( out_payload );

    uint8_t opened[ROCKETNET_RECEIVE_CEILING];
    const uint8_t * payload = NULL;
    const int outcome = rocketnet_server_protocol_process_received( server, from, ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP,
                                                                    packet_data, packet_bytes, current_timestamp, current_time,
                                                                    response_packet, response_bytes,
                                                                    opened, out_payload_capacity, &payload,
                                                                    out_payload_bytes, out_payload_client_index );
    if ( payload && *out_payload_bytes > 0 )
    {
        memcpy( out_payload, payload, (size_t) *out_payload_bytes );
#if ROCKETNET_DEV
        server->test_payload_writes++;
#endif // #if ROCKETNET_DEV
    }

    return outcome;
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
    return rocketnet_server_protocol_process_packet_capacity( server, from, packet_data, packet_bytes,
                                                              current_timestamp, current_time,
                                                              response_packet, response_bytes,
                                                              out_payload, ROCKETNET_RECEIVE_PAYLOAD_BYTES,
                                                              out_payload_bytes, out_payload_client_index );
}

// Cookie key rotation: modelled by Tick, LiveGens, PreviousBoundedToOnePeriod
// and InFlightCookieSurvivesRotation in proofs/tla/RNSecurityTables.tla.

void rocketnet_server_protocol_rotate_cookie_key_if_due( struct rocketnet_server_protocol_t * server,
                                                         double current_time )
{
    rocketnet_assert( server );

    if ( server->cookie_rotation_seconds <= 0 )
        return;

    if ( current_time < server->cookie_key_rotation_time )
        return;

    const double period = (double) server->cookie_rotation_seconds;
    if ( current_time < server->cookie_key_rotation_time + period )
    {
        memcpy( server->previous_cookie_key, server->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
        server->previous_cookie_key_expire_time = server->cookie_key_rotation_time + period;
        server->has_previous_cookie_key = 1;
        server->cookie_key_rotation_time += period;
    }
    else
    {
        sodium_memzero( server->previous_cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
        server->previous_cookie_key_expire_time = 0.0;
        server->has_previous_cookie_key = 0;
        server->cookie_key_rotation_time = current_time + period;
    }

    randombytes_buf( server->cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    rocketnet_debug( "server rotated connection cookie secret" );
}

// proofs/tla/ServerConnection.tla Tick
// proofs/tla/RNSecurityTables.tla Tick
void rocketnet_server_protocol_update( struct rocketnet_server_protocol_t * server,
                                       double current_time )
{
    rocketnet_assert( server );
    rocketnet_assert( server->encryption_map );
    rocketnet_assert( server->client_slots );
    rocketnet_assert( server->counters );

    rocketnet_server_protocol_rotate_cookie_key_if_due( server, current_time );

    // reset this server's per-update handshake budget for the next update

    server->counters->handshakes_processed_this_update = 0;

    uint64_t * counters = server->counters->values;

    int iterator = -1;
    while ( rocketnet_encryption_map_next( server->encryption_map, &iterator ) )
    {
        const struct rocketnet_encryption_mapping_control_t * control = rocketnet_encryption_map_control( server->encryption_map, iterator );

        if ( !control->established )
        {
            // TRANSPORT.md t12 rule 14: the earlier of the idle and handshake deadlines removes the mapping.
            if ( current_time >= control->handshake_deadline )
            {
                // the claim leaves with its mapping here too: today the two
                // deadlines are equal and the claim has already expired, so
                // this is a no-op, but the claim's lifetime must not depend on it
                rocketnet_debug( "server removed encryption mapping. handshake deadline expired" );
                rocketnet_server_release_mapping_claim( server, iterator );
                rocketnet_encryption_map_remove_current( server->encryption_map, iterator );
                counters[ROCKETNET_COUNTER_MAPPING_HANDSHAKE_DEADLINE_EXPIRED]++;
            }
            else if ( current_time >= control->last_receive_time + ROCKETNET_HALF_OPEN_IDLE_TIMEOUT_SECONDS )
            {
                // proofs/tla/MCReceiveOrder.tla IdleRemove: the mapping leaves alone
                // and releases its token's claim before it is removed
                rocketnet_debug( "server removed encryption mapping. half-open idle timeout" );
                rocketnet_server_release_mapping_claim( server, iterator );
                rocketnet_encryption_map_remove_current( server->encryption_map, iterator );
                counters[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT]++;
            }

            continue;
        }

        if ( control->timeout_seconds > 0 && current_time >= control->last_receive_time + control->timeout_seconds )
        {
            // an established mapping always holds its slot: established is
            // set only beside client_index, and a reset removes the mapping
            const int client_index = control->client_index;
            rocketnet_assert( client_index != -1 );
            rocketnet_debug( "client %d timed out", client_index );

            rocketnet_server_connection_reset( server, client_index, ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
            counters[ROCKETNET_COUNTER_MAPPING_IDLE_TIMEOUT]++;
            counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT]++;
        }
    }
}

void rocketnet_server_protocol_note_missing_mapping( struct rocketnet_server_protocol_t * server )
{
    server->counters->values[ROCKETNET_COUNTER_SLOT_WITHOUT_MAPPING]++;
}

static struct rocketnet_encryption_mapping_t * rocketnet_server_connected_mapping( struct rocketnet_server_protocol_t * server,
                                                                                   int client_index )
{
    if ( !rocketnet_client_slots_connected( server->client_slots, client_index ) )
        return NULL;

    // rocketnet_client_slots_assign accepts -1 as a mapping index, so a connected
    // slot without a mapping is a state the API permits, and an assert is absent from release

    int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );
    if ( mapping_index < 0 )
    {
        // cannot happen in the shipped protocol; counted so a future regression is not silent

        rocketnet_server_protocol_note_missing_mapping( server );
        return NULL;
    }

    return rocketnet_encryption_map_entry( server->encryption_map, mapping_index );
}

// The ack block's reference (tla/AckBlockWrap.tla ReadBlock, highestSent;
// proofs/tla/SessionSendSequence.tla BaselineIsHighestSealed):
// the delivery bookkeeping's high-water mark, folded from the sequence each
// packet actually sealed (rocketnet_ack_endpoint_record_sent_classified),
// and the same mark the block is then applied against. The next sequence
// less one would also count sequences a fill has taken but not sealed, so it
// is not read here. A protocol built without the bookkeeping has nothing to
// apply a block to and falls back to the highest sequence taken.

uint64_t rocketnet_server_protocol_highest_sent_session( struct rocketnet_server_protocol_t * server,
                                                         int client_index,
                                                         struct rocketnet_encryption_mapping_t * mapping )
{
    rocketnet_assert( server );
    rocketnet_assert( mapping );

    if ( server->acks && client_index >= 0 )
        return rocketnet_platform_atomic_load_u64( &server->acks->endpoints[client_index].highest_sequence );

    const uint64_t next = rocketnet_platform_atomic_load_u64( &mapping->send_sequence_session );
    return next > 0 ? next - 1 : 0;
}

// Builds one sealed session packet for a mapping: the sealed plaintext
// opens with the ack block built from this connection's receive state, and
// the per-packet type data follows it. Every session send passes through
// here or through the payload's in-place twin, so sending any packet sends
// a block, and sending a block resets the ack debt. The sent packet is
// recorded in the delivery bookkeeping -- these sequences carry no packet
// id and surface nothing when acknowledged.
// proofs/tla/RNChannel.tla SendAck and proofs/tla/RNWindow.tla SendAck: the block carries
// the packet ids opened in the fixed batch and the debt clears once it leaves.

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

    int block_bytes = rocketnet_session_ack_block_build( plaintext, type_data_bytes, &mapping->accepted_session );
    if ( type_data_bytes > 0 )
        memcpy( plaintext + block_bytes, type_data, (size_t) type_data_bytes );

    // proofs/tla/SessionSendSequence.tla ControlTake and ControlSeal: the
    // sequence is taken in one fetch-add and is the one sealed below.
    const uint64_t sequence = rocketnet_session_sequence_take( &mapping->send_sequence_session );
#if ROCKETNET_DEV
    rocketnet_dev_session_sequence_taken( sequence );
#endif // #if ROCKETNET_DEV

    int bytes = rocketnet_write_sealed_packet( out_packet, packet_type,
                                               server->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + type_data_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );


    // the sequence this packet actually carried, reported to the caller
    // rather than re-derived from the counter, which a fill may have moved

    if ( out_sequence )
        *out_sequence = sequence;

    if ( server->acks && client_index >= 0 )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, bytes, 0, 0, 0, current_time ),
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );
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

// proofs/tla/RNEndpoint.tla WriteAck: sends a packet that carries only the owed acks, and only when that debt is nonzero.
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

    if ( server->acks->endpoints[client_index].ack_debt == 0 )
        return 0;

    int bytes = rocketnet_server_write_session_packet( server, mapping, client_index,
                                                       ROCKETNET_CONNECTION_ACK_PACKET,
                                                       NULL, 0, current_time, out_packet, NULL );

    rocketnet_spam( "server sent standalone ack to client %d", client_index );

    return bytes;
}

static int rocketnet_server_keep_alive_due( const struct rocketnet_server_protocol_t * server,
                                            const struct rocketnet_encryption_mapping_t * mapping,
                                            int client_index, double current_time )
{
    const double send_rate = server->send_rate > 0.0 ? server->send_rate : 10.0;
    const int acknowledged = server->acks && client_index >= 0
                                 ? (int) server->acks->endpoints[client_index].keep_alive_acked
                                 : 0;
    const double last = acknowledged ? mapping->last_send_time : mapping->last_keep_alive_send_time;
    return !( current_time - last < 1.0 / send_rate );
}

// proofs/tla/ServerConnection.tla ServerKeepAlive
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

    // Without ack bookkeeping the helper uses the unsuppressed clock.

    if ( !rocketnet_server_keep_alive_due( server, mapping, client_index, current_time ) )
        return 0;

    return rocketnet_server_write_keep_alive( server, client_index, current_time, out_packet );
}

int rocketnet_server_protocol_maintenance_due( const struct rocketnet_server_protocol_t * server,
                                               int client_index, double current_time )
{
    rocketnet_assert( server );

    if ( !rocketnet_client_slots_connected( server->client_slots, client_index ) )
        return 0;

    const int mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );

    // Let the transport record a connected slot missing its mapping.
    // The preflight itself must not change counters or connection state.

    if ( mapping_index < 0 )
        return 1;

    const struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, mapping_index );
    return rocketnet_server_keep_alive_due( server, mapping, client_index, current_time ) ||
           ( server->acks && server->acks->endpoints[client_index].ack_debt != 0 );
}

// The fill-phase pair behind the acquire/commit send surface. Any number of
// game threads run these between update and the flush; the phase contract is
// what makes them safe -- mappings are created and torn down only by update,
// which never runs during a fill, so the lookup is over stable state and the
// only shared words touched are the sequence (an atomic add) and the send
// timestamp (an atomic store).

// proofs/tla/SessionSendSequence.tla FillStart: the sequence is one atomic add.
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

// proofs/tla/SessionSendSequence.tla FillSeal: seals the held sequence and
// records it sent, which folds the ack baseline.
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

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return 0;

    // the application's bytes come first: the block truncates to the
    // budget's remaining room, oldest entries dropped first. the 7-byte
    // minimum always fits.

    uint8_t ack_block[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    int ack_block_bytes = rocketnet_session_ack_block_build( ack_block, payload_bytes, &mapping->accepted_session );

    int wire_bytes = rocketnet_write_sealed_payload_in_place( wire_start, server->packet_filter, sequence,
                                                              ack_block, ack_block_bytes,
                                                              payload_bytes, server->protocol_id,
                                                              mapping->connection_salt,
                                                              mapping->keys.session_server_to_client );
    if ( wire_bytes == 0 )
        return 0;

    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, wire_bytes, packet_id, 1, 0, current_time ),
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );
        rocketnet_platform_atomic_store_u64( &endpoint->ack_debt, 0 );      // fill-phase threads reset concurrently
    }

    rocketnet_platform_atomic_store_double( &mapping->last_send_time, current_time );

    return wire_bytes;
}

#if ROCKETNET_DEV
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

    if ( payload_bytes < 1 || payload_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return 0;

    struct rocketnet_encryption_mapping_t * mapping = rocketnet_server_connected_mapping( server, client_index );
    if ( !mapping )
        return 0;

    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    int block_bytes = rocketnet_session_ack_block_build( plaintext, payload_bytes, &mapping->accepted_session );
    memcpy( plaintext + block_bytes, payload_data, (size_t) payload_bytes );

    // proofs/tla/SessionSendSequence.tla ControlTake and ControlSeal: the
    // sequence is taken in one fetch-add and is the one sealed below.
    const uint64_t sequence = rocketnet_session_sequence_take( &mapping->send_sequence_session );
#if ROCKETNET_DEV
    rocketnet_dev_session_sequence_taken( sequence );
#endif // #if ROCKETNET_DEV

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET,
                                               server->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + payload_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;


    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, bytes, 0, 0, 0, current_time ),
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );
        endpoint->ack_debt = 0;
    }

    rocketnet_spam( "server sent payload to client %d (%d bytes)", client_index, payload_bytes );

    mapping->last_send_time = current_time;

    return bytes;
}
#endif // #if ROCKETNET_DEV

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

// proofs/tla/SessionSendSequence.tla FillStart: the block is one atomic add.
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

    int block_bytes = rocketnet_session_ack_block_build( plaintext, ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes, &mapping->accepted_session );

    rocketnet_write_fragment_header( plaintext + block_bytes, payload_id, fragment_index, fragment_count );
    memcpy( plaintext + block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES, fragment_data, (size_t) fragment_bytes );

    int bytes = rocketnet_write_sealed_packet( out_packet, ROCKETNET_CONNECTION_FRAGMENT_PACKET,
                                               server->packet_filter, sequence,
                                               NULL, plaintext, block_bytes + ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                                               server->protocol_id, mapping->connection_salt,
                                               NULL, mapping->keys.session_server_to_client );
    if ( bytes == 0 )
        return 0;

    rocketnet_assert( bytes <= ROCKETNET_SEND_BUDGET );

    if ( server->acks )
    {
        struct rocketnet_ack_endpoint_t * endpoint = &server->acks->endpoints[client_index];
        rocketnet_ack_count_sent_result( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, bytes, 0, 0, aggregate_id, current_time ),
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_LOST],
                                         &server->counters->values[ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW] );
        rocketnet_platform_atomic_store_u64( &endpoint->ack_debt, 0 );      // fill-phase threads reset concurrently
    }

    rocketnet_platform_atomic_store_double( &mapping->last_send_time, current_time );

    return bytes;
}

#if ROCKETNET_DEV
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
#endif // #if ROCKETNET_DEV

// proofs/tla/ServerConnection.tla ServerAppDisconnect
// proofs/tla/ServerConnection.tla ServerDiscCopy: each of the burst's
// ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS copies is written here.
// proofs/tla/RNSecurityTables.tla Disconnect: the slot is released with its mapping
// (rocketnet_server_connection_reset).
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

    rocketnet_server_connection_reset( server, client_index, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
}

void rocketnet_server_protocol_accept_payload( struct rocketnet_server_protocol_t * server, int client_index, uint64_t connection_id, uint64_t sequence )
{
    if ( !sequence || !rocketnet_client_slots_connected( server->client_slots, client_index ) ||
         rocketnet_server_protocol_connection_id( server, client_index ) != connection_id )
        return;
    const int index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );

    // a connected slot may hold mapping index -1 (see rocketnet_server_connected_mapping)

    if ( index < 0 )
        return;
    rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->encryption_map, index );
    if ( rocketnet_replay_already_received( &mapping->accepted_session, sequence ) )
        return;
    rocketnet_replay_advance( &mapping->accepted_session, sequence );
    if ( server->acks )
        server->acks->endpoints[client_index].ack_debt++;
}
