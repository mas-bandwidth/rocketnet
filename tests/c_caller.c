/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Calls into the public API from C, for the unit suite. A C caller may hand
// any int where the API takes an enum. Keep the C entry points covered even
// where the C++ enum has a fixed int underlying type.

#include "rocketnet.h"

void rocketnet_test_c_caller_log_level( int level );
const char * rocketnet_test_c_caller_log_level_name( int level );
const char * rocketnet_test_c_caller_status_name( int status );
const char * rocketnet_test_c_caller_client_state_name( int state );
const char * rocketnet_test_c_caller_disconnect_reason_name( int reason );
int rocketnet_test_c_caller_max_unfragmented_message_bytes( int reliability, int channel );
uint8_t * rocketnet_test_c_caller_client_start_message( struct rocketnet_client_t * client, int channel, int reliability, int max_bytes );
uint8_t * rocketnet_test_c_caller_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, int reliability, int max_bytes );
bool rocketnet_test_c_caller_client_can_send_message( const struct rocketnet_client_t * client, int channel, int reliability, int max_bytes );
bool rocketnet_test_c_caller_server_can_send_message( const struct rocketnet_server_t * server, int client_index, int channel, int reliability, int max_bytes );

void rocketnet_test_c_caller_log_level( int level )
{
    rocketnet_log_level( (enum rocketnet_log_level_t) level );
}

const char * rocketnet_test_c_caller_log_level_name( int level )
{
    return rocketnet_log_level_name( (enum rocketnet_log_level_t) level );
}

const char * rocketnet_test_c_caller_status_name( int status )
{
    return rocketnet_status_name( (enum rocketnet_status_t) status );
}

const char * rocketnet_test_c_caller_client_state_name( int state )
{
    return rocketnet_client_state_name( (enum rocketnet_client_state_t) state );
}

const char * rocketnet_test_c_caller_disconnect_reason_name( int reason )
{
    return rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) reason );
}

int rocketnet_test_c_caller_max_unfragmented_message_bytes( int reliability, int channel )
{
    return rocketnet_max_unfragmented_message_bytes( (enum rocketnet_reliability_t) reliability, channel );
}

uint8_t * rocketnet_test_c_caller_client_start_message( struct rocketnet_client_t * client, int channel, int reliability, int max_bytes )
{
    return rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) reliability, max_bytes );
}

uint8_t * rocketnet_test_c_caller_server_start_message( struct rocketnet_server_t * server, uint64_t destination_id, int channel, int reliability, int max_bytes )
{
    return rocketnet_server_start_message( server, destination_id, channel, (enum rocketnet_reliability_t) reliability, max_bytes );
}

bool rocketnet_test_c_caller_client_can_send_message( const struct rocketnet_client_t * client, int channel, int reliability, int max_bytes )
{
    return rocketnet_client_can_send_message( client, channel, (enum rocketnet_reliability_t) reliability, max_bytes );
}

bool rocketnet_test_c_caller_server_can_send_message( const struct rocketnet_server_t * server, int client_index, int channel, int reliability, int max_bytes )
{
    return rocketnet_server_can_send_message( server, client_index, channel, (enum rocketnet_reliability_t) reliability, max_bytes );
}
