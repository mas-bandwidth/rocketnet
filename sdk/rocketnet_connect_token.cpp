/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_connect_token.h"
#include "rocketnet_crypto_tally.h"
#include "rocketnet_read_write.h"
#include "rocketnet_address.h"
#include "rocketnet_internal.h"
#include "rocketnet_test_seams.h"

#include <sodium.h>
#include <string.h>

void rocketnet_write_connect_token_private( const struct rocketnet_connect_token_private_t * input, uint8_t * buffer )
{
    uint8_t * p = buffer;
    rocketnet_write_uint64( &p, input->client_id );
    rocketnet_write_uint64( &p, input->protocol_id );
    rocketnet_write_uint64( &p, input->expire_timestamp );
    rocketnet_write_bytes( &p, input->client_to_server_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, input->server_to_client_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, input->user_data, ROCKETNET_USER_DATA_BYTES );
    rocketnet_write_zero_bytes( &p, ROCKETNET_PRIVATE_BLOCK_BYTES - (int) ( p - buffer ) );
}

void rocketnet_read_connect_token_private( struct rocketnet_connect_token_private_t * output, const uint8_t * buffer )
{
    const uint8_t * p = buffer;
    output->client_id = rocketnet_read_uint64( &p );
    output->protocol_id = rocketnet_read_uint64( &p );
    output->expire_timestamp = rocketnet_read_uint64( &p );
    rocketnet_read_bytes( &p, output->client_to_server_key, ROCKETNET_KEY_BYTES );
    rocketnet_read_bytes( &p, output->server_to_client_key, ROCKETNET_KEY_BYTES );
    rocketnet_read_bytes( &p, output->user_data, ROCKETNET_USER_DATA_BYTES );
}

void rocketnet_write_signed_connect_data( const struct rocketnet_signed_connect_data_t * input, uint8_t * buffer )
{
    uint8_t * p = buffer;
    rocketnet_write_uint32( &p, input->version );
    rocketnet_write_uint64( &p, input->protocol_id );
    rocketnet_write_uint64( &p, input->create_timestamp );
    rocketnet_write_uint64( &p, input->expire_timestamp );
    rocketnet_write_uint32( &p, (uint32_t) input->timeout_seconds );
    rocketnet_write_uint8( &p, input->address_flags );
    rocketnet_write_bytes( &p, input->ipv4_address, 6 );
    rocketnet_write_bytes( &p, input->ipv6_address, 18 );
    rocketnet_write_bytes( &p, input->server_public_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, input->sealed_private_block, ROCKETNET_SEALED_BLOCK_BYTES );
    rocketnet_write_zero_bytes( &p, ROCKETNET_SIGNED_CONNECT_DATA_BYTES - (int) ( p - buffer ) );
}

void rocketnet_read_signed_connect_data( struct rocketnet_signed_connect_data_t * output, const uint8_t * buffer )
{
    const uint8_t * p = buffer;
    output->version = rocketnet_read_uint32( &p );
    output->protocol_id = rocketnet_read_uint64( &p );
    output->create_timestamp = rocketnet_read_uint64( &p );
    output->expire_timestamp = rocketnet_read_uint64( &p );
    output->timeout_seconds = (int32_t) rocketnet_read_uint32( &p );
    output->address_flags = rocketnet_read_uint8( &p );
    rocketnet_read_bytes( &p, output->ipv4_address, 6 );
    rocketnet_read_bytes( &p, output->ipv6_address, 18 );
    rocketnet_read_bytes( &p, output->server_public_key, ROCKETNET_KEY_BYTES );
    rocketnet_read_bytes( &p, output->sealed_private_block, ROCKETNET_SEALED_BLOCK_BYTES );
}

