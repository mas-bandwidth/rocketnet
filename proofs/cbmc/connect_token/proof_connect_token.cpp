/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    ESBMC nightly proofs of the connect-token readers.

    The reader is sdk/rocketnet_connect_token.cpp, included below; no sdk code
    is copied. The token is exactly ROCKETNET_CONNECT_TOKEN_BYTES symbolic
    bytes, the signed connect data is exactly
    ROCKETNET_SIGNED_CONNECT_DATA_BYTES symbolic bytes, and the private block
    is exactly ROCKETNET_PRIVATE_BLOCK_BYTES symbolic bytes, so a read one
    past any documented block is an out-of-bounds failure and not slack.

    libsodium is not proved. Each call a reader makes is a model in this
    harness that checks its own buffer arguments through the shared ESBMC
    shim at the lengths libsodium documents, writes nondet bytes,
    and returns a nondet success or failure. crypto_box_seal_open checks that
    it reads ROCKETNET_SEALED_BLOCK_BYTES at
    ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK inside the signed data and writes
    ROCKETNET_PRIVATE_BLOCK_BYTES.

    Flags: --std c++17 --z3 --overflow-check, with ESBMC's default bounds,
           pointer and divide-by-zero checks. The proof target bounds each
           solver run to 30 minutes and 8 GiB of address space.
    Unwinding: per harness. The byte-copy library model needs one iteration
    beyond the longest field copy (576, 432, or 256 bytes); the identifier
    hash and private-block output models need 17 and 385 respectively for
    the shared shim's nondeterministic byte-write loops.
*/

#define thread_local

#include <stdlib.h>

#include "../esbmc_shim.h"
#include "rocketnet_connect_token.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static int nondet_int( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
#define PROOF_R_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_W_OK( p, n ) ( (void) ( p ), (void) ( n ), 1 )
#define PROOF_HAVOC( p, n ) ( memset( (p), 0, (size_t) (n) ) )
#else
int nondet_int( void );
unsigned char nondet_uchar( void );
#define PROOF_R_OK( p, n ) __CPROVER_r_ok( (p), (size_t) (n) )
#define PROOF_W_OK( p, n ) __CPROVER_w_ok( (p), (size_t) (n) )
#define PROOF_HAVOC( p, n ) __CPROVER_havoc_slice( (p), (size_t) (n) )
#endif

/* the tally lives in the sdk unit that is compiled in the library; the proof,
   like that unit, holds the one definition */

thread_local struct rocketnet_crypto_tally_t rocketnet_crypto_tally;

/* the signed data the open model is called with, so it can check the exact
   sealed-block offset the reader passes */

static const uint8_t * g_signed_connect_data;

/* libsodium models. */

int crypto_box_seal_open( unsigned char * m,
                          const unsigned char * c,
                          unsigned long long clen,
                          const unsigned char * pk,
                          const unsigned char * sk )
{
    __CPROVER_assert( c == g_signed_connect_data + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK,
                      "seal_open reads the sealed block at its signed-data offset" );
    __CPROVER_assert( clen == ROCKETNET_SEALED_BLOCK_BYTES, "seal_open reads the sealed block length" );
    __CPROVER_assert( PROOF_R_OK( c, clen ), "seal_open reads its sealed block" );
    __CPROVER_assert( PROOF_R_OK( pk, ROCKETNET_KEY_BYTES ), "seal_open reads the server public key" );
    __CPROVER_assert( PROOF_R_OK( sk, ROCKETNET_KEY_BYTES ), "seal_open reads the server secret key" );
    __CPROVER_assert( PROOF_W_OK( m, ROCKETNET_PRIVATE_BLOCK_BYTES ), "seal_open writes the private block" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    if ( ok != 0 )
        return -1;

    PROOF_HAVOC( m, ROCKETNET_PRIVATE_BLOCK_BYTES );
    return 0;
}

int crypto_sign_verify_detached( const unsigned char * sig,
                                 const unsigned char * m,
                                 unsigned long long mlen,
                                 const unsigned char * pk )
{
    __CPROVER_assert( PROOF_R_OK( sig, ROCKETNET_SIGNATURE_BYTES ), "verify reads the signature" );
    __CPROVER_assert( mlen == ROCKETNET_SIGNED_CONNECT_DATA_BYTES, "verify reads the whole signed block" );
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "verify reads the signed block" );
    __CPROVER_assert( PROOF_R_OK( pk, ROCKETNET_KEY_BYTES ), "verify reads the backend public key" );

    int ok = nondet_int();
    __CPROVER_assume( ok == 0 || ok == -1 );
    return ok;
}

