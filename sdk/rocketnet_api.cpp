/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public client and server (rocketnet.h) over the SDK's layers: a public
// client is a message client and a public server is a message server, the
// same object under its public name, so every call here is a guard, a
// conversion and one call down. The configs are resolved here: the public
// struct is split into the transport config and the message config the
// layers take, the message fields and the send budget take their environment
// overrides here, and the transport fields take theirs at the transport's
// create, as they always have.

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_counters.h"
#include "rocketnet_message_object.h"
#include "rocketnet_send_budget.h"

#include <limits.h>
#include <string.h>

// ---------------------------------------------------------------
//                     the contracts this file keeps
// ---------------------------------------------------------------

static_assert( ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN == ROCKETNET_CLIENT_TRANSPORT_STATE_INVALID_CONNECT_TOKEN, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_RESPONSE_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_REQUEST_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT == ROCKETNET_CLIENT_TRANSPORT_STATE_COOKIE_REQUEST_TIMED_OUT, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTION_DENIED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_DISCONNECTED == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_RESPONSE, "client state" );
static_assert( ROCKETNET_CLIENT_STATE_CONNECTED == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED, "client state" );

static_assert( sizeof( struct rocketnet_receipt_t ) == sizeof( struct rocketnet_message_receipt_t ) &&
               offsetof( struct rocketnet_receipt_t, serial ) == offsetof( struct rocketnet_message_receipt_t, serial ) &&
               offsetof( struct rocketnet_receipt_t, status ) == offsetof( struct rocketnet_message_receipt_t, status ), "the public receipt is the message receipt" );

static_assert( ROCKETNET_DEFAULT_CLIENT_SEND_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS && ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS <= ROCKETNET_SEND_BUDGET_MAX_KBPS, "the default budgets are valid budgets" );

uint32_t rocketnet_version( void )
{
    return ROCKETNET_VERSION;
}

// ---------------------------------------------------------------
//                          the config split
// ---------------------------------------------------------------

// every message field, one row each, in the order of rocketnet_message_config_t

#define RN_PUBLIC_MESSAGE_FIELDS( X ) \
    X( num_channels ) X( window_messages ) X( receive_ring_bytes ) X( max_message_bytes ) X( send_buffer_bytes ) \
    X( send_queue_messages ) X( packets_in_flight ) X( max_packets_per_update ) X( unreliable_deliveries )      \
    X( sequenced_holds ) X( receipt_ring ) X( rto_initial_ms ) X( rto_min_ms ) X( rto_max_ms ) X( reliable_timeout_ms )

// the send budget fields: one int each, read under the endpoint's env name.
// 0 or less removes the budget; the ceiling is the budget's own.

