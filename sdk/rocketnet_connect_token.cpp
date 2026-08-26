/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"
#include "rocketnet_hydrogen.h"
#include <memory.h>

bool rocketnet_write_connect_token( rocketnet_connect_token_t * token, char * output, const uint8_t * private_key )
{
    rocketnet_assert( token );
    rocketnet_assert( output );
    rocketnet_assert( private_key );

    char context[hydro_sign_CONTEXTBYTES];
    memset( context, 0, sizeof(context) );
    int result = hydro_sign_create(&token->signature[0], (uint8_t*) token, sizeof(rocketnet_connect_token_t) - sizeof(token->signature), context, private_key );
    if ( result != 0 )
    {
        output[0] = '\0';
        return false;
    }

    int bytes = rocketnet_base64_encode_data( (uint8_t*) token, sizeof(rocketnet_connect_token_t), output, ROCKETNET_MAX_CONNECT_TOKEN_BYTES );
    if ( bytes <= 0 )
    {
        output[0] = '\0';
        return false;
    }

    return true;
}

bool rocketnet_read_connect_token( rocketnet_connect_token_t * token, const char * input, const uint8_t * public_key )
{
    rocketnet_assert( token );
    rocketnet_assert( input );
    rocketnet_assert( public_key );

    if ( rocketnet_base64_decode_data( input, (uint8_t*) token, sizeof(rocketnet_connect_token_t) ) != sizeof(rocketnet_connect_token_t) )
    {
        return false;
    }

    char context[hydro_sign_CONTEXTBYTES];
    memset( context, 0, sizeof(context) );
    int result = hydro_sign_verify( &token->signature[0], (uint8_t*) token, sizeof(rocketnet_connect_token_t) - sizeof(token->signature), context, public_key );
    if ( result != 0 )
    {
        return false;
    }
    
    return true;
}

void rocketnet_endian_fix( rocketnet_connect_token_t * token )
{
    rocketnet_assert( token );
    rocketnet_endian_fix( &token->version );
    rocketnet_endian_fix( &token->expire_timestamp );
    rocketnet_endian_fix( &token->buyer_id );
    rocketnet_endian_fix( &token->server_id );
    rocketnet_endian_fix( &token->rocketnet_id );
    rocketnet_endian_fix( &token->user_hash );
}
