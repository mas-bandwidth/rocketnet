/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_connect_token.h"
#include "rocketnet_read_write.h"

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

int rocketnet_write_connect_token( const struct rocketnet_signed_connect_data_t * signed_fields,
                                   const struct rocketnet_connect_token_private_t * private_fields,
                                   const uint8_t * backend_secret_key,
                                   uint8_t * token )
{
    // the inner protocol id and expire are the signed ones, by construction
    struct rocketnet_connect_token_private_t private_data = *private_fields;
    private_data.protocol_id = signed_fields->protocol_id;
    private_data.expire_timestamp = signed_fields->expire_timestamp;

    uint8_t private_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
    rocketnet_write_connect_token_private( &private_data, private_block );

    struct rocketnet_signed_connect_data_t signed_data = *signed_fields;
    if ( crypto_box_seal( signed_data.sealed_private_block, private_block, ROCKETNET_PRIVATE_BLOCK_BYTES, signed_data.server_public_key ) != 0 )
        return 0;

    uint8_t * p = token;
    rocketnet_write_signed_connect_data( &signed_data, p );
    p += ROCKETNET_SIGNED_CONNECT_DATA_BYTES;

    uint8_t signature[ROCKETNET_SIGNATURE_BYTES];
    if ( crypto_sign_detached( signature, NULL, token, ROCKETNET_SIGNED_CONNECT_DATA_BYTES, backend_secret_key ) != 0 )
        return 0;

    rocketnet_write_bytes( &p, signature, ROCKETNET_SIGNATURE_BYTES );
    rocketnet_write_bytes( &p, private_data.client_to_server_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_bytes( &p, private_data.server_to_client_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_zero_bytes( &p, ROCKETNET_CONNECT_TOKEN_BYTES - (int) ( p - token ) );

    return 1;
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