static const struct rocketnet_config_field_t rn_client_budget_fields[] =
{
    { "send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 },
};

static const struct rocketnet_config_field_t rn_server_budget_fields[] =
{
    { "per_client_send_kbps", ROCKETNET_CONFIG_KIND_INT, 0, (int) sizeof(int), (int64_t) INT_MIN, (int64_t) ROCKETNET_SEND_BUDGET_MAX_KBPS, 0 },
};

const struct rocketnet_config_field_t * rocketnet_client_budget_config_fields( int * num_fields )
{
    *num_fields = 1;
    return rn_client_budget_fields;
}

const struct rocketnet_config_field_t * rocketnet_server_budget_config_fields( int * num_fields )
{
    *num_fields = 1;
    return rn_server_budget_fields;
}

static uint32_t rn_budget_kbps( int kbps )
{
    return kbps <= 0 ? (uint32_t) ROCKETNET_SEND_BUDGET_MAX_KBPS : (uint32_t) kbps;
}

// a NULL or empty string is the default; a longer one than the layer holds
// is refused, loudly, never cut

static bool rn_copy_config_string( char * dest, size_t dest_size, const char * source, const char * fallback, const char * endpoint, const char * field )
{
    if ( !source || source[0] == '\0' )
        source = fallback;
    const size_t length = strlen( source );
    if ( length + 1 > dest_size )
    {
        rocketnet_error( "%s config refused: %s is %d characters, the most is %d", endpoint, field, (int) length, (int) dest_size - 1 );
        return false;
    }
    memcpy( dest, source, length + 1 );
    return true;
}

static bool rn_apply_message_and_budget_env( const char * endpoint, struct rocketnet_message_config_t * message, int * kbps, const struct rocketnet_config_field_t * budget_fields )
{
    int num_message_fields = 0;
    const struct rocketnet_config_field_t * message_fields = rocketnet_message_config_fields( &num_message_fields );
    if ( !rocketnet_config_apply_env( endpoint, message, message_fields, num_message_fields ) )
        return false;
    if ( !rocketnet_config_apply_env( endpoint, kbps, budget_fields, 1 ) )
        return false;
    return rocketnet_config_validate( endpoint, kbps, budget_fields, 1 );
}

static bool rn_client_split( const struct rocketnet_client_config_t * config, bool read_env,
                             struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    struct rocketnet_client_config_t defaults;
    if ( !config )
    {
        rocketnet_client_default_config( &defaults );
        config = &defaults;
    }

    rocketnet_client_transport_default_config_current( transport );
    memcpy( transport->filter_value, config->filter_value, sizeof( transport->filter_value ) );
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->num_reassembly_slots = config->num_reassembly_slots;
    if ( !rn_copy_config_string( transport->driver, sizeof( transport->driver ), config->transport, "auto", "client", "transport" ) )
        return false;
    transport->state_change_callback = config->state_change_callback;
    transport->time_callback = config->time_callback;
    transport->simulated_loss_percent = config->simulated_loss_percent;
    transport->simulated_latency_ms = config->simulated_latency_ms;
    transport->simulated_jitter_ms = config->simulated_jitter_ms;
    transport->simulated_duplicate_percent = config->simulated_duplicate_percent;

    rocketnet_message_default_config( message );
#define RN_COPY( name ) message->name = config->name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY

    int send_kbps = config->send_kbps;
    if ( read_env && !rn_apply_message_and_budget_env( "client", message, &send_kbps, rn_client_budget_fields ) )
        return false;
    if ( !read_env && !rocketnet_config_validate( "client", &send_kbps, rn_client_budget_fields, 1 ) )
        return false;
    *kbps = rn_budget_kbps( send_kbps );
    return true;
}

static bool rn_server_split( const struct rocketnet_server_config_t * config, bool read_env,
                             struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    struct rocketnet_server_config_t defaults;
    if ( !config )
    {
        rocketnet_server_default_config( &defaults );
        config = &defaults;
    }

    rocketnet_server_transport_default_config_current( transport );
    transport->protocol_id = config->protocol_id;
    transport->max_clients = config->max_clients;
    memcpy( transport->filter_value, config->filter_value, sizeof( transport->filter_value ) );
    memcpy( transport->backend_public_key, config->backend_public_key, sizeof( transport->backend_public_key ) );
    memcpy( transport->server_public_key, config->server_public_key, sizeof( transport->server_public_key ) );
    memcpy( transport->server_secret_key, config->server_secret_key, sizeof( transport->server_secret_key ) );
    transport->num_xdp_queues = config->num_xdp_queues;
    transport->num_os_cpus = config->num_os_cpus;
    transport->max_payload_bytes = config->max_payload_bytes;
    transport->num_reassembly_slots = config->num_reassembly_slots;
    transport->max_timeout_seconds = config->max_timeout_seconds;
    transport->handshake_deadline_seconds = config->handshake_deadline_seconds;
    transport->simulated_loss_percent = config->simulated_loss_percent;
    transport->simulated_latency_ms = config->simulated_latency_ms;
    transport->simulated_jitter_ms = config->simulated_jitter_ms;
    transport->simulated_duplicate_percent = config->simulated_duplicate_percent;
    if ( !rn_copy_config_string( transport->driver, sizeof( transport->driver ), config->transport, "auto", "server", "transport" ) )
        return false;
    if ( !rn_copy_config_string( transport->public_address, sizeof( transport->public_address ), config->public_address, "", "server", "public_address" ) )
        return false;

    rocketnet_message_default_config( message );
#define RN_COPY( name ) message->name = config->name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY

    // max_clients sizes the message server as well as the transport, so its
    // env override applies here, before either is made: the transport's
    // create reads the same variable again and agrees. every other transport
    // field takes its override at the transport's create, as before.
    if ( read_env )
    {
        int num_fields = 0;
        const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
        for ( int i = 0; i < num_fields; i++ )
            if ( strcmp( fields[i].name, "max_clients" ) == 0 && !rocketnet_config_apply_env( "server", transport, &fields[i], 1 ) )
                return false;
    }

    int send_kbps = config->per_client_send_kbps;
    if ( read_env && !rn_apply_message_and_budget_env( "server", message, &send_kbps, rn_server_budget_fields ) )
        return false;
    if ( !read_env && !rocketnet_config_validate( "server", &send_kbps, rn_server_budget_fields, 1 ) )
        return false;
    *kbps = rn_budget_kbps( send_kbps );
    return true;
}

bool rocketnet_client_config_resolve( const struct rocketnet_client_config_t * config, bool read_env,
                                     struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    return rn_client_split( config, read_env, transport, message, kbps );
}

bool rocketnet_server_config_resolve( const struct rocketnet_server_config_t * config, bool read_env,
                                     struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps )
{
    return rn_server_split( config, read_env, transport, message, kbps );
}

// ---------------------------------------------------------------
//                              client
// ---------------------------------------------------------------

static inline struct rocketnet_message_client_t * rn_client( const struct rocketnet_client_t * client )
{
    return (struct rocketnet_message_client_t *) client;
}

void rocketnet_client_default_config( struct rocketnet_client_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    memset( config, 0, sizeof( *config ) );
    struct rocketnet_client_transport_config_t transport;
    rocketnet_client_transport_default_config_current( &transport );
    memcpy( config->filter_value, transport.filter_value, sizeof( config->filter_value ) );
    config->max_payload_bytes = transport.max_payload_bytes;
    config->num_reassembly_slots = transport.num_reassembly_slots;
    config->transport = NULL;
    config->state_change_callback = transport.state_change_callback;
    config->time_callback = transport.time_callback;
    config->simulated_loss_percent = transport.simulated_loss_percent;
    config->simulated_latency_ms = transport.simulated_latency_ms;
    config->simulated_jitter_ms = transport.simulated_jitter_ms;
    config->simulated_duplicate_percent = transport.simulated_duplicate_percent;
    config->send_kbps = ROCKETNET_DEFAULT_CLIENT_SEND_KBPS;
    struct rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
#define RN_COPY( name ) config->name = message.name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY
}

static size_t rn_client_config_bytes_1()
{
    return offsetof( struct rocketnet_client_config_t, simulated_loss_percent );
}

static size_t rn_server_config_bytes_1()
{
    return offsetof( struct rocketnet_server_config_t, simulated_loss_percent );
}

static bool rn_public_config_size_known( size_t bytes, size_t revision_1, size_t current )
{
    return bytes == revision_1 || bytes == current;
}

int rocketnet_client_default_config_sized( struct rocketnet_client_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;
    if ( !rn_public_config_size_known( config_bytes, rn_client_config_bytes_1(), sizeof( struct rocketnet_client_config_t ) ) )
    {
        rocketnet_error( "client default config refused: %d is not a known size of rocketnet_client_config_t (%d)", (int) config_bytes, (int) sizeof( struct rocketnet_client_config_t ) );
        return 0;
    }
    struct rocketnet_client_config_t defaults;
    rocketnet_client_default_config( &defaults );
    memcpy( config, &defaults, config_bytes );
    return 1;
}

int rocketnet_client_validate_config( const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, false, &transport, &message, &kbps ) )
        return 0;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "client", &transport, fields, num_fields ) )
        return 0;
    return rocketnet_message_validate_config( &message );
}