bool rocketnet_signed_connect_data_names_port_zero( const struct rocketnet_signed_connect_data_t * signed_fields )
{
    rocketnet_assert( signed_fields );

    // each port is the address's last two bytes, little-endian

    const bool ipv4_port_zero = signed_fields->ipv4_address[4] == 0 && signed_fields->ipv4_address[5] == 0;
    const bool ipv6_port_zero = signed_fields->ipv6_address[16] == 0 && signed_fields->ipv6_address[17] == 0;

    return ( ( signed_fields->address_flags & ROCKETNET_ADDRESS_FLAG_IPV4 ) != 0 && ipv4_port_zero ) ||
           ( ( signed_fields->address_flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) != 0 && ipv6_port_zero );
}

#if ROCKETNET_DEV

static int rocketnet_box_seal_with_ephemeral( uint8_t * c, const uint8_t * m, unsigned long long mlen,
                                              const uint8_t * pk, const uint8_t * esk )
{
    // crypto_box_seal's construction with the ephemeral secret supplied rather than
    // drawn: the test-only deterministic writer's seal, and nothing else's. Output
    // is epk || crypto_box_easy(m), crypto_box_SEALBYTES longer than m. 0 on
    // success, as libsodium returns.

    int result = -1;
    uint8_t epk[crypto_box_PUBLICKEYBYTES];
    uint8_t nonce[crypto_box_NONCEBYTES];
    crypto_generichash_state state;

    if ( crypto_scalarmult_base( epk, esk ) != 0 )
        goto out;

    if ( crypto_generichash_init( &state, NULL, 0, crypto_box_NONCEBYTES ) != 0 ||
         crypto_generichash_update( &state, epk, crypto_box_PUBLICKEYBYTES ) != 0 ||
         crypto_generichash_update( &state, pk, crypto_box_PUBLICKEYBYTES ) != 0 ||
         crypto_generichash_final( &state, nonce, crypto_box_NONCEBYTES ) != 0 )
        goto out;

    if ( crypto_box_easy( c + crypto_box_PUBLICKEYBYTES, m, mlen, nonce, pk, esk ) != 0 )
        goto out;

    memcpy( c, epk, crypto_box_PUBLICKEYBYTES );
    result = 0;

out:
    sodium_memzero( nonce, sizeof( nonce ) );
    sodium_memzero( &state, sizeof( state ) );
    return result;
}

#endif // #if ROCKETNET_DEV

static int rocketnet_write_connect_token_sealed( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                 const struct rocketnet_connect_token_private_t * private_fields,
                                                 const uint8_t * backend_secret_key,
                                                 const uint8_t * ephemeral_secret_key,
                                                 uint8_t * token )
{
    // the one writer behind both entry points: ephemeral_secret_key NULL seals
    // with crypto_box_seal's fresh ephemeral key (production); non-NULL seals
    // with that key (the test-only vectors, ROCKETNET_DEV only)

    int result = 0;
    uint8_t * p = NULL;

    // the inner protocol id and expire are the signed ones, by construction

    struct rocketnet_connect_token_private_t private_data = *private_fields;
    private_data.protocol_id = signed_fields->protocol_id;
    private_data.expire_timestamp = signed_fields->expire_timestamp;

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &private_data, private_block );

    struct rocketnet_signed_connect_data_t signed_data = *signed_fields;
#if ROCKETNET_DEV
    if ( ephemeral_secret_key == NULL )
    {
        if ( crypto_box_seal( signed_data.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, signed_data.server_public_key ) != 0 )
            goto out;
    }
    else
    {
        if ( rocketnet_box_seal_with_ephemeral( signed_data.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES,
                                                signed_data.server_public_key, ephemeral_secret_key ) != 0 )
            goto out;
    }
#else // #if ROCKETNET_DEV
    (void) ephemeral_secret_key;

    if ( crypto_box_seal( signed_data.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, signed_data.server_public_key ) != 0 )
        goto out;
