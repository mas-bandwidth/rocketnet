/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_client_backend_token.h"
#include "rocketnet_hydrogen.h"

static const char * context = "client backend token";

int rocketnet_write_client_backend_token( rocketnet_client_backend_token_t * token, uint8_t * output, const uint8_t * private_key )
{
    rocketnet_assert( token );
    rocketnet_assert( output );
    rocketnet_assert( private_key );
    const int encrypt_result = hydro_secretbox_encrypt( output, (uint8_t*) token + sizeof(token->crypto_header), sizeof(rocketnet_client_backend_token_t) - sizeof(token->crypto_header), 0, context, private_key );
    if ( encrypt_result == 0 )
        return sizeof(rocketnet_client_backend_token_t);
    else
        return 0;
}

bool rocketnet_read_client_backend_token( rocketnet_client_backend_token_t * token, const uint8_t * input, int input_bytes, const uint8_t * private_key )
{
    rocketnet_assert( token );
    rocketnet_assert( input );
    rocketnet_assert( private_key );
    if ( input_bytes != sizeof(rocketnet_client_backend_token_t) )
        return false;
    int decrypt_result = hydro_secretbox_decrypt( (uint8_t*) token + sizeof(token->crypto_header), input, input_bytes, 0, context, private_key );
    return decrypt_result == 0;
}

void rocketnet_endian_fix( rocketnet_client_backend_token_t * token )
{
    rocketnet_assert( token );
    rocketnet_endian_fix( &token->version );
    rocketnet_endian_fix( &token->expire_timestamp );
    rocketnet_endian_fix( &token->buyer_id );
    rocketnet_endian_fix( &token->server_id );
    rocketnet_endian_fix( &token->rocketnet_id );
    rocketnet_endian_fix( &token->user_hash );
}