uint64_t rocketnet_client_required_memory( const struct rocketnet_client_config_t * config )
{
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, true, &transport, &message, &kbps ) )
        return 0;
    return rocketnet_message_client_required_memory( &message );
}

struct rocketnet_client_t * rocketnet_client_create( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, void * memory, uint64_t memory_bytes )
{
    if ( !rocketnet_api_null_guard( connect_token ) )
        return NULL;
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_client_split( config, true, &transport, &message, &kbps ) )
        return NULL;
    return (struct rocketnet_client_t *) rocketnet_message_client_create( context, connect_token, &transport, &message, memory, memory_bytes, kbps );
}

struct rocketnet_client_t * rocketnet_client_create_sized( void * context, const uint8_t * connect_token, const struct rocketnet_client_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes )
{
    if ( config && !rn_public_config_size_known( config_bytes, rn_client_config_bytes_1(), sizeof( struct rocketnet_client_config_t ) ) )
    {
        rocketnet_error( "client create refused: %d is not a known size of rocketnet_client_config_t (%d)", (int) config_bytes, (int) sizeof( struct rocketnet_client_config_t ) );
        return NULL;
    }
    if ( !config || config_bytes == sizeof( struct rocketnet_client_config_t ) )
        return rocketnet_client_create( context, connect_token, config, memory, memory_bytes );
    struct rocketnet_client_config_t current;
    rocketnet_client_default_config( &current );
    memcpy( &current, config, config_bytes );
    return rocketnet_client_create( context, connect_token, &current, memory, memory_bytes );
}