#endif // #if ROCKETNET_DEV

    p = token;
    rocketnet_write_signed_connect_data( &signed_data, p );
    p += ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    if ( crypto_sign_detached( signature, NULL, token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, backend_secret_key ) != 0 )
        goto out;

    rocketnet_write_bytes( &p, signature, ROCKETNET_SIGNATURE_BYTES );
    rocketnet_write_bytes( &p, private_data.client_to_server_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, private_data.server_to_client_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_zero_bytes( &p, ROCKETNET_CONNECT_TOKEN_BYTES - (int) ( p - token ) );

    result = 1;

out:
    // the wipe runs on every path out, including the two goto out paths, and
    // nothing outside this call observes either buffer

    sodium_memzero( private_block, sizeof( private_block ) );
    sodium_memzero( &private_data, sizeof( private_data ) );
    return result;
}

int rocketnet_write_connect_token( const struct rocketnet_signed_connect_data_t * signed_fields,
                                   const struct rocketnet_connect_token_private_t * private_fields,
                                   const uint8_t * backend_secret_key,
                                   uint8_t * token )
{
    return rocketnet_write_connect_token_sealed( signed_fields, private_fields, backend_secret_key, NULL, token );
}

#if ROCKETNET_DEV

int rocketnet_write_connect_token_deterministic( const struct rocketnet_signed_connect_data_t * signed_fields,
                                                 const struct rocketnet_connect_token_private_t * private_fields,
                                                 const uint8_t * backend_secret_key,
                                                 const uint8_t * ephemeral_secret_key,
                                                 uint8_t * token )
{
    // TEST-ONLY: exists for standard/vectors/connect_token_v1.json; see the header

    if ( ephemeral_secret_key == NULL )
        return 0;

    return rocketnet_write_connect_token_sealed( signed_fields, private_fields, backend_secret_key, ephemeral_secret_key, token );
}

#endif // #if ROCKETNET_DEV

