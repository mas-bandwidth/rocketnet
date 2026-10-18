/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    ESBMC harnesses for the server per-type packet readers. The real
    sdk/rocketnet_server_protocol.cpp is included below. Libsodium calls
    are modeled; this file does not prove libsodium or external SDK helpers.
    A result is a proof only after every reachable helper has a sound model
    and the stated packet and loop bounds finish verification.
*/

#define thread_local

#include <stdlib.h>

#if defined(__CPROVER__) && !defined(__cplusplus)
#ifndef static_assert
#define static_assert( _cond, _msg ) _Static_assert( ( _cond ), ( _msg ) )
#endif
#endif

#include "../esbmc_shim.h"
#include "rocketnet_server_protocol.cpp"
#include "rocketnet_protocol.cpp"
#include "rocketnet_connect_token.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned nondet_uint( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
static uint64_t nondet_uint64( void ) { return 0; }
#define PROOF_R_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_W_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_HAVOC( p, n ) ( memset( (p), 0, (size_t) (n) ) )
#else
int nondet_int( void );
unsigned nondet_uint( void );
unsigned char nondet_uchar( void );
uint64_t nondet_uint64( void );
#define PROOF_R_OK( p, n ) __CPROVER_r_ok( (p), (size_t) (n) )
#define PROOF_W_OK( p, n ) __CPROVER_w_ok( (p), (size_t) (n) )
#define PROOF_HAVOC( p, n ) __CPROVER_havoc_slice( (p), (size_t) (n) )
#endif

/* libsodium models. */

int crypto_box_keypair( unsigned char * pk, unsigned char * sk )
{
    __CPROVER_assert( PROOF_W_OK( pk, 32 ), "keypair writes the public key" );
    __CPROVER_assert( PROOF_W_OK( sk, 32 ), "keypair writes the secret key" );
    PROOF_HAVOC( pk, 32 );
    PROOF_HAVOC( sk, 32 );
    return 0;
}

int crypto_box_seal_open( unsigned char * m,
                          const unsigned char * c,
                          unsigned long long clen,
                          const unsigned char * pk,
                          const unsigned char * sk )
{
    __CPROVER_assert( clen >= 48, "sealed box input holds the overhead" );
    __CPROVER_assert( PROOF_R_OK( c, clen ), "seal_open reads the sealed box" );
    __CPROVER_assert( PROOF_R_OK( pk, 32 ), "seal_open reads the public key" );
    __CPROVER_assert( PROOF_R_OK( sk, 32 ), "seal_open reads the secret key" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    if ( ok != 0 )
        return -1;

    const unsigned long long plaintext = clen - 48;
    __CPROVER_assert( PROOF_W_OK( m, plaintext ), "seal_open writes the plaintext inside the output" );
    PROOF_HAVOC( m, plaintext );
    return 0;
}

int crypto_sign_verify_detached( const unsigned char * sig,
                                 const unsigned char * m,
                                 unsigned long long mlen,
                                 const unsigned char * pk )
{
    __CPROVER_assert( PROOF_R_OK( sig, 64 ), "sign_verify reads the signature" );
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "sign_verify reads the message" );
    __CPROVER_assert( PROOF_R_OK( pk, 32 ), "sign_verify reads the public key" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    return ok;
}

int crypto_aead_chacha20poly1305_ietf_decrypt( unsigned char * m,
                                               unsigned long long * mlen_p,
                                               unsigned char * nsec,
                                               const unsigned char * c,
                                               unsigned long long clen,
                                               const unsigned char * ad,
                                               unsigned long long adlen,
                                               const unsigned char * npub,
                                               const unsigned char * k )
{
    (void) nsec;
    __CPROVER_assert( clen >= 16, "decrypt input holds the 16-byte tag" );
    __CPROVER_assert( PROOF_R_OK( c, clen ), "decrypt reads the ciphertext and tag" );
    __CPROVER_assert( ( adlen == 0 || PROOF_R_OK( ad, adlen ) ), "decrypt reads the associated data" );
    __CPROVER_assert( PROOF_R_OK( npub, 12 ), "decrypt reads the 12-byte nonce" );
    __CPROVER_assert( PROOF_R_OK( k, 32 ), "decrypt reads the 32-byte key" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    if ( ok != 0 )
        return -1;

    const unsigned long long plaintext = clen - 16;
    __CPROVER_assert( PROOF_W_OK( m, plaintext ), "decrypt writes the plaintext inside the output" );
    __CPROVER_assert( PROOF_W_OK( mlen_p, 8 ), "decrypt writes the plaintext length" );
    PROOF_HAVOC( m, plaintext );
    *mlen_p = plaintext;
    return 0;
}

int crypto_shorthash_siphash24( unsigned char * out,
                                const unsigned char * in,
                                unsigned long long inlen,
                                const unsigned char * k )
{
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "siphash reads its input" );
    __CPROVER_assert( PROOF_R_OK( k, 16 ), "siphash reads its 16-byte key" );
    __CPROVER_assert( PROOF_W_OK( out, 8 ), "siphash writes 8 bytes" );
    PROOF_HAVOC( out, 8 );
    return 0;
}

int crypto_generichash( unsigned char * out,
                        size_t outlen,
                        const unsigned char * in,
                        unsigned long long inlen,
                        const unsigned char * key,
                        size_t keylen )
{
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "generichash reads its input" );
    __CPROVER_assert( ( keylen == 0 || PROOF_R_OK( key, keylen ) ), "generichash reads its key" );
    __CPROVER_assert( PROOF_W_OK( out, outlen ), "generichash writes its output" );
    PROOF_HAVOC( out, outlen );
    return 0;
}

