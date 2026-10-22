/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

/* Keep this a C translation unit: client.c and soak.c need these symbols
   from a C++ library. No clock, sockets, randomness or library startup. */
#include "rocketnet_connect_token.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "internal C linkage: %s at %d\n", #condition, __LINE__); return 1; } } while (0)

int main( void )
{
    struct rocketnet_connect_token_private_t input = {0};
    struct rocketnet_connect_token_private_t output = {0};
    uint8_t private_bytes[ROCKETNET_PRIVATE_BLOCK_BYTES];
    input.client_id = 42;
    input.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    input.expire_timestamp = 1030;
    input.user_data[ROCKETNET_USER_DATA_BYTES - 1] = 123;
    rocketnet_write_connect_token_private( &input, private_bytes );
    rocketnet_read_connect_token_private( &output, private_bytes );
    CHECK( output.client_id == input.client_id );
    CHECK( output.protocol_id == input.protocol_id );
    CHECK( output.expire_timestamp == input.expire_timestamp );
    CHECK( memcmp( output.user_data, input.user_data, sizeof(input.user_data) ) == 0 );

    struct rocketnet_signed_connect_data_t signed_input = {0};
    struct rocketnet_signed_connect_data_t signed_output = {0};
    uint8_t signed_bytes[ROCKETNET_SIGNED_CONNECT_DATA_BYTES];
    signed_input.version = ROCKETNET_WIRE_VERSION;
    signed_input.create_timestamp = 1000;
    signed_input.expire_timestamp = 1030;
    memcpy( signed_input.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES );
    rocketnet_write_signed_connect_data( &signed_input, signed_bytes );
    rocketnet_read_signed_connect_data( &signed_output, signed_bytes );
    CHECK( signed_output.version == ROCKETNET_WIRE_VERSION );
    CHECK( signed_output.create_timestamp == 1000 );
    CHECK( signed_output.expire_timestamp == 1030 );
    CHECK( memcmp( signed_output.server_public_key, rocketnet_dev_server_public_key, ROCKETNET_KEY_BYTES ) == 0 );

    /* The tools' binary writer rejects an invalid address before crypto work. */
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK( rocketnet_write_test_connect_token( "invalid", 42, 1000, token, sizeof(token) ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    puts( "internal C linkage passed" );
    return 0;
}
