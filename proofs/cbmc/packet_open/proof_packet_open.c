/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the packet prefilter, the filter epoch table, the sealed
    packet opener, the associated data writer and the sequence nonce.

    The readers are sdk/rocketnet_protocol.cpp, included below. libsodium is
    not proved: each call the readers make is a model in this harness that
    checks its own buffer arguments with __CPROVER_r_ok / __CPROVER_w_ok at
    the lengths libsodium documents, writes nondet bytes, and returns a nondet
    success or failure.

    Packet buffers are allocated at exactly the symbolic packet_bytes, so a
    read one past the caller's length is an out-of-bounds failure and not
    slack. packet_bytes runs over 0..ROCKETNET_RECEIVE_PACKET_BYTES (the receive
    ceiling) for the opener, and over 0..ROCKETNET_RECEIVE_CEILING + 1 for the
    prefilter so both refusal edges are covered.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 64. The epoch table is at most four entries; the fixed-length
    memcpy and memcmp models are at most 32 bytes.
*/

#define thread_local

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/*
    rocketnet_ack.h is C++ (templates, default arguments). The readers proved
    here do not use it; rocketnet_protocol.cpp calls one function from it, the
    ack block builder, which no harness below reaches. The guard is taken and
    that one declaration stands in, so the C front end parses the readers.
*/
#define ROCKETNET_SDK_ROCKETNET_ACK_H
struct rocketnet_replay_protection_t;
int rocketnet_ack_block_build( uint8_t * output, int capacity, const struct rocketnet_replay_protection_t * replay );

#include "rocketnet_protocol.cpp"

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
#endif // #ifndef __CPROVER__

/* libsodium models. */

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

void sodium_memzero( void * const pnt, const size_t len )
{
    __CPROVER_assert( PROOF_W_OK( pnt, len ), "memzero writes its buffer" );
    PROOF_HAVOC( pnt, len );
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
    __CPROVER_assert( PROOF_R_OK( m, mlen ), "encrypt reads the plaintext" );
    __CPROVER_assert( PROOF_R_OK( ad, adlen ), "encrypt reads the associated data" );
    __CPROVER_assert( PROOF_R_OK( npub, 12 ), "encrypt reads the 12-byte nonce" );
    __CPROVER_assert( PROOF_R_OK( k, 32 ), "encrypt reads the 32-byte key" );
    __CPROVER_assert( PROOF_W_OK( c, mlen + 16 ), "encrypt writes the ciphertext and tag" );
    __CPROVER_assert( PROOF_W_OK( clen_p, 8 ), "encrypt writes the ciphertext length" );
    PROOF_HAVOC( c, mlen + 16 );
    *clen_p = mlen + 16;

    return 0;
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

/* the prefilter: any packet_bytes below the header or above the receive
   ceiling is refused before a read; otherwise the packet is indexed only
   inside packet_bytes. */

void harness_prefilter( void )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_RECEIVE_CEILING + 1 );

    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );

    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];
    const int result = rocketnet_prefilter_check( packet, packet_bytes, packet_filter );

    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES )
    {
        __CPROVER_assert( result == ROCKETNET_PREFILTER_TOO_SHORT, "a short packet is refused as too short" );
        return;
    }

    if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
    {
        __CPROVER_assert( result == ROCKETNET_PREFILTER_TOO_LARGE, "an oversized packet is refused as too large" );
        return;
    }

    if ( packet[1] != ROCKETNET_FILTER_EPOCH_UNKEYED )
    {
        __CPROVER_assert( result == ROCKETNET_PREFILTER_WRONG_EPOCH, "a keyed packet is refused at the unkeyed epoch" );
        return;
    }

    if ( packet[2] != packet_filter[0] || packet[3] != packet_filter[1] ||
         packet[4] != packet_filter[2] || packet[5] != packet_filter[3] )
    {
        __CPROVER_assert( result == ROCKETNET_PREFILTER_WRONG_VALUE, "a wrong filter value is refused" );
        return;
    }

    __CPROVER_assert( result == packet[0], "an accepted packet returns its type" );
}

/* the sealed opener: every packet type byte, sequence, session-nonce and
   ephemeral pairing, over exactly packet_bytes of packet and out_capacity of
   output. */

static void open_assert( int result, int out_capacity )
{
    __CPROVER_assert( result >= -1, "open returns -1 or a length" );
    __CPROVER_assert( result <= out_capacity, "open result fits the output capacity" );
}