int sodium_memcmp( const void * b1, const void * b2, size_t len )
{
    __CPROVER_assert( PROOF_R_OK( b1, len ), "memcmp reads its first buffer" );
    __CPROVER_assert( PROOF_R_OK( b2, len ), "memcmp reads its second buffer" );
    int r = nondet_int();
    __CPROVER_assume( r >= -127 && r <= 127 );
    return r;
}

void sodium_memzero( void * const pnt, const size_t len )
{
    __CPROVER_assert( PROOF_W_OK( pnt, len ), "memzero writes its buffer" );
    memset( pnt, 0, len );
}

int crypto_aead_chacha20poly1305_ietf_encrypt( unsigned char * c,
                                               unsigned long long * clen_p,
                                               const unsigned char * m,
                                               unsigned long long mlen,
                                               const unsigned char * ad,
                                               unsigned long long adlen,
                                               const unsigned char * nsec,
                                               const unsigned char * npub,
                                               const unsigned char * k )
{
    (void) nsec;
    __CPROVER_assert( PROOF_R_OK( m, (size_t) mlen ), "encrypt reads plaintext" );
    __CPROVER_assert( ( adlen == 0 || PROOF_R_OK( ad, (size_t) adlen ) ), "encrypt reads associated data" );
    __CPROVER_assert( PROOF_R_OK( npub, 12 ), "encrypt reads nonce" );
    __CPROVER_assert( PROOF_R_OK( k, 32 ), "encrypt reads key" );
    __CPROVER_assert( PROOF_W_OK( c, (size_t) mlen + 16 ), "encrypt writes ciphertext and tag" );
    __CPROVER_assert( PROOF_W_OK( clen_p, sizeof( *clen_p ) ), "encrypt writes length" );
    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    if ( ok == 0 )
    {
        PROOF_HAVOC( c, (size_t) mlen + 16 );
        *clen_p = mlen + 16;
    }
    return ok;
}

bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b )
{
    __CPROVER_assert( PROOF_R_OK( a, sizeof( *a ) ), "equal reads first address" );
    __CPROVER_assert( PROOF_R_OK( b, sizeof( *b ) ), "equal reads second address" );
    int result = nondet_int();
    __CPROVER_assume( result == 0 || result == 1 );
    return result != 0;
}

/* The harness assembles one server slot from modeled helper create calls.
   The transport create implementation and its allocator are outside this
   translation unit. */

void rocketnet_debug( const char * format, ... )
{
    (void) format;
}

/* Models for some helpers from other SDK translation units. The remaining
   helper contracts and their proof limits are recorded in results.md. */

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity )
{
    (void) context;
    __CPROVER_assume( capacity >= 1 );
    struct rocketnet_encryption_map_t * m =
        (struct rocketnet_encryption_map_t *) malloc( sizeof( struct rocketnet_encryption_mapping_t ) );
    __CPROVER_assume( m != NULL );
    struct rocketnet_encryption_mapping_t * mapping = (struct rocketnet_encryption_mapping_t *) m;
    *mapping = {};
    mapping->client_index = -1;
    mapping->send_sequence_handshake = 1;
    mapping->send_sequence_session = 1;
    return m;
}