int crypto_generichash( unsigned char * out,
                        size_t outlen,
                        const unsigned char * in,
                        unsigned long long inlen,
                        const unsigned char * key,
                        size_t keylen )
{
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "generichash reads its input" );
    if ( keylen > 0 )
        __CPROVER_assert( PROOF_R_OK( key, keylen ), "generichash reads its key" );
    __CPROVER_assert( PROOF_W_OK( out, outlen ), "generichash writes its output" );
    PROOF_HAVOC( out, outlen );
    return 0;
}

/* The writer functions in the included unit reference the remaining libsodium
   calls even though no harness calls a writer. The models keep the unit
   linkable and bound their own buffers like the others. */

int crypto_box_seal( unsigned char * c, const unsigned char * m,
                     unsigned long long mlen, const unsigned char * pk )
{
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "seal reads its plaintext" );
    __CPROVER_assert( PROOF_R_OK( pk, ROCKETNET_KEY_BYTES ), "seal reads the public key" );
    __CPROVER_assert( PROOF_W_OK( c, mlen + ROCKETNET_SEALED_BLOCK_BYTES - ROCKETNET_PRIVATE_BLOCK_BYTES ),
                      "seal writes the sealed plaintext" );
    PROOF_HAVOC( c, mlen + ROCKETNET_SEALED_BLOCK_BYTES - ROCKETNET_PRIVATE_BLOCK_BYTES );
    return 0;
}

int crypto_scalarmult_base( unsigned char * q, const unsigned char * n )
{
    __CPROVER_assert( PROOF_R_OK( n, ROCKETNET_KEY_BYTES ), "scalarmult reads its secret" );
    __CPROVER_assert( PROOF_W_OK( q, ROCKETNET_KEY_BYTES ), "scalarmult writes the public key" );
    PROOF_HAVOC( q, ROCKETNET_KEY_BYTES );
    return 0;
}

int crypto_generichash_init( crypto_generichash_state * state,
                             const unsigned char * key,
                             const size_t keylen,
                             const size_t outlen )
{
    if ( keylen > 0 )
        __CPROVER_assert( PROOF_R_OK( key, keylen ), "generichash_init reads its key" );
    __CPROVER_assert( PROOF_W_OK( state, sizeof( *state ) ), "generichash_init writes its state" );
    PROOF_HAVOC( state, sizeof( *state ) );
    return 0;
}

int crypto_generichash_update( crypto_generichash_state * state,
                               const unsigned char * in,
                               unsigned long long inlen )
{
    __CPROVER_assert( PROOF_R_OK( state, sizeof( *state ) ), "generichash_update reads its state" );
    __CPROVER_assert( PROOF_R_OK( in, inlen ), "generichash_update reads its input" );
    __CPROVER_assert( PROOF_W_OK( state, sizeof( *state ) ), "generichash_update writes its state" );
    PROOF_HAVOC( state, sizeof( *state ) );
    return 0;
}

int crypto_generichash_final( crypto_generichash_state * state,
                              unsigned char * out,
                              const size_t outlen )
{
    __CPROVER_assert( PROOF_R_OK( state, sizeof( *state ) ), "generichash_final reads its state" );
    __CPROVER_assert( PROOF_W_OK( out, outlen ), "generichash_final writes its output" );
    PROOF_HAVOC( out, outlen );
    return 0;
}

int crypto_box_easy( unsigned char * c, const unsigned char * m,
                     unsigned long long mlen, const unsigned char * n,
                     const unsigned char * pk, const unsigned char * sk )
{
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "box_easy reads its plaintext" );
    __CPROVER_assert( PROOF_R_OK( n, 24 ), "box_easy reads its nonce" );
    __CPROVER_assert( PROOF_R_OK( pk, ROCKETNET_KEY_BYTES ), "box_easy reads the public key" );
    __CPROVER_assert( PROOF_R_OK( sk, ROCKETNET_KEY_BYTES ), "box_easy reads the secret key" );
    __CPROVER_assert( PROOF_W_OK( c, mlen + 16 ), "box_easy writes the ciphertext and tag" );
    PROOF_HAVOC( c, mlen + 16 );
    return 0;
}

int crypto_sign_detached( unsigned char * sig, unsigned long long * siglen_p,
                          const unsigned char * m, unsigned long long mlen,
                          const unsigned char * sk )
{
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "sign reads its message" );
    __CPROVER_assert( PROOF_R_OK( sk, ROCKETNET_SIGNATURE_BYTES ), "sign reads its secret key" );
    __CPROVER_assert( PROOF_W_OK( sig, ROCKETNET_SIGNATURE_BYTES ), "sign writes the signature" );
    if ( siglen_p != NULL )
    {
        __CPROVER_assert( PROOF_W_OK( siglen_p, 8 ), "sign writes the signature length" );
        *siglen_p = ROCKETNET_SIGNATURE_BYTES;
    }
    PROOF_HAVOC( sig, ROCKETNET_SIGNATURE_BYTES );
    return 0;
}

