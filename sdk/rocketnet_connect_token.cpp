/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_connect_token.h"
#include "rocketnet_read_write.h"
#include "rocketnet_address.h"
#include "rocketnet.h"

#include <sodium.h>
#include <string.h>

uint8_t rocketnet_test_private_block_copy[ROCKETNET_PRIVATE_BLOCK_BYTES];
bool rocketnet_test_private_block_copied = false;

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

int rocketnet_write_connect_token( const struct rocketnet_signed_connect_data_t * signed_fields,
                                   const struct rocketnet_connect_token_private_t * private_fields,
                                   const uint8_t * backend_secret_key,
                                   uint8_t * token )
{
    int result = 0;
    uint8_t * p = NULL;
    // the inner protocol id and expire are the signed ones, by construction
    struct rocketnet_connect_token_private_t private_data = *private_fields;
    private_data.protocol_id = signed_fields->protocol_id;
    private_data.expire_timestamp = signed_fields->expire_timestamp;

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &private_data, private_block );

    struct rocketnet_signed_connect_data_t signed_data = *signed_fields;
    if ( crypto_box_seal( signed_data.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, signed_data.server_public_key ) != 0 )
        goto out;

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
    sodium_memzero( private_block, sizeof(private_block) );
    sodium_memzero( &private_data, sizeof(private_data) );
    memcpy( rocketnet_test_private_block_copy, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES );
    rocketnet_test_private_block_copied = true;
    return result;
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
    return crypto_sign_verify_detached( signature, signed_connect_data, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, backend_public_key ) == 0;
}

int rocketnet_open_connect_token_private( struct rocketnet_connect_token_private_t * output,
                                          const uint8_t * signed_connect_data,
                                          const uint8_t * server_public_key,
                                          const uint8_t * server_secret_key )
{
    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    if ( crypto_box_seal_open( private_block,
                               signed_connect_data + ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK,
                               ROCKETNET_SEALED_BLOCK_BYTES,
                               server_public_key,
                               server_secret_key ) != 0 )
        return 0;
    rocketnet_read_connect_token_private( output, private_block );
    sodium_memzero( private_block, sizeof(private_block) );
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

const uint8_t rocketnet_dev_backend_public_key[32] =
{
    0xb0, 0xab, 0x03, 0xb3, 0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99,
    0x29, 0xa5, 0xb4, 0xbf, 0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d,
    0x08, 0x4a, 0x08, 0xd8, 0x58, 0xbd, 0x24, 0x2c};

#if ROCKETNET_DEVELOPMENT
const uint8_t rocketnet_dev_backend_secret_key[64] =
{
    0x75, 0x29, 0x02, 0x15, 0xe1, 0xff, 0x4d, 0xf2, 0x09, 0x92, 0x57, 0x8a,
    0x22, 0x3c, 0x7c, 0xd4, 0x63, 0xb9, 0xa2, 0x7c, 0xf8, 0x3a, 0xa7, 0x1c,
    0x7a, 0x16, 0x93, 0x1c, 0x01, 0x9b, 0x7c, 0x30, 0xb0, 0xab, 0x03, 0xb3,
    0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf,
    0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8,
    0x58, 0xbd, 0x24, 0x2c};
#endif // #if ROCKETNET_DEVELOPMENT

const uint8_t rocketnet_dev_server_public_key[32] =
{
    0x3d, 0x24, 0x90, 0x88, 0x9d, 0x37, 0x4c, 0x29, 0x6f, 0xab, 0x1d, 0x57,
    0x70, 0xcb, 0x71, 0x47, 0x6a, 0x06, 0xf7, 0x4e, 0xf2, 0xf2, 0x54, 0x0c,
    0xf2, 0x21, 0x5f, 0x76, 0x14, 0xf4, 0x80, 0x1c};

const uint8_t rocketnet_dev_server_secret_key[32] =
{
    0x6f, 0xfb, 0x8f, 0x73, 0x37, 0xec, 0x37, 0x76, 0x97, 0xf5, 0x68, 0xde,
    0x31, 0x18, 0xd8, 0x39, 0x59, 0x56, 0x0b, 0x25, 0x92, 0x8d, 0x22, 0x70,
    0x7b, 0xa2, 0x29, 0x75, 0x89, 0xd0, 0x8b, 0x97};

#if ROCKETNET_DEVELOPMENT
int rocketnet_write_dev_connect_token( const char * server_address_string,
                                       uint64_t client_id,
                                       uint64_t current_timestamp,
                                       uint8_t * connect_token )
{
    rocketnet_assert( server_address_string );
    rocketnet_assert( connect_token );

    struct rocketnet_address_t server_address;
    if ( !rocketnet_address_parse( &server_address, server_address_string ) )
        return 0;
    if ( server_address.type != ROCKETNET_ADDRESS_IPV4 && server_address.type != ROCKETNET_ADDRESS_IPV6 )
        return 0;

    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_VERSION;
    signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    signed_fields.create_timestamp = current_timestamp;
    signed_fields.expire_timestamp = current_timestamp + 300;
    signed_fields.timeout_seconds = 10;
    memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );

    uint8_t * p;
    if ( server_address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
        p = signed_fields.ipv4_address;
        rocketnet_write_bytes( &p, server_address.data.ipv4, 4 );
        rocketnet_write_uint16( &p, server_address.port );
    }
    else
    {
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
        p = signed_fields.ipv6_address;
        for ( int i = 0; i < 8; i++ )
            rocketnet_write_uint16( &p, server_address.data.ipv6[i] );
        rocketnet_write_uint16( &p, server_address.port );
    }

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = client_id;
    randombytes_buf( private_fields.client_to_server_key, ROCKETNET_KEY_BYTES );
    randombytes_buf( private_fields.server_to_client_key, ROCKETNET_KEY_BYTES );

    int result = rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_dev_backend_secret_key, connect_token );

    sodium_memzero( &private_fields, sizeof(private_fields) );

    return result;
}
#endif // #if ROCKETNET_DEVELOPMENT