struct rocketnet_used_token_history_t * rocketnet_used_token_history_create( void * context, int capacity )
{
    (void) context;
    __CPROVER_assume( capacity >= 1 );
    struct rocketnet_used_token_history_t * h =
        (struct rocketnet_used_token_history_t *) malloc( (size_t) capacity * 512 );
    __CPROVER_assume( h != NULL );
    return h;
}

struct rocketnet_client_slots_t * rocketnet_client_slots_create( void * context, int max_clients )
{
    (void) context;
    __CPROVER_assume( max_clients >= 1 );
    struct rocketnet_client_slots_t * s =
        (struct rocketnet_client_slots_t *) malloc( (size_t) max_clients * 512 );
    __CPROVER_assume( s != NULL );
    return s;
}

struct rocketnet_server_acks_t * rocketnet_server_acks_create( void * context, int max_clients )
{
    __CPROVER_assume( max_clients == 1 );
    struct rocketnet_server_acks_t * a =
        (struct rocketnet_server_acks_t *) malloc( sizeof( *a ) );
    __CPROVER_assume( a != NULL );
    *a = {};
    a->context = context;
    a->max_clients = 1;
    a->acked_capacity = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    a->acked_client_index = (int *) malloc( (size_t) a->acked_capacity * sizeof( int ) );
    a->acked_packet_ids = (uint64_t *) malloc( (size_t) a->acked_capacity * sizeof( uint64_t ) );
    a->endpoints = (struct rocketnet_ack_endpoint_t *) malloc( sizeof( *a->endpoints ) );
    __CPROVER_assume( a->acked_client_index && a->acked_packet_ids && a->endpoints );
    a->endpoints[0].ack_debt = 0;
    return a;
}

struct rocketnet_reassembly_set_t * rocketnet_reassembly_set_create( void * context,
                                                                    int num_connections,
                                                                    int num_slots,
                                                                    int max_payload_bytes,
                                                                    int delivered_capacity )
{
    __CPROVER_assume( num_connections == 1 && num_slots == 1 );
    struct rocketnet_reassembly_set_t * r =
        (struct rocketnet_reassembly_set_t *) malloc( sizeof( *r ) );
    __CPROVER_assume( r != NULL );
    *r = {};
    r->context = context;
    r->num_connections = num_connections;
    r->num_slots = num_slots;
    r->max_payload_bytes = max_payload_bytes;
    r->delivered_capacity = delivered_capacity;
    return r;
}

int rocketnet_encryption_map_find( const struct rocketnet_encryption_map_t * map,
                                   const struct rocketnet_address_t * address )
{
    (void) map;
    __CPROVER_assert( PROOF_R_OK( address, sizeof( struct rocketnet_address_t ) ),
        "find reads the address" );
    int r = nondet_int();
    __CPROVER_assume( r == -1 || r == 0 );
    if ( r == 0 ) {
        struct rocketnet_encryption_mapping_t * mapping = (struct rocketnet_encryption_mapping_t *) map;
        int client_index = nondet_int();
        __CPROVER_assume( client_index == -1 || client_index == 0 );
        mapping->client_index = client_index;
    }
    return r;
}

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map,
                                                                       int index )
{
    __CPROVER_assert( index == 0, "one-entry map index" );
    __CPROVER_assert( PROOF_R_OK( map, sizeof( struct rocketnet_encryption_mapping_t ) ), "entry reads mapping" );
    return (struct rocketnet_encryption_mapping_t *) map;
}

int rocketnet_client_slots_max_clients( const struct rocketnet_client_slots_t * slots )
{
    (void) slots;
    return 1;
}

void rocketnet_spam( const char * format, ... ) { (void) format; }

int rocketnet_client_slots_find( const struct rocketnet_client_slots_t * slots, uint64_t client_id )
{
    (void) client_id;
    __CPROVER_assert( PROOF_R_OK( slots, 1 ), "find reads slots" );
    int r = nondet_int();
    __CPROVER_assume( r == -1 || r == 0 );
    return r;
}