enum rocketnet_status_t rocketnet_generate_backend_keypair( uint8_t * public_key, uint8_t * secret_key )
{
    if ( !rocketnet_api_null_guard( public_key ) || !rocketnet_api_null_guard( secret_key ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( !rocketnet_crypto_ready() )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    if ( crypto_sign_keypair( public_key, secret_key ) != 0 )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    return ROCKETNET_STATUS_OK;
}

enum rocketnet_status_t rocketnet_generate_backend_keypair_from_seed( const uint8_t * seed, uint8_t * public_key, uint8_t * secret_key )
{
    if ( !rocketnet_api_null_guard( seed ) || !rocketnet_api_null_guard( public_key ) || !rocketnet_api_null_guard( secret_key ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( !rocketnet_crypto_ready() )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    if ( crypto_sign_seed_keypair( public_key, secret_key, seed ) != 0 )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    return ROCKETNET_STATUS_OK;
}

enum rocketnet_status_t rocketnet_generate_server_keypair( uint8_t * public_key, uint8_t * secret_key )
{
    if ( !rocketnet_api_null_guard( public_key ) || !rocketnet_api_null_guard( secret_key ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( !rocketnet_crypto_ready() )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    if ( crypto_box_keypair( public_key, secret_key ) != 0 )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    return ROCKETNET_STATUS_OK;
}

enum rocketnet_status_t rocketnet_write_production_connect_token( const uint8_t * backend_secret_key,
                                                                  const uint8_t * server_public_key,
                                                                  uint64_t protocol_id,
                                                                  const char * const * server_address_strings,
                                                                  int server_address_count,
                                                                  uint64_t client_id,
                                                                  uint64_t create_timestamp,
                                                                  uint64_t expire_timestamp,
                                                                  int32_t timeout_seconds,
                                                                  const uint8_t * user_data,
                                                                  uint8_t * connect_token,
                                                                  size_t connect_token_bytes )
{
    if ( !rocketnet_api_null_guard( backend_secret_key ) ||
         !rocketnet_api_null_guard( server_public_key ) ||
         !rocketnet_api_null_guard( server_address_strings ) ||
         !rocketnet_api_null_guard( connect_token ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( server_address_count < 1 || server_address_count > 2 )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( connect_token_bytes < ROCKETNET_CONNECT_TOKEN_BYTES )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( protocol_id == 0 )
        return ROCKETNET_STATUS_INVALID_PROTOCOL_ID;

    // every endpoint refuses these: a production server refuses a timeout of zero or below (t13.2),
    // and a token whose expire is at or before its create is already expired when it is made

    if ( timeout_seconds <= 0 || expire_timestamp <= create_timestamp )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = protocol_id;
    signed_fields.create_timestamp = create_timestamp;
    signed_fields.expire_timestamp = expire_timestamp;
    signed_fields.timeout_seconds = timeout_seconds;
    memcpy( signed_fields.server_public_key, server_public_key, ROCKETNET_KEY_BYTES );

    for ( int i = 0; i < server_address_count; i++ )
    {
        if ( !rocketnet_api_null_guard( server_address_strings[i] ) )
            return ROCKETNET_STATUS_INVALID_ARGUMENT;

        struct rocketnet_address_t address;
        if ( !rocketnet_address_parse( &address, server_address_strings[i] ) )
            return ROCKETNET_STATUS_INVALID_ADDRESS;

        // a token naming port 0 names no server: refuse the address before
        // any field is written

        if ( address.port == 0 )
            return ROCKETNET_STATUS_INVALID_ADDRESS;

        uint8_t * p;
        if ( address.type == ROCKETNET_ADDRESS_IPV4 )
        {
            if ( signed_fields.address_flags & ROCKETNET_ADDRESS_FLAG_IPV4 )
                return ROCKETNET_STATUS_INVALID_ARGUMENT;

            signed_fields.address_flags |= ROCKETNET_ADDRESS_FLAG_IPV4;
            p = signed_fields.ipv4_address;
            rocketnet_write_bytes( &p, address.data.ipv4, 4 );
            rocketnet_write_uint16( &p, address.port );
        }
        else if ( address.type == ROCKETNET_ADDRESS_IPV6 )
        {
            if ( signed_fields.address_flags & ROCKETNET_ADDRESS_FLAG_IPV6 )
                return ROCKETNET_STATUS_INVALID_ARGUMENT;

            signed_fields.address_flags |= ROCKETNET_ADDRESS_FLAG_IPV6;
            p = signed_fields.ipv6_address;

            for ( int j = 0; j < 8; j++ )
                rocketnet_write_uint16( &p, address.data.ipv6[j] );

            rocketnet_write_uint16( &p, address.port );
        }
        else
        {
            return ROCKETNET_STATUS_INVALID_ADDRESS;
        }
    }

    if ( !rocketnet_crypto_ready() )
        return ROCKETNET_STATUS_CRYPTO_INIT_FAILED;

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    randombytes_buf( private_fields.client_to_server_key, ROCKETNET_KEY_BYTES );
    randombytes_buf( private_fields.server_to_client_key, ROCKETNET_KEY_BYTES );

    if ( user_data != NULL )
        memcpy( private_fields.user_data, user_data, ROCKETNET_USER_DATA_BYTES );

    const int result = rocketnet_write_connect_token( &signed_fields, &private_fields, backend_secret_key, connect_token );

    sodium_memzero( &private_fields, sizeof( private_fields ) );

    return result ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CRYPTO_INIT_FAILED;
}

void rocketnet_read_connect_token( struct rocketnet_connect_token_t * output, const uint8_t * token )
{
    const uint8_t * p = token;
    rocketnet_read_bytes( &p, output->signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES );
    rocketnet_read_bytes( &p, output->signature, ROCKETNET_SIGNATURE_BYTES );
    rocketnet_read_bytes( &p, output->client_to_server_key, ROCKETNET_KEY_BYTES );
    rocketnet_read_bytes( &p, output->server_to_client_key, ROCKETNET_KEY_BYTES );
}

int rocketnet_verify_signed_connect_data( const uint8_t * signed_connect_data,
                                          const uint8_t * signature,
                                          const uint8_t * backend_public_key )
{
    ROCKETNET_CRYPTO_TALLY( signature_verify );
    return crypto_sign_verify_detached( signature, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, backend_public_key ) == 0;
}

// The private block seam, compiled into the unit suite's hook library only
// (ROCKETNET_DEV): rocketnet_open_connect_token_private
// hands its private block staging to the hook as it enters (at_exit 0, so a
// unit case can plant a canary in it) and on each of its two exits (at_exit
// 1, so the case reads what the staging still holds)
// (test_unit_secrets_wiped.cpp).
#if ROCKETNET_DEV
void ( *rocketnet_token_private_block_hook )( uint8_t * block, int bytes, int at_exit ) = NULL;
#define ROCKETNET_TOKEN_PRIVATE_BLOCK_SEEN( block, bytes, at_exit ) do { if ( rocketnet_token_private_block_hook ) rocketnet_token_private_block_hook( block, bytes, at_exit ); } while ( 0 )
#else
#define ROCKETNET_TOKEN_PRIVATE_BLOCK_SEEN( block, bytes, at_exit ) do {} while ( 0 )
#endif // #if ROCKETNET_DEV

int rocketnet_open_connect_token_private( struct rocketnet_connect_token_private_t * output,
                                          const uint8_t * signed_connect_data,
                                          const uint8_t * server_public_key,
                                          const uint8_t * server_secret_key )
{
    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    ROCKETNET_TOKEN_PRIVATE_BLOCK_SEEN( private_block, (int) sizeof( private_block ), 0 );

    ROCKETNET_CRYPTO_TALLY( seal_open );

    if ( crypto_box_seal_open( private_block,
                               signed_connect_data + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK,
                               ROCKETNET_SEALED_BLOCK_BYTES,
                               server_public_key,
                               server_secret_key ) != 0 )
    {
        // the refused exit wipes the staging as the open exit does

        sodium_memzero( private_block, sizeof( private_block ) );
        ROCKETNET_TOKEN_PRIVATE_BLOCK_SEEN( private_block, (int) sizeof( private_block ), 1 );
        return 0;
    }

    rocketnet_read_connect_token_private( output, private_block );
    sodium_memzero( private_block, sizeof( private_block ) );
    ROCKETNET_TOKEN_PRIVATE_BLOCK_SEEN( private_block, (int) sizeof( private_block ), 1 );

    return 1;
}

int rocketnet_connect_token_private_consistent( const struct rocketnet_connect_token_private_t * private_fields,
                                                const uint8_t * signed_connect_data )
{
    const uint8_t * p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_PROTOCOL_ID;
    uint64_t signed_protocol_id = rocketnet_read_uint64( &p );

    p = signed_connect_data + ROCKETNET_SIGNED_OFFSET_EXPIRE;
    uint64_t signed_expire = rocketnet_read_uint64( &p );

    return private_fields->protocol_id == signed_protocol_id && private_fields->expire_timestamp == signed_expire;
}

void rocketnet_connect_token_identifier( uint8_t * identifier, const uint8_t * signed_connect_data )
{
    crypto_generichash( identifier, ROCKETNET_TOKEN_IDENTIFIER_BYTES,
                        signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES,
                        NULL, 0 );
}

// the published development keys, from the standard, byte for byte -- the
// test suite asserts these equal the vector copies, so they cannot drift

const uint8_t rocketnet_dev_backend_public_key[32] = {
    0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99,
    0x29, 0xa5, 0xb4, 0xbf, 0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d,
    0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c
};

#if ROCKETNET_DEV
const uint8_t rocketnet_dev_backend_secret_key[64] = {
    0x75, 0x29, 0x02, 0x15, 0xe1, 0xff, 0x4d, 0xf2, 0x09, 0x92, 0x57, 0x8a,
    0x22, 0x3c, 0x7c, 0xd4, 0x63, 0xb9, 0xa2, 0x7c, 0xf8, 0x3a, 0xa7, 0x1c,
    0x7a, 0x16, 0x93, 0x1c, 0x01, 0x9b, 0x7c, 0x30, 0xb0, 0xab, 0x03, 0xb3,
    0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf,
    0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8,
    0x58, 0xbd, 0x24, 0x2c
};
#endif // #if ROCKETNET_DEV

const uint8_t rocketnet_dev_server_public_key[32] = {
    0x3d, 0x24, 0x90, 0x88, 0x9d, 0x37, 0x4c, 0x29, 0x6f, 0xab, 0x1d, 0x57,
    0x70, 0xcb, 0x71, 0x47, 0x6a, 0x06, 0xf7, 0x4e, 0xf2, 0xf2, 0x54, 0x0c,
    0xf2, 0x21, 0x5f, 0x76, 0x14, 0xf4, 0x80, 0x1c
};

#if ROCKETNET_DEV
const uint8_t rocketnet_dev_server_secret_key[32] = {
    0x6f, 0xfb, 0x8f, 0x73, 0x37, 0xec, 0x37, 0x76, 0x97, 0xf5, 0x68, 0xde,
    0x31, 0x18, 0xd8, 0x39, 0x59, 0x56, 0x0b, 0x25, 0x92, 0x8d, 0x22, 0x70,
    0x7b, 0xa2, 0x29, 0x75, 0x89, 0xd0, 0x8b, 0x97
};


// the development token: the production writer over the published development
// keys, protocol id, lifetimes and one server address. A token naming port 0
// is refused, as the production writer refuses it.

enum rocketnet_status_t rocketnet_write_test_connect_token( const char * server_address_string,
                                                            uint64_t client_id,
                                                            uint64_t current_timestamp,
                                                            uint8_t * connect_token,
                                                            size_t connect_token_bytes )
{
    if ( !rocketnet_api_null_guard( server_address_string ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    if ( connect_token && connect_token_bytes < ROCKETNET_CONNECT_TOKEN_BYTES )
    {
        rocketnet_error( "test connect token refused: the buffer is %d bytes, not the %d a token needs", (int) connect_token_bytes, ROCKETNET_CONNECT_TOKEN_BYTES );
        return ROCKETNET_STATUS_INVALID_ARGUMENT;
    }

    const char * const addresses[1] = { server_address_string };

    return rocketnet_write_production_connect_token( rocketnet_dev_backend_secret_key, rocketnet_dev_server_public_key, ROCKETNET_DEV_PROTOCOL_ID,
                                                     addresses, 1, client_id,
                                                     current_timestamp, current_timestamp + ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS,
                                                     ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, NULL,
                                                     connect_token, connect_token_bytes );
}

enum rocketnet_status_t rocketnet_create_dev_connect_token( const char * server_address_ipv4,
                                                            const char * server_address_ipv6,
                                                            const uint8_t * server_public_key,
                                                            int expire_seconds,
                                                            int timeout_seconds,
                                                            uint64_t client_id,
                                                            uint64_t protocol_id,
                                                            uint64_t unix_timestamp,
                                                            const uint8_t * user_data,
                                                            char * connect_token )
{
    // the public development token: rocketnet_create_connect_token with the
    // published development backend key

    if ( !rocketnet_api_null_guard( server_public_key ) )
        return ROCKETNET_STATUS_INVALID_ARGUMENT;

    struct rocketnet_connect_token_config_t config;
    rocketnet_connect_token_default_config( &config );
    memcpy( config.backend_secret_key, rocketnet_dev_backend_secret_key, sizeof( config.backend_secret_key ) );
    config.server_address_ipv4 = server_address_ipv4;
    config.server_address_ipv6 = server_address_ipv6;
    memcpy( config.server_public_key, server_public_key, sizeof( config.server_public_key ) );
    config.client_id = client_id;
    config.protocol_id = protocol_id;
    if ( user_data )
        memcpy( config.user_data, user_data, sizeof( config.user_data ) );
    config.expire_seconds = expire_seconds;
    config.timeout_seconds = timeout_seconds;
    config.unix_timestamp = unix_timestamp;

    const enum rocketnet_status_t status = rocketnet_create_connect_token( &config, connect_token );

    sodium_memzero( &config, sizeof( config ) );

    return status;
}
#endif // #if ROCKETNET_DEV