void rocketnet_client_destroy( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rocketnet_message_client_destroy( rn_client( client ) );
}

int rocketnet_client_update( struct rocketnet_client_t * client, double time )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_MESSAGE_OK;
    return rocketnet_message_client_update( rn_client( client ), time );
}

void rocketnet_client_send_packets( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rocketnet_message_client_send_packets( rn_client( client ) );
}

void rocketnet_client_disconnect( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rocketnet_client_transport_disconnect( rocketnet_message_client_transport( rn_client( client ) ) );
}

enum rocketnet_client_state_t rocketnet_client_state( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_CLIENT_STATE_DISCONNECTED;
    return (enum rocketnet_client_state_t) rocketnet_message_client_state( rn_client( client ) );
}

const char * rocketnet_client_state_name( int state )
{
    return rocketnet_client_transport_state_name( state );
}

int rocketnet_client_disconnect_reason( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    return rocketnet_message_client_disconnect_reason( rn_client( client ) );
}

int rocketnet_client_index( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rocketnet_client_transport_client_index( rocketnet_message_client_transport( rn_client( client ) ) );
}

const char * rocketnet_client_transport_name( const struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    return rocketnet_client_transport_driver( rocketnet_message_client_transport( rn_client( client ) ) );
}

uint8_t * rocketnet_client_start( struct rocketnet_client_t * client, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( client ) )
        return NULL;
    return rocketnet_message_client_start( rn_client( client ), channel, reliability, bytes );
}

uint32_t rocketnet_client_finish( struct rocketnet_client_t * client, int bytes_written )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rocketnet_message_client_finish( rn_client( client ), bytes_written );
}

void rocketnet_client_abort( struct rocketnet_client_t * client )
{
    if ( !rocketnet_api_null_guard( client ) )
        return;
    rocketnet_message_client_abort( rn_client( client ) );
}

int rocketnet_client_receive( struct rocketnet_client_t * client, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( data ) || !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rocketnet_message_client_receive( rn_client( client ), channel, data, bytes );
}

void rocketnet_client_release( struct rocketnet_client_t * client, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( client ) || !rocketnet_api_null_guard( data ) )
        return;
    rocketnet_message_client_release( rn_client( client ), data );
}

int rocketnet_client_receipts( struct rocketnet_client_t * client, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rocketnet_message_client_receipts( rn_client( client ), (struct rocketnet_message_receipt_t *) out, capacity );
}

int rocketnet_client_set_send_kbps( struct rocketnet_client_t * client, int kbps )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    return rocketnet_message_client_set_send_kbps( rn_client( client ), rn_budget_kbps( kbps ) );
}

static uint64_t rn_counter_sum( const struct rocketnet_counters_t * a, const struct rocketnet_counters_t * b, int counter )
{
    return ( a ? rocketnet_counter_value( a, counter ) : 0 ) + ( b ? rocketnet_counter_value( b, counter ) : 0 );
}