int rocketnet_client_slots_num_free( const struct rocketnet_client_slots_t * slots )
{
    __CPROVER_assert( PROOF_R_OK( slots, 1 ), "free count reads slots" );
    int r = nondet_int();
    __CPROVER_assume( r == 0 || r == 1 );
    return r;
}

int rocketnet_client_slots_assign( struct rocketnet_client_slots_t * slots,
                                   uint64_t client_id, int mapping_index,
                                   const uint8_t * user_data )
{
    (void) client_id;
    __CPROVER_assert( PROOF_W_OK( slots, 1 ), "assign writes slots" );
    __CPROVER_assert( mapping_index == 0, "assign uses live mapping" );
    __CPROVER_assert( PROOF_R_OK( user_data, ROCKETNET_USER_DATA_BYTES ), "assign reads user data" );
    return 0;
}

int rocketnet_client_slots_connected( const struct rocketnet_client_slots_t * slots, int client_index )
{
    __CPROVER_assert( PROOF_R_OK( slots, 1 ), "connected reads slots" );
    if ( client_index != 0 ) return 0;
    int r = nondet_int();
    __CPROVER_assume( r == 0 || r == 1 );
    return r;
}

int rocketnet_client_slots_mapping_index( const struct rocketnet_client_slots_t * slots, int client_index )
{
    __CPROVER_assert( PROOF_R_OK( slots, 1 ), "mapping index reads slots" );
    __CPROVER_assert( client_index == 0, "connected slot index" );
    return 0;
}

int rocketnet_client_slots_confirmed( const struct rocketnet_client_slots_t * slots, int client_index )
{
    __CPROVER_assert( PROOF_R_OK( slots, 1 ), "confirmed reads slots" );
    __CPROVER_assert( client_index == 0, "confirmed slot index" );
    int r = nondet_int();
    __CPROVER_assume( r == 0 || r == 1 );
    return r;
}

void rocketnet_client_slots_confirm( struct rocketnet_client_slots_t * slots, int client_index )
{
    __CPROVER_assert( PROOF_W_OK( slots, 1 ), "confirm writes slots" );
    __CPROVER_assert( client_index == 0, "confirm slot index" );
}

void rocketnet_client_slots_release( struct rocketnet_client_slots_t * slots, int client_index )
{
    __CPROVER_assert( PROOF_W_OK( slots, 1 ), "release writes slots" );
    __CPROVER_assert( client_index == 0, "release slot index" );
}

void rocketnet_used_token_history_record( struct rocketnet_used_token_history_t * history,
                                          const uint8_t * token_identifier,
                                          const struct rocketnet_address_t * address,
                                          uint64_t expire_timestamp, uint64_t current_timestamp )
{
    (void) expire_timestamp; (void) current_timestamp;
    __CPROVER_assert( PROOF_W_OK( history, 1 ), "record writes history" );
    __CPROVER_assert( PROOF_R_OK( token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ), "record reads identifier" );
    __CPROVER_assert( PROOF_R_OK( address, sizeof( *address ) ), "record reads address" );
}

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index )
{
    __CPROVER_assert( PROOF_R_OK( map, sizeof( struct rocketnet_encryption_mapping_t ) ), "address reads map" );
    __CPROVER_assert( index == 0, "address uses live mapping" );
    static struct rocketnet_address_t address;
    return &address;
}

void rocketnet_server_acks_reset_slot( struct rocketnet_server_acks_t * acks, int client_index )
{
    __CPROVER_assert( PROOF_W_OK( acks, sizeof( *acks ) ), "reset writes acks" );
    __CPROVER_assert( client_index == 0, "reset slot index" );
}

void rocketnet_reassembly_set_reset_connection( struct rocketnet_reassembly_set_t * set, int connection_index )
{
    __CPROVER_assert( PROOF_W_OK( set, sizeof( *set ) ), "reset writes reassembly" );
    __CPROVER_assert( connection_index == 0, "reset connection index" );
}

void rocketnet_ack_endpoint_mark_keep_alive( struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence )
{
    (void) sequence;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof( *endpoint ) ), "keep alive marks endpoint" );
}