void harness_open_sealed_packet( void )
{
    int packet_bytes = nondet_int();
    __CPROVER_assume( packet_bytes >= 0 && packet_bytes <= ROCKETNET_RECEIVE_PACKET_BYTES );

    int out_capacity = nondet_int();
    __CPROVER_assume( out_capacity >= 0 && out_capacity <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    uint8_t * packet = (uint8_t *) malloc( (size_t) packet_bytes );
    __CPROVER_assume( packet != NULL );

    uint8_t * out = (uint8_t *) malloc( (size_t) out_capacity );
    __CPROVER_assume( out != NULL );

    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t key[ROCKETNET_KEY_BYTES];

    const uint8_t packet_type = nondet_uchar();
    const uint64_t sequence = nondet_uint64();
    const uint64_t protocol_id = nondet_uint64();

    const int has_nonce = rocketnet_packet_type_has_session_nonce( packet_type );
    const int has_ephemeral = rocketnet_packet_type_has_ephemeral_in_ad( packet_type );
    const int sealed = rocketnet_packet_type_is_sealed( packet_type );

    const int header_bytes = ROCKETNET_PREFILTER_BYTES + ROCKETNET_SEQUENCE_BYTES +
                             ( has_nonce ? ROCKETNET_SESSION_NONCE_BYTES : 0 );

    const int result = rocketnet_open_sealed_packet( out,
                                                     has_nonce ? session_nonce : NULL,
                                                     out_capacity,
                                                     packet,
                                                     packet_bytes,
                                                     packet_type,
                                                     sequence,
                                                     protocol_id,
                                                     connection_salt,
                                                     has_ephemeral ? ephemeral : NULL,
                                                     key );
    open_assert( result, out_capacity );

    if ( !sealed || sequence == 0 )
    {
        __CPROVER_assert( result == -1, "an unsealed type or zero sequence is refused" );
    }
    else if ( packet_bytes < header_bytes + ROCKETNET_PACKET_TAG_BYTES )
    {
        __CPROVER_assert( result == -1, "a short sealed packet is refused" );
    }
    else if ( out_capacity < packet_bytes - header_bytes - ROCKETNET_PACKET_TAG_BYTES )
    {
        __CPROVER_assert( result == -1, "a small output is refused" );
    }
    else
    {
        __CPROVER_assert( result == -1 || result == packet_bytes - header_bytes - ROCKETNET_PACKET_TAG_BYTES,
                          "an opened packet returns the plaintext length" );
    }

    // a session nonce on a type that does not carry one is refused

    const int extra_nonce = rocketnet_open_sealed_packet( out,
                                                          session_nonce,
                                                          out_capacity,
                                                          packet,
                                                          packet_bytes,
                                                          packet_type,
                                                          sequence,
                                                          protocol_id,
                                                          connection_salt,
                                                          has_ephemeral ? ephemeral : NULL,
                                                          key );

    if ( !has_nonce )
    {
        __CPROVER_assert( extra_nonce == -1, "a session nonce on the wrong type is refused" );
    }

    // an ephemeral key on a type that does not carry one is refused

    const int extra_ephemeral = rocketnet_open_sealed_packet( out,
                                                              has_nonce ? session_nonce : NULL,
                                                              out_capacity,
                                                              packet,
                                                              packet_bytes,
                                                              packet_type,
                                                              sequence,
                                                              protocol_id,
                                                              connection_salt,
                                                              ephemeral,
                                                              key );

    if ( !has_ephemeral )
    {
        __CPROVER_assert( extra_ephemeral == -1, "an ephemeral key on the wrong type is refused" );
    }

    // a type that requires one and does not get it is refused

    const int missing_pairing = rocketnet_open_sealed_packet( out,
                                                              NULL,
                                                              out_capacity,
                                                              packet,
                                                              packet_bytes,
                                                              packet_type,
                                                              sequence,
                                                              protocol_id,
                                                              connection_salt,
                                                              NULL,
                                                              key );

    if ( has_nonce || has_ephemeral )
    {
        __CPROVER_assert( missing_pairing == -1, "a required nonce or ephemeral key is refused when missing" );
    }
}

/* the associated data writer: 53 bytes with the ephemeral key, 21 without. */

void harness_write_packet_associated_data( void )
{
    uint8_t with_ephemeral[ROCKETNET_AD_WITH_EPHEMERAL_BYTES];
    uint8_t without_ephemeral[ROCKETNET_AD_BYTES];
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t ephemeral[ROCKETNET_KEY_BYTES];

    const uint64_t protocol_id = nondet_uint64();
    const uint8_t packet_type = nondet_uchar();

    const int with_bytes = rocketnet_write_packet_associated_data( with_ephemeral,
                                                                   protocol_id,
                                                                   packet_type,
                                                                   connection_salt,
                                                                   ephemeral );
    __CPROVER_assert( with_bytes == ROCKETNET_AD_WITH_EPHEMERAL_BYTES,
                      "the writer returns 53 bytes with the ephemeral key" );

    const int without_bytes = rocketnet_write_packet_associated_data( without_ephemeral,
                                                                      protocol_id,
                                                                      packet_type,
                                                                      connection_salt,
                                                                      NULL );
    __CPROVER_assert( without_bytes == ROCKETNET_AD_BYTES,
                      "the writer returns 21 bytes without the ephemeral key" );
}

/* the sequence nonce: four zero bytes then the sequence little-endian, all
   inside twelve bytes. */

void harness_sequence_nonce( void )
{
    uint8_t nonce[12];
    const uint64_t sequence = nondet_uint64();

    rocketnet_sequence_nonce( nonce, sequence );

    __CPROVER_assert( nonce[0] == 0 && nonce[1] == 0 && nonce[2] == 0 && nonce[3] == 0,
                      "the nonce starts with four zero bytes" );
    for ( int i = 0; i < 8; i++ )
        __CPROVER_assert( nonce[4 + i] == (uint8_t) ( ( sequence >> ( 8 * i ) ) & 0xFF ),
                          "the nonce carries the sequence little-endian" );
}

int main( void )
{
    harness_prefilter();
    harness_open_sealed_packet();
    harness_write_packet_associated_data();
    harness_sequence_nonce();

    return 0;
}