uint64_t rocketnet_client_counter( const struct rocketnet_client_t * client, enum rocketnet_client_counter_t counter )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0;
    struct rocketnet_message_client_t * message_client = rn_client( client );
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message_client );
    const struct rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_client_counters( message_client );
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( transport, &stats );
    switch ( counter )
    {
        case ROCKETNET_CLIENT_COUNTER_PACKETS_SENT:         return stats.packets_sent;
        case ROCKETNET_CLIENT_COUNTER_PACKETS_RECEIVED:     return stats.packets_received;
        case ROCKETNET_CLIENT_COUNTER_BYTES_SENT:           return stats.bytes_sent;
        case ROCKETNET_CLIENT_COUNTER_BYTES_RECEIVED:       return stats.bytes_received;
        case ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED:    return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_DELIVERED );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_RESENT:      return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_DUPLICATE:   return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        case ROCKETNET_CLIENT_COUNTER_RECEIPTS_ACKED:       return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
        case ROCKETNET_CLIENT_COUNTER_RECEIPTS_LOST:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
        case ROCKETNET_CLIENT_COUNTER_BUDGET_DROPS:         return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        case ROCKETNET_CLIENT_COUNTER_SEND_DEFERRED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_SEND_DEFERRED );
        case ROCKETNET_CLIENT_COUNTER_MESSAGES_NOT_RELEASED: return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
        default:                                            return 0;
    }
}

double rocketnet_client_stat( const struct rocketnet_client_t * client, enum rocketnet_client_stat_t stat )
{
    if ( !rocketnet_api_null_guard( client ) )
        return 0.0;
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( rocketnet_message_client_transport( rn_client( client ) ), &stats );
    switch ( stat )
    {
        case ROCKETNET_CLIENT_STAT_RTT:                         return stats.rtt;
        case ROCKETNET_CLIENT_STAT_JITTER:                      return stats.jitter;
        case ROCKETNET_CLIENT_STAT_PACKET_LOSS:                 return stats.packet_loss;
        case ROCKETNET_CLIENT_STAT_PACKETS_SENT_PER_SECOND:     return stats.packets_sent_per_second;
        case ROCKETNET_CLIENT_STAT_PACKETS_RECEIVED_PER_SECOND: return stats.packets_received_per_second;
        case ROCKETNET_CLIENT_STAT_BANDWIDTH_SENT:              return stats.bandwidth_sent_kbps;
        case ROCKETNET_CLIENT_STAT_BANDWIDTH_RECEIVED:          return stats.bandwidth_received_kbps;
        default:                                                return 0.0;
    }
}

// ---------------------------------------------------------------
//                              server
// ---------------------------------------------------------------

static inline struct rocketnet_message_server_t * rn_server( const struct rocketnet_server_t * server )
{
    return (struct rocketnet_message_server_t *) server;
}

static inline struct rocketnet_server_transport_t * rn_server_transport( const struct rocketnet_server_t * server )
{
    return rocketnet_message_server_transport( rn_server( server ) );
}

static void rn_server_config_from_transport( struct rocketnet_server_config_t * config, const struct rocketnet_server_transport_config_t * transport )
{
    memset( config, 0, sizeof( *config ) );
    config->protocol_id = transport->protocol_id;
    config->max_clients = transport->max_clients;
    memcpy( config->filter_value, transport->filter_value, sizeof( config->filter_value ) );
    memcpy( config->backend_public_key, transport->backend_public_key, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, transport->server_public_key, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, transport->server_secret_key, sizeof( config->server_secret_key ) );
    config->num_xdp_queues = transport->num_xdp_queues;
    config->num_os_cpus = transport->num_os_cpus;
    config->max_payload_bytes = transport->max_payload_bytes;
    config->num_reassembly_slots = transport->num_reassembly_slots;
    config->max_timeout_seconds = transport->max_timeout_seconds;
    config->handshake_deadline_seconds = transport->handshake_deadline_seconds;
    config->simulated_loss_percent = transport->simulated_loss_percent;
    config->simulated_latency_ms = transport->simulated_latency_ms;
    config->simulated_jitter_ms = transport->simulated_jitter_ms;
    config->simulated_duplicate_percent = transport->simulated_duplicate_percent;
    config->transport = NULL;
    config->public_address = NULL;
    config->per_client_send_kbps = ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS;
    struct rocketnet_message_config_t message;
    rocketnet_message_default_config( &message );
#define RN_COPY( name ) config->name = message.name;
    RN_PUBLIC_MESSAGE_FIELDS( RN_COPY )
#undef RN_COPY
}