int rocketnet_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay )
{
    __CPROVER_assert( capacity >= 7, "ack block minimum capacity" );
    __CPROVER_assert( PROOF_R_OK( replay, sizeof( *replay ) ), "ack block reads replay" );
    int n = replay->highest_opened == 0 ? ROCKETNET_ACK_BLOCK_MIN_BYTES : nondet_int();
    __CPROVER_assume( n >= ROCKETNET_ACK_BLOCK_MIN_BYTES && n <= capacity );
    __CPROVER_assert( PROOF_W_OK( output, (size_t) n ), "ack block writes output" );
    PROOF_HAVOC( output, (size_t) n );
    return n;
}

enum rocketnet_ack_sent_result_t rocketnet_ack_endpoint_record_sent_classified(
    struct rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, int wire_bytes,
    uint64_t packet_id, int has_packet_id, uint64_t aggregate_id, double time )
{
    (void) sequence; (void) wire_bytes; (void) packet_id; (void) has_packet_id; (void) aggregate_id; (void) time;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof( *endpoint ) ), "record sent writes endpoint" );
    int r = nondet_int();
    __CPROVER_assume( r >= ROCKETNET_ACK_SENT_RECORDED && r <= ROCKETNET_ACK_SENT_REFUSED_STALE );
    return (enum rocketnet_ack_sent_result_t) r;
}

void rocketnet_ack_count_sent_result( enum rocketnet_ack_sent_result_t result,
                                     uint64_t * lost_counter, uint64_t * overflow_counter )
{
    (void) result;
    __CPROVER_assert( PROOF_W_OK( lost_counter, sizeof( *lost_counter ) ), "count writes lost" );
    __CPROVER_assert( PROOF_W_OK( overflow_counter, sizeof( *overflow_counter ) ), "count writes overflow" );
}

int rocketnet_used_token_history_check( const struct rocketnet_used_token_history_t * history,
                                        const uint8_t * token_identifier,
                                        const struct rocketnet_address_t * address,
                                        uint64_t current_timestamp )
{
    (void) current_timestamp;
    __CPROVER_assert( PROOF_R_OK( history, 1 ), "history check reads history" );
    __CPROVER_assert( PROOF_R_OK( token_identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES ), "history check reads identifier" );
    __CPROVER_assert( PROOF_R_OK( address, sizeof( *address ) ), "history check reads address" );
    int r = nondet_int();
    __CPROVER_assume( r == 0 || r == 1 );
    return r;
}

void rocketnet_derive_handshake_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce )
{
    __CPROVER_assert( PROOF_W_OK( keys, sizeof( *keys ) ), "derive writes handshake keys" );
    __CPROVER_assert( PROOF_R_OK( token_client_to_server_key, ROCKETNET_KEY_BYTES ), "derive reads first token key" );
    __CPROVER_assert( PROOF_R_OK( token_server_to_client_key, ROCKETNET_KEY_BYTES ), "derive reads second token key" );
    __CPROVER_assert( PROOF_R_OK( connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ), "derive reads salt" );
    __CPROVER_assert( PROOF_R_OK( session_nonce, ROCKETNET_SESSION_NONCE_BYTES ), "derive reads nonce" );
    PROOF_HAVOC( keys, sizeof( *keys ) );
}

int rocketnet_derive_connection_keys( struct rocketnet_connection_keys_t * keys,
                                      const uint8_t * token_client_to_server_key,
                                      const uint8_t * token_server_to_client_key,
                                      const uint8_t * connection_salt,
                                      const uint8_t * session_nonce,
                                      const uint8_t * own_ephemeral_secret_key,
                                      const uint8_t * peer_ephemeral_public_key,
                                      const uint8_t * client_ephemeral_public_key,
                                      const uint8_t * server_ephemeral_public_key )
{
    __CPROVER_assert( PROOF_W_OK( keys, sizeof( *keys ) ), "derive writes connection keys" );
    __CPROVER_assert( PROOF_R_OK( token_client_to_server_key, ROCKETNET_KEY_BYTES ), "derive reads first token key" );
    __CPROVER_assert( PROOF_R_OK( token_server_to_client_key, ROCKETNET_KEY_BYTES ), "derive reads second token key" );
    __CPROVER_assert( PROOF_R_OK( connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ), "derive reads salt" );
    __CPROVER_assert( PROOF_R_OK( session_nonce, ROCKETNET_SESSION_NONCE_BYTES ), "derive reads nonce" );
    __CPROVER_assert( PROOF_R_OK( own_ephemeral_secret_key, ROCKETNET_KEY_BYTES ), "derive reads own secret" );
    __CPROVER_assert( PROOF_R_OK( peer_ephemeral_public_key, ROCKETNET_KEY_BYTES ), "derive reads peer public" );
    __CPROVER_assert( PROOF_R_OK( client_ephemeral_public_key, ROCKETNET_KEY_BYTES ), "derive reads client public" );
    __CPROVER_assert( PROOF_R_OK( server_ephemeral_public_key, ROCKETNET_KEY_BYTES ), "derive reads server public" );
    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == 1 );
    if ( ok ) PROOF_HAVOC( keys, sizeof( *keys ) );
    return ok;
}

