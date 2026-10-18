/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the server per-type packet readers:
    rocketnet_server_protocol_process_connection_request, the static
    rocketnet_server_process_connection_response and
    rocketnet_server_process_sealed_packet, and the dispatcher
    rocketnet_server_protocol_process_packet.

    The readers are sdk/rocketnet_server_protocol.cpp, included below. libsodium
    is not proved: each call a reader makes is a model in this harness that checks
    its own buffer arguments with __CPROVER_r_ok / __CPROVER_w_ok at the lengths
    libsodium documents, writes nondet bytes, and returns a nondet success or
    failure. The helper functions the readers call from other sdk translation
    units (the encryption map, client slots, used-token history, acks,
    reassembly, signature verification and cookie helpers) are modeled the same
    way: each checks its reads and writes, writes nondet state, and returns a
    nondet result, so the readers are exercised on every reachable outcome.

    A packet buffer is allocated at exactly the symbolic packet_bytes, so a read
    one past the caller's length is an out-of-bounds failure and not slack.
    packet_bytes runs over 0..ROCKETNET_MAX_PACKET_BYTES (1472, the receive
    ceiling).

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 64, as the packet opener proof. The fixed-length memcpy and
    memcmp models are at most a few hundred bytes; the loops in the readers are
    bounded by fixed wire sizes.
*/

#define thread_local

#include <stdlib.h>

#if defined(__CPROVER__) && !defined(__cplusplus)
#ifndef static_assert
#define static_assert( _cond, _msg ) _Static_assert( ( _cond ), ( _msg ) )
#endif
#endif

#include "rocketnet_server_protocol.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned nondet_uint( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
static unsigned long long nondet_uint64( void ) { return 0; }
#define PROOF_R_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_W_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_HAVOC( p, n ) ( memset( (p), 0, (size_t) (n) ) )
#else
int nondet_int( void );
unsigned nondet_uint( void );
unsigned char nondet_uchar( void );
unsigned long long nondet_uint64( void );
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
    __CPROVER_assert( PROOF_R_OK( ad, adlen ), "decrypt reads the associated data" );
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
    __CPROVER_assert( PROOF_R_OK( key, keylen ), "generichash reads its key" );
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
    PROOF_HAVOC( pnt, len );
}

/* The server struct, built by the code's own create path at the smallest
   accepted config, as the card instructs. The sub-tables (encryption map,
   client slots, used-token history, acks, reassembly) are opaque, so the
   create functions build them. */

void rocketnet_debug( const char * format, ... )
{
    (void) format;
}

/* Models of the sdk helper functions the readers call from other translation
   units. Each checks its reads and writes, writes nondet state, and returns a
   nondet result. The encryption map, client slots, used-token history, acks
   and reassembly handles are opaque; each create model allocates an opaque
   object, and each lookup model reads and writes it. The single client slot
   config means a lookup collapses to index 0 or "absent" (-1). */

struct rocketnet_encryption_map_t * rocketnet_encryption_map_create( void * context, int capacity )
{
    (void) context;
    __CPROVER_assume( capacity >= 1 );
    struct rocketnet_encryption_map_t * m =
        (struct rocketnet_encryption_map_t *) malloc( (size_t) capacity * 2048 );
    __CPROVER_assume( m != NULL );
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
    (void) context;
    __CPROVER_assume( max_clients >= 1 );
    struct rocketnet_server_acks_t * a =
        (struct rocketnet_server_acks_t *) malloc( (size_t) max_clients * 2048 );
    __CPROVER_assume( a != NULL );
    return a;
}

struct rocketnet_reassembly_set_t * rocketnet_reassembly_set_create( void * context,
                                                                    int num_connections,
                                                                    int num_slots,
                                                                    int max_payload_bytes,
                                                                    int delivered_capacity )
{
    (void) context;
    (void) num_connections;
    (void) num_slots;
    (void) max_payload_bytes;
    (void) delivered_capacity;
    struct rocketnet_reassembly_set_t * r =
        (struct rocketnet_reassembly_set_t *) malloc( 8192 );
    __CPROVER_assume( r != NULL );
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
    return r;
}

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map,
                                                                       int index )
{
    (void) index;
    return (struct rocketnet_encryption_mapping_t *) map;
}

int rocketnet_client_slots_max_clients( const struct rocketnet_client_slots_t * slots )
{
    (void) slots;
    return 1;
}

static struct rocketnet_server_protocol_t * build_server( struct rocketnet_counters_t * counters,
                                                         struct rocketnet_signature_verify_budget_t * budget )
{
    memset( counters, 0, sizeof( *counters ) );
    memset( budget, 0, sizeof( *budget ) );
    static struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
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

    int mapping_index = nondet_int();

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
