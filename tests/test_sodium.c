/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Fixed published vectors also run when no independent system sodium exists.
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "sodium check failed: %s at %d\n", #x, __LINE__); exit(1); } } while (0)
static void unhex( unsigned char * out, const char * text )
{
    for ( size_t i = 0; i < strlen( text ) / 2; ++i )
    {
        const char pair[3] = { text[i * 2], text[i * 2 + 1], '\0' };
        char * end = NULL;
        const unsigned long byte = strtoul( pair, &end, 16 );
        CHECK( end != pair );
        out[i] = (unsigned char) byte;
    }
}
static void equal( const unsigned char * actual, const char * hex )
{
    unsigned char expected[64];
    CHECK( strlen( hex ) / 2 <= sizeof( expected ) );
    unhex( expected, hex );
    CHECK( memcmp( actual, expected, strlen( hex ) / 2 ) == 0 );
}
int main( void )
{
    unsigned char out[64], seed[32], pk[32], sk[64];
    CHECK( sodium_init() >= 0 );
    // BLAKE2b empty string, SHA-512 empty string, SipHash reference vector 0.
    CHECK( crypto_generichash( out, 64, (const unsigned char *) "", 0, NULL, 0 ) == 0 );
    equal( out, "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce" );
    CHECK( crypto_hash_sha512( out, (const unsigned char *) "", 0 ) == 0 );
    equal( out, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" );
    for ( int i = 0; i < 32; ++i )
        seed[i] = (unsigned char) i;
    CHECK( crypto_shorthash( out, (const unsigned char *) "", 0, seed ) == 0 );
    equal( out, "310e0edd47db6f72" );
    // RFC 7748 Alice public key.
    unhex( seed, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a" );
    CHECK( crypto_scalarmult_base( out, seed ) == 0 );
    equal( out, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a" );
    // RFC 8032 Ed25519 test 1 (empty message).
    unhex( seed, "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60" );
    CHECK( crypto_sign_seed_keypair( pk, sk, seed ) == 0 );
    equal( pk, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a" );
    CHECK( crypto_sign_detached( out, NULL, (const unsigned char *) "", 0, sk ) == 0 );
    equal( out, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" );
    CHECK( crypto_sign_verify_detached( out, (const unsigned char *) "", 0, pk ) == 0 );
    out[0] ^= 1;
    CHECK( crypto_sign_verify_detached( out, (const unsigned char *) "", 0, pk ) == -1 );
    // Empty message and associated data may be NULL. Stale baseline
    // nonnull annotations used to trip UBSan inside the AEAD implementation.
    unsigned char nonce[12] = { 0 }, key[32] = { 0 }, plain[1];
    unsigned long long count = 99;
    CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( out, &count, NULL, 0, NULL, 0, NULL, nonce, key ) == 0 );
    CHECK( count == 16 );
    CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( plain, &count, NULL, out, 16, NULL, 0, nonce, key ) == 0 );
    CHECK( count == 0 );
    puts( "test_sodium_known_answers" );
    return 0;
}