int rocketnet_encryption_map_add( struct rocketnet_encryption_map_t * map,
                                  const struct rocketnet_address_t * address,
                                  double handshake_deadline )
{
    (void) handshake_deadline;
    __CPROVER_assert( PROOF_W_OK( map, sizeof( struct rocketnet_encryption_mapping_t ) ), "add writes mapping" );
    __CPROVER_assert( PROOF_R_OK( address, sizeof( *address ) ), "add reads address" );
    int index = nondet_int();
    __CPROVER_assume( index == -1 || index == 0 );
    if ( index == 0 )
    {
        struct rocketnet_encryption_mapping_t * mapping = (struct rocketnet_encryption_mapping_t *) map;
        *mapping = {};
        mapping->client_index = -1;
        mapping->send_sequence_handshake = 1;
        mapping->send_sequence_session = 1;
    }
    return index;
}

void rocketnet_encryption_map_remove( struct rocketnet_encryption_map_t * map, int index )
{
    __CPROVER_assert( index == 0, "remove uses live mapping" );
    __CPROVER_assert( PROOF_W_OK( map, sizeof( struct rocketnet_encryption_mapping_t ) ), "remove writes mapping" );
}

void randombytes_buf( void * const buf, const size_t size )
{
    __CPROVER_assert( PROOF_W_OK( buf, size ), "randombytes writes output" );
    PROOF_HAVOC( buf, size );
}

int rocketnet_replay_already_received( const struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    (void) sequence;
    __CPROVER_assert( PROOF_R_OK( replay, sizeof( *replay ) ), "replay test reads window" );
    int r = nondet_int();
    __CPROVER_assume( r == 0 || r == 1 );
    return r;
}

void rocketnet_replay_advance( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    __CPROVER_assert( PROOF_W_OK( replay, sizeof( *replay ) ), "replay advance writes window" );
    if ( sequence > replay->highest_opened ) replay->highest_opened = sequence;
}

int rocketnet_ack_block_read( struct rocketnet_ack_block_t * block,
                              const uint8_t * data, int data_bytes,
                              uint64_t highest_sent )
{
    (void) highest_sent;
    __CPROVER_assert( data_bytes >= 0, "ack input nonnegative" );
    __CPROVER_assert( PROOF_R_OK( data, (size_t) data_bytes ), "ack reads opened data" );
    __CPROVER_assert( PROOF_W_OK( block, sizeof( *block ) ), "ack writes block" );
    int n = nondet_int();
    __CPROVER_assume( n == 0 || ( n >= ROCKETNET_ACK_BLOCK_MIN_BYTES &&
                        n <= data_bytes && n <= ROCKETNET_ACK_BLOCK_MAX_BYTES &&
                        ( n - ROCKETNET_ACK_BLOCK_MIN_BYTES ) % 4 == 0 ) );
    if ( n ) {
        block->num_ranges = ( n - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 4;
        block->largest = nondet_uint64();
        block->first_range_length = nondet_uint();
    }
    return n;
}

void rocketnet_ack_endpoint_record_received( struct rocketnet_ack_endpoint_t * endpoint,
                                             uint64_t sequence, int wire_bytes, double time )
{
    (void) sequence; (void) wire_bytes; (void) time;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof( *endpoint ) ), "received record writes endpoint" );
}