void sodium_memzero( void * const pnt, const size_t len )
{
    __CPROVER_assert( PROOF_W_OK( pnt, len ), "memzero writes its buffer" );
    PROOF_HAVOC( pnt, len );
}

/* the client-side split: exactly ROCKETNET_CONNECT_TOKEN_BYTES symbolic bytes
   fill the four fields at their documented offsets. */

void harness_read_connect_token( void )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    struct rocketnet_connect_token_t output;

    rocketnet_read_connect_token( &output, token );

    __CPROVER_assert( output.signed_connect_data[0] == token[0] && output.signed_connect_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES - 1] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES - 1],
                      "the signed connect data is the first 576 token bytes" );
    __CPROVER_assert( output.signature[0] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES] && output.signature[ROCKETNET_SIGNATURE_BYTES - 1] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES - 1],
                      "the signature follows the signed connect data" );
    __CPROVER_assert( output.client_to_server_key[0] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES] && output.client_to_server_key[ROCKETNET_KEY_BYTES - 1] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES + ROCKETNET_KEY_BYTES - 1],
                      "the client-to-server key follows the signature" );
    __CPROVER_assert( output.server_to_client_key[0] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES + ROCKETNET_KEY_BYTES] && output.server_to_client_key[ROCKETNET_KEY_BYTES - 1] == token[ROCKETNET_SIGNED_CONNECT_DATA_BYTES + ROCKETNET_SIGNATURE_BYTES + 2 * ROCKETNET_KEY_BYTES - 1],
                      "the server-to-client key follows the client-to-server key" );
}

/* the signed connect data: every field read at its documented offset, all of
   them inside ROCKETNET_SIGNED_CONNECT_DATA_BYTES. */

void harness_read_signed_connect_data( void )
{
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    struct rocketnet_signed_connect_data_t output;

    rocketnet_read_signed_connect_data( &output, signed_data );

    const uint8_t * p = signed_data + ROCKETNET_SIGNED_OFFSET_VERSION;
    const uint32_t version = rocketnet_read_uint32( &p );
    __CPROVER_assert( output.version == version, "version is read at its offset" );

    p = signed_data + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID;
    const uint64_t protocol_id = rocketnet_read_uint64( &p );
    __CPROVER_assert( output.protocol_id == protocol_id, "protocol id is read at its offset" );

    p = signed_data + ROCKETNET_SIGNED_OFFSET_CREATE;
    const uint64_t create = rocketnet_read_uint64( &p );
    __CPROVER_assert( output.create_timestamp == create, "create timestamp is read at its offset" );

    p = signed_data + ROCKETNET_SIGNED_OFFSET_EXPIRE;
    const uint64_t expire = rocketnet_read_uint64( &p );
    __CPROVER_assert( output.expire_timestamp == expire, "expire timestamp is read at its offset" );

    p = signed_data + ROCKETNET_SIGNED_OFFSET_TIMEOUT;
    const int32_t timeout = (int32_t) rocketnet_read_uint32( &p );
    __CPROVER_assert( output.timeout_seconds == timeout, "timeout is read at its offset" );

    p = signed_data + ROCKETNET_SIGNED_OFFSET_ADDRESS_FLAGS;
    const uint8_t flags = rocketnet_read_uint8( &p );
    __CPROVER_assert( output.address_flags == flags, "address flags are read at their offset" );

    __CPROVER_assert( output.ipv4_address[0] == signed_data[ROCKETNET_SIGNED_OFFSET_IPV4] && output.ipv4_address[5] == signed_data[ROCKETNET_SIGNED_OFFSET_IPV4 + 5],
                      "the ipv4 address is read at its offset" );
    __CPROVER_assert( output.ipv6_address[0] == signed_data[ROCKETNET_SIGNED_OFFSET_IPV6] && output.ipv6_address[17] == signed_data[ROCKETNET_SIGNED_OFFSET_IPV6 + 17],
                      "the ipv6 address is read at its offset" );
    __CPROVER_assert( output.server_public_key[0] == signed_data[ROCKETNET_SIGNED_OFFSET_SERVER_KEY] && output.server_public_key[ROCKETNET_KEY_BYTES - 1] == signed_data[ROCKETNET_SIGNED_OFFSET_SERVER_KEY + ROCKETNET_KEY_BYTES - 1],
                      "the server public key is read at its offset" );
    __CPROVER_assert( output.sealed_private_block[0] == signed_data[ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK] && output.sealed_private_block[ROCKETNET_SEALED_BLOCK_BYTES - 1] == signed_data[ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES - 1],
                      "the sealed private block is read at its offset" );
    __CPROVER_assert( ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK + ROCKETNET_SEALED_BLOCK_BYTES <= ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                      "the sealed block ends inside the signed block" );
}