void rocketnet_server_default_config( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config_current( &transport );
    rn_server_config_from_transport( config, &transport );
}

int rocketnet_server_default_config_sized( struct rocketnet_server_config_t * config, size_t config_bytes )
{
    if ( !rocketnet_api_null_guard( config ) )
        return 0;
    if ( !rn_public_config_size_known( config_bytes, rn_server_config_bytes_1(), sizeof( struct rocketnet_server_config_t ) ) )
    {
        rocketnet_error( "server default config refused: %d is not a known size of rocketnet_server_config_t (%d)", (int) config_bytes, (int) sizeof( struct rocketnet_server_config_t ) );
        return 0;
    }
    struct rocketnet_server_config_t defaults;
    rocketnet_server_default_config( &defaults );
    memcpy( config, &defaults, config_bytes );
    return 1;
}

void rocketnet_server_dev_config( struct rocketnet_server_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_transport_config_t transport;
    memset( &transport, 0, sizeof(transport) );
    rocketnet_server_transport_dev_config( &transport );
    rn_server_config_from_transport( config, &transport );
}

int rocketnet_server_validate_config( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, false, &transport, &message, &kbps ) )
        return 0;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "server", &transport, fields, num_fields ) )
        return 0;
    return rocketnet_message_validate_config( &message );
}

uint64_t rocketnet_server_required_memory( const struct rocketnet_server_config_t * config )
{
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, true, &transport, &message, &kbps ) )
        return 0;
    return rocketnet_message_server_required_memory( &transport, &message );
}

struct rocketnet_server_t * rocketnet_server_create( void * context, const char * address, const struct rocketnet_server_config_t * config, void * memory, uint64_t memory_bytes )
{
    if ( !rocketnet_api_null_guard( address ) )
        return NULL;
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rn_server_split( config, true, &transport, &message, &kbps ) )
        return NULL;
    return (struct rocketnet_server_t *) rocketnet_message_server_create( context, address, &transport, &message, memory, memory_bytes, kbps );
}

struct rocketnet_server_t * rocketnet_server_create_sized( void * context, const char * address, const struct rocketnet_server_config_t * config, size_t config_bytes, void * memory, uint64_t memory_bytes )
{
    if ( config && !rn_public_config_size_known( config_bytes, rn_server_config_bytes_1(), sizeof( struct rocketnet_server_config_t ) ) )
    {
        rocketnet_error( "server create refused: %d is not a known size of rocketnet_server_config_t (%d)", (int) config_bytes, (int) sizeof( struct rocketnet_server_config_t ) );
        return NULL;
    }
    if ( !config || config_bytes == sizeof( struct rocketnet_server_config_t ) )
        return rocketnet_server_create( context, address, config, memory, memory_bytes );
    struct rocketnet_server_config_t current;
    rocketnet_server_default_config( &current );
    memcpy( &current, config, config_bytes );
    return rocketnet_server_create( context, address, &current, memory, memory_bytes );
}

void rocketnet_server_destroy( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rocketnet_message_server_destroy( rn_server( server ) );
}

int rocketnet_server_update( struct rocketnet_server_t * server, double time )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_MESSAGE_OK;
    return rocketnet_message_server_update( rn_server( server ), time );
}

void rocketnet_server_send_packets( struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rocketnet_message_server_send_packets( rn_server( server ) );
}

uint16_t rocketnet_server_port( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_server_transport_port( rn_server_transport( server ) );
}