int rocketnet_ack_endpoint_process_block( struct rocketnet_ack_endpoint_t * endpoint,
                                          const struct rocketnet_ack_block_t * block,
                                          double time,
                                          uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                          uint64_t * unknown_sequence_counter,
                                          uint64_t * batch_full_counter )
{
    (void) time;
    __CPROVER_assert( PROOF_W_OK( endpoint, sizeof( *endpoint ) ), "process ack writes endpoint" );
    __CPROVER_assert( PROOF_R_OK( block, sizeof( *block ) ), "process ack reads block" );
    __CPROVER_assert( acked_capacity >= 0, "ack capacity nonnegative" );
    __CPROVER_assert( PROOF_W_OK( acked_packet_ids, (size_t) acked_capacity * sizeof( uint64_t ) ), "process ack writes ids" );
    __CPROVER_assert( PROOF_W_OK( acked_count, sizeof( *acked_count ) ), "process ack writes count" );
    __CPROVER_assert( PROOF_W_OK( unknown_sequence_counter, sizeof( *unknown_sequence_counter ) ), "process ack writes unknown count" );
    __CPROVER_assert( PROOF_W_OK( batch_full_counter, sizeof( *batch_full_counter ) ), "process ack writes batch count" );
    __CPROVER_assert( *acked_count >= 0 && *acked_count <= acked_capacity, "ack count starts inside capacity" );
    int after = nondet_int();
    __CPROVER_assume( after >= *acked_count && after <= acked_capacity );
    *acked_count = after;
    return after;
}

int rocketnet_reassembly_process_fragment( struct rocketnet_reassembly_set_t * set,
                                           int connection_index,
                                           const uint8_t * type_data, int type_data_bytes,
                                           uint64_t * counters,
                                           const struct rocketnet_reassembly_counters_t * fragment_counters )
{
    __CPROVER_assert( PROOF_W_OK( set, sizeof( *set ) ), "fragment writes set" );
    __CPROVER_assert( connection_index == 0, "fragment uses connected slot" );
    __CPROVER_assert( type_data_bytes >= 0, "fragment bytes nonnegative" );
    __CPROVER_assert( PROOF_R_OK( type_data, (size_t) type_data_bytes ), "fragment reads type data" );
    __CPROVER_assert( PROOF_W_OK( counters, sizeof( uint64_t ) * ROCKETNET_NUM_COUNTERS ), "fragment writes counters" );
    __CPROVER_assert( PROOF_R_OK( fragment_counters, sizeof( *fragment_counters ) ), "fragment reads counter indices" );
    int accepted = nondet_int();
    __CPROVER_assume( accepted == 0 || accepted == 1 );
    int complete = nondet_int();
    __CPROVER_assume( complete >= 0 && complete <= set->max_payload_bytes );
    set->completed_bytes = complete;
    return accepted;
}

static struct rocketnet_server_protocol_t * build_server( struct rocketnet_counters_t * counters,
                                                         struct rocketnet_signature_verify_budget_t * budget )
{
    *counters = {};
    *budget = {};
    static struct rocketnet_server_protocol_t server;
    server = {};
    server.protocol_id = 1;
    server.send_rate = 10.0;
    server.cookie_lifetime_seconds = 3;
    server.cookie_rotation_seconds = 60;
    server.cookie_key_rotation_time = 0.0;
    server.previous_cookie_key_expire_time = 0.0;
    server.max_token_lifetime_seconds = 0;
    server.handshake_deadline_seconds = 5.0;
    server.max_timeout_seconds = 0;
    server.allow_negative_timeout = 0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 1 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, 1 );
    server.acks = rocketnet_server_acks_create( NULL, 1 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 1, 1, 16384, 4 );
    server.counters = counters;
    server.signature_verify_budget = *budget;
    server.next_connection_id = 0;
    server.connection_ids = NULL;
    server.acked_connection_ids = NULL;
    server.payload_connection_id = 0;
    return &server;
}

/* The connection request reader: a packet of any length up to the maximum,
   every connect-token byte symbolic. The reader must never read past
   packet_bytes and never write past ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES. */

void harness_connection_request( void )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_MAX_PACKET_BYTES );

    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );

    struct rocketnet_address_t from;
    struct rocketnet_counters_t counters;
    struct rocketnet_signature_verify_budget_t budget;
    struct rocketnet_server_protocol_t * server = build_server( &counters, &budget );

    uint8_t response[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    int response_bytes = 0;

    const int result = rocketnet_server_protocol_process_connection_request( server,
        &from, packet, packet_bytes, 1000ULL, 1000.0, response, &response_bytes );

    __CPROVER_assert( response_bytes >= 0, "response length is not negative" );
    __CPROVER_assert( response_bytes <= ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        "response length fits the response buffer" );
    __CPROVER_assert( result >= ROCKETNET_SERVER_PROCESS_IGNORED
                      && result <= ROCKETNET_SERVER_PROCESS_ACCEPTED,
        "result is a documented server process outcome" );
}