/* the private block: every field read at its documented offset, all of them
   inside ROCKETNET_PRIVATE_BLOCK_BYTES. */

void harness_read_connect_token_private( void )
{
    uint8_t buffer[ROCKETNET_PRIVATE_BLOCK_BYTES];
    struct rocketnet_connect_token_private_t output;

    rocketnet_read_connect_token_private( &output, buffer );

    const uint8_t * p = buffer;
    __CPROVER_assert( output.client_id == rocketnet_read_uint64( &p ), "client id is read first" );
    __CPROVER_assert( output.protocol_id == rocketnet_read_uint64( &p ), "protocol id follows the client id" );
    __CPROVER_assert( output.expire_timestamp == rocketnet_read_uint64( &p ), "expire follows the protocol id" );
    __CPROVER_assert( output.client_to_server_key[0] == p[0] && output.client_to_server_key[ROCKETNET_KEY_BYTES - 1] == p[ROCKETNET_KEY_BYTES - 1],
                      "the client-to-server key follows the expire" );
    p += ROCKETNET_KEY_BYTES;
    __CPROVER_assert( output.server_to_client_key[0] == p[0] && output.server_to_client_key[ROCKETNET_KEY_BYTES - 1] == p[ROCKETNET_KEY_BYTES - 1],
                      "the server-to-client key follows the client-to-server key" );
    p += ROCKETNET_KEY_BYTES;
    __CPROVER_assert( output.user_data[0] == p[0] && output.user_data[ROCKETNET_USER_DATA_BYTES - 1] == p[ROCKETNET_USER_DATA_BYTES - 1],
                      "the user data follows the server-to-client key" );
    p += ROCKETNET_USER_DATA_BYTES;
    __CPROVER_assert( (size_t) ( p - buffer ) <= ROCKETNET_PRIVATE_BLOCK_BYTES,
                      "the last private field ends inside the block" );
}

/* the signature verifier: symbolic signed data, signature and backend public
   key; the model reads all three and returns a symbolic success or failure. */

void harness_verify_signed_connect_data( void )
{
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    uint8_t backend_public_key[ROCKETNET_KEY_BYTES];

    const int result = rocketnet_verify_signed_connect_data( signed_data, signature, backend_public_key );

    __CPROVER_assert( result == 0 || result == 1, "verify returns 0 or 1" );
}

/* the sealed-block opener: exactly 576 bytes of signed data, so the sealed
   block at offset 89 is inside it. The model checks the offset and length. */

void harness_open_connect_token_private( void )
{
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t server_public_key[ROCKETNET_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_KEY_BYTES];
    struct rocketnet_connect_token_private_t output;

    g_signed_connect_data = signed_data;

    const int result = rocketnet_open_connect_token_private( &output, signed_data,
                                                             server_public_key, server_secret_key );

    __CPROVER_assert( result == 0 || result == 1, "open returns 0 or 1" );
}

/* the inner consistency check: every private field symbolic, the whole signed
   block symbolic; the two compared fields are read at their offsets. */

void harness_private_consistent( void )
{
    struct rocketnet_connect_token_private_t private_fields;
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];

    const int result = rocketnet_connect_token_private_consistent( &private_fields, signed_data );

    const uint8_t * p = signed_data + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID;
    const uint64_t signed_protocol_id = rocketnet_read_uint64( &p );
    p = signed_data + ROCKETNET_SIGNED_OFFSET_EXPIRE;
    const uint64_t signed_expire = rocketnet_read_uint64( &p );

    __CPROVER_assert( result == 0 || result == 1, "consistency returns 0 or 1" );
    __CPROVER_assert( result == ( private_fields.protocol_id == signed_protocol_id && private_fields.expire_timestamp == signed_expire ),
                      "consistency compares the protocol id and expire at their offsets" );
}

/* the identifier: the 16-byte hash of exactly 576 symbolic signed bytes. */

void harness_connect_token_identifier( void )
{
    uint8_t signed_data[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    uint8_t identifier[ROCKETNET_TOKEN_IDENTIFIER_BYTES];

    rocketnet_connect_token_identifier( identifier, signed_data );
}

int main( void )
{
    harness_read_connect_token();
    harness_read_signed_connect_data();
    harness_read_connect_token_private();
    harness_verify_signed_connect_data();
    harness_open_connect_token_private();
    harness_private_consistent();
    harness_connect_token_identifier();
    return 0;
}