const char * rocketnet_server_transport_name( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    return rocketnet_server_transport_driver( rn_server_transport( server ) );
}

int rocketnet_server_max_clients( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return ( (const struct rn_message_object_t *) server )->num_connections;
}

int rocketnet_server_num_connected_clients( const struct rocketnet_server_t * server )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_server_transport_num_connected_clients( rn_server_transport( server ) );
}

static bool rn_server_slot_in_range( const struct rocketnet_server_t * server, int client_index )
{
    return client_index >= 0 && client_index < ( (const struct rn_message_object_t *) server )->num_connections;
}

int rocketnet_server_client_connected( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_message_server_client_connected( rn_server( server ), client_index );
}

uint64_t rocketnet_server_client_id( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    if ( !rn_server_slot_in_range( server, client_index ) )
        return 0;
    return rocketnet_server_transport_client_id( rn_server_transport( server ), client_index );
}

void rocketnet_server_disconnect_client( struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    if ( !rn_server_slot_in_range( server, client_index ) )
        return;
    rocketnet_server_transport_disconnect_client( rn_server_transport( server ), client_index );
}

int rocketnet_server_disconnect_reason( const struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    return rocketnet_message_server_disconnect_reason( rn_server( server ), client_index );
}

uint8_t * rocketnet_server_start( struct rocketnet_server_t * server, int client_index, int channel, int reliability, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) )
        return NULL;
    return rocketnet_message_server_start( rn_server( server ), client_index, channel, reliability, bytes );
}

uint32_t rocketnet_server_finish( struct rocketnet_server_t * server, int client_index, int bytes_written )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_message_server_finish( rn_server( server ), client_index, bytes_written );
}

void rocketnet_server_abort( struct rocketnet_server_t * server, int client_index )
{
    if ( !rocketnet_api_null_guard( server ) )
        return;
    rocketnet_message_server_abort( rn_server( server ), client_index );
}

int rocketnet_server_broadcast( struct rocketnet_server_t * server, int channel, int reliability, const uint8_t * data, int bytes )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( data ) )
        return 0;
    return rocketnet_message_server_broadcast( rn_server( server ), channel, reliability, data, bytes );
}

int rocketnet_server_receive( struct rocketnet_server_t * server, int client_index, int channel, const uint8_t ** data, int * bytes )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( data ) || !rocketnet_api_null_guard( bytes ) )
        return 0;
    return rocketnet_message_server_receive( rn_server( server ), client_index, channel, data, bytes );
}

void rocketnet_server_release( struct rocketnet_server_t * server, int client_index, const uint8_t * data )
{
    if ( !rocketnet_api_null_guard( server ) || !rocketnet_api_null_guard( data ) )
        return;
    rocketnet_message_server_release( rn_server( server ), client_index, data );
}

int rocketnet_server_receipts( struct rocketnet_server_t * server, int client_index, struct rocketnet_receipt_t * out, int capacity )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_message_server_receipts( rn_server( server ), client_index, (struct rocketnet_message_receipt_t *) out, capacity );
}

int rocketnet_server_set_send_kbps( struct rocketnet_server_t * server, int client_index, int kbps )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    return rocketnet_message_server_set_send_kbps( rn_server( server ), client_index, rn_budget_kbps( kbps ) );
}