/* The connection response reader (static): the mapping entry at a symbolic
   index, with every response-packet byte written by the reader falling inside
   the response buffer. A connection response carries no packet bytes of its
   own; the mapping is read at mapping_index. */

void harness_connection_response( void )
{
    struct rocketnet_counters_t counters;
    struct rocketnet_signature_verify_budget_t budget;
    struct rocketnet_server_protocol_t * server = build_server( &counters, &budget );

    int mapping_index = 0;  // the caller supplies an index found in the one-entry map

    uint8_t response[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    int response_bytes = 0;

    const int result = rocketnet_server_process_connection_response( server, mapping_index,
        1000ULL, 1000.0, response, &response_bytes );

    __CPROVER_assert( response_bytes >= 0 && response_bytes <= ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        "response fits the response buffer" );
    __CPROVER_assert( result >= ROCKETNET_SERVER_PROCESS_IGNORED
                      && result <= ROCKETNET_SERVER_PROCESS_ACCEPTED,
        "result is a documented server process outcome" );
}

/* The sealed session packet reader (static): every packet type byte, on a
   packet of up to the maximum size, with the payload written inside the
   payload buffer. */

void harness_sealed_packet( void )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_MAX_PACKET_BYTES );

    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );

    struct rocketnet_address_t from;
    struct rocketnet_counters_t counters;
    struct rocketnet_signature_verify_budget_t budget;
    struct rocketnet_server_protocol_t * server = build_server( &counters, &budget );

    uint8_t response[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    int response_bytes = 0;
    uint8_t out_payload[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
    int out_payload_bytes = 0;
    int out_payload_client_index = -1;

    const uint8_t packet_type = nondet_uchar();
    __CPROVER_assume( packet_type == ROCKETNET_CONNECTION_RESPONSE_PACKET ||
                      packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET ||
                      packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET ||
                      packet_type == ROCKETNET_CONNECTION_DISCONNECT_PACKET ||
                      packet_type == ROCKETNET_CONNECTION_ACK_PACKET ||
                      packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET );

    const int result = rocketnet_server_process_sealed_packet( server, &from, packet_type,
        packet, packet_bytes, 1000ULL, 1000.0, response, &response_bytes,
        out_payload, &out_payload_bytes, &out_payload_client_index );

    __CPROVER_assert( response_bytes >= 0 && response_bytes <= ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        "response fits the response buffer" );
    __CPROVER_assert( out_payload_bytes >= 0 && out_payload_bytes <= ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES,
        "payload fits the payload buffer" );
    __CPROVER_assert( result >= ROCKETNET_SERVER_PROCESS_IGNORED
                      && result <= ROCKETNET_SERVER_PROCESS_ACCEPTED,
        "result is a documented server process outcome" );
}

/* The dispatcher: packet_data of packet_bytes symbolic bytes, response_packet
   and out_payload sized as the header documents, with *response_bytes and
   *out_payload_bytes inside their buffers. */

void harness_process_packet( void )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_MAX_PACKET_BYTES );

    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );

    struct rocketnet_address_t from;
    struct rocketnet_counters_t counters;
    struct rocketnet_signature_verify_budget_t budget;
    struct rocketnet_server_protocol_t * server = build_server( &counters, &budget );

    uint8_t response[ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES];
    uint8_t out_payload[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
    int response_bytes = 0;
    int out_payload_bytes = 0;
    int out_payload_client_index = -1;

    const int result = rocketnet_server_protocol_process_packet( server, &from, packet, packet_bytes,
        1000ULL, 1000.0, response, &response_bytes, out_payload, &out_payload_bytes,
        &out_payload_client_index );

    __CPROVER_assert( response_bytes >= 0 && response_bytes <= ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
        "response fits the response buffer" );
    __CPROVER_assert( out_payload_bytes >= 0 && out_payload_bytes <= ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES,
        "payload fits the payload buffer" );
    (void) result;
}

int main( void )
{
    harness_connection_request();
    harness_connection_response();
    harness_sealed_packet();
    harness_process_packet();
    return 0;
}