uint64_t rocketnet_server_counter( const struct rocketnet_server_t * server, enum rocketnet_server_counter_t counter )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    const struct rocketnet_counters_t * transport_counters = rocketnet_server_transport_counters( transport );
    const struct rocketnet_counters_t * message_counters = rocketnet_message_server_counters( rn_server( server ) );
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( transport, &stats );
    switch ( counter )
    {
        case ROCKETNET_SERVER_COUNTER_PACKETS_SENT:             return stats.packets_sent;
        case ROCKETNET_SERVER_COUNTER_PACKETS_RECEIVED:         return stats.packets_received;
        case ROCKETNET_SERVER_COUNTER_BYTES_SENT:               return stats.bytes_sent;
        case ROCKETNET_SERVER_COUNTER_BYTES_RECEIVED:           return stats.bytes_received;
        case ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CLIENT_CONNECTED );
        case ROCKETNET_SERVER_COUNTER_CLIENTS_DENIED_FULL:      return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS ) +
                                                                       rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS );
        case ROCKETNET_SERVER_COUNTER_HANDSHAKES_OVER_BUDGET:   return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_CONNECTION_REQUEST_HANDSHAKE_BUDGET_EXCEEDED );
        case ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED:        return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_PAYLOAD_DELIVERED );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_RESENT:          return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_DUPLICATE:       return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        case ROCKETNET_SERVER_COUNTER_RECEIPTS_ACKED:           return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED );
        case ROCKETNET_SERVER_COUNTER_RECEIPTS_LOST:            return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
        case ROCKETNET_SERVER_COUNTER_BUDGET_DROPS:             return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        case ROCKETNET_SERVER_COUNTER_MESSAGES_NOT_RELEASED:    return rn_counter_sum( transport_counters, message_counters, ROCKETNET_COUNTER_MESSAGE_NOT_RELEASED );
        default:                                                return 0;
    }
}

double rocketnet_server_stat( const struct rocketnet_server_t * server, enum rocketnet_server_stat_t stat )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0.0;
    struct rocketnet_server_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_server_transport_stats( rn_server_transport( server ), &stats );
    switch ( stat )
    {
        case ROCKETNET_SERVER_STAT_PACKETS_SENT_PER_SECOND:     return (double) stats.packets_sent_per_second;
        case ROCKETNET_SERVER_STAT_PACKETS_RECEIVED_PER_SECOND: return (double) stats.packets_received_per_second;
        case ROCKETNET_SERVER_STAT_BANDWIDTH_SENT:              return stats.bandwidth_sent_kbps;
        case ROCKETNET_SERVER_STAT_BANDWIDTH_RECEIVED:          return stats.bandwidth_received_kbps;
        default:                                                return 0.0;
    }
}

static bool rn_server_client_stats( const struct rocketnet_server_t * server, int client_index, struct rocketnet_server_transport_client_stats_t * stats )
{
    memset( stats, 0, sizeof( *stats ) );
    if ( !rn_server_slot_in_range( server, client_index ) )
        return false;
    struct rocketnet_server_transport_t * transport = rn_server_transport( server );
    if ( !rocketnet_server_transport_is_client_connected( transport, client_index ) )
        return false;
    rocketnet_server_transport_client_stats( transport, client_index, stats );
    return true;
}

uint64_t rocketnet_server_client_counter( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_counter_t counter )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0;
    struct rocketnet_server_transport_client_stats_t stats;
    if ( !rn_server_client_stats( server, client_index, &stats ) )
        return 0;
    switch ( counter )
    {
        case ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_SENT:      return stats.packets_sent;
        case ROCKETNET_SERVER_CLIENT_COUNTER_PACKETS_RECEIVED:  return stats.packets_received;
        case ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_SENT:        return stats.bytes_sent;
        case ROCKETNET_SERVER_CLIENT_COUNTER_BYTES_RECEIVED:    return stats.bytes_received;
        default:                                                return 0;
    }
}

double rocketnet_server_client_stat( const struct rocketnet_server_t * server, int client_index, enum rocketnet_server_client_stat_t stat )
{
    if ( !rocketnet_api_null_guard( server ) )
        return 0.0;
    struct rocketnet_server_transport_client_stats_t stats;
    if ( !rn_server_client_stats( server, client_index, &stats ) )
        return 0.0;
    switch ( stat )
    {
        case ROCKETNET_SERVER_CLIENT_STAT_RTT:                  return stats.rtt;
        case ROCKETNET_SERVER_CLIENT_STAT_JITTER:               return stats.jitter;
        case ROCKETNET_SERVER_CLIENT_STAT_PACKET_LOSS:          return stats.packet_loss;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_SENT:       return stats.bandwidth_sent_kbps;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_RECEIVED:   return stats.bandwidth_received_kbps;
        case ROCKETNET_SERVER_CLIENT_STAT_BANDWIDTH_ACKED:      return stats.bandwidth_acked_kbps;
        default:                                                return 0.0;
    }
}
