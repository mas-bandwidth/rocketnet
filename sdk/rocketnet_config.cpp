/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_fragment.h"
#include "rocketnet_base64.h"
#include "rocketnet_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>

// ----------------------------------------------------------------
//              the tables, expanded into everything derived
// ----------------------------------------------------------------

// the default configs: the DEFAULT layer of default < struct < env, written
// from the tables so a field's default lives in exactly one place

#define ROCKETNET_CONFIG_DEFAULT_INT( name, def, min, max, flags )      config->name = (def);
#define ROCKETNET_CONFIG_DEFAULT_UINT64( name, def, flags )             config->name = (def);
#define ROCKETNET_CONFIG_DEFAULT_BYTES( name, size, def, flags )        { static const uint8_t rocketnet_default_##name[size] = def; memcpy( config->name, rocketnet_default_##name, size ); }
#define ROCKETNET_CONFIG_DEFAULT_STRING( name, capacity, def, flags )   snprintf( config->name, capacity, "%s", def );

void rocketnet_client_transport_default_config_current( struct rocketnet_client_transport_config_t * config )
{
    memset( config, 0, sizeof(*config) );
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_DEFAULT_INT, ROCKETNET_CONFIG_DEFAULT_UINT64, ROCKETNET_CONFIG_DEFAULT_BYTES, ROCKETNET_CONFIG_DEFAULT_STRING )
}

void rocketnet_client_transport_default_config( struct rocketnet_client_transport_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config_current( &defaults );
    memset( config, 0, sizeof(*config) );
    memcpy( config, &defaults, ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR );
}

void rocketnet_server_transport_default_config_current( struct rocketnet_server_transport_config_t * config )
{
    memset( config, 0, sizeof(*config) );
    ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_DEFAULT_INT, ROCKETNET_CONFIG_DEFAULT_UINT64, ROCKETNET_CONFIG_DEFAULT_BYTES, ROCKETNET_CONFIG_DEFAULT_STRING )
}

void rocketnet_server_transport_default_config( struct rocketnet_server_transport_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    struct rocketnet_server_transport_config_t defaults;
    rocketnet_server_transport_default_config_current( &defaults );
    memset( config, 0, sizeof(*config) );
    memcpy( config, &defaults, ROCKETNET_SERVER_TRANSPORT_CONFIG_BYTES_1 );
}

void rocketnet_message_default_config( struct rocketnet_message_config_t * config )
{
    if ( !rocketnet_api_null_guard( config ) )
        return;
    rocketnet_assert( config );
    memset( config, 0, sizeof(struct rocketnet_message_config_t) );
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_DEFAULT_INT, ROCKETNET_CONFIG_DEFAULT_UINT64, ROCKETNET_CONFIG_DEFAULT_BYTES, ROCKETNET_CONFIG_DEFAULT_STRING )
}

// the field descriptors: the walkable form of each table, one entry per
// declared field, offsets taken from the real structs

#define ROCKETNET_CONFIG_DESC_INT( name, def, min, max, flags )         { #name, ROCKETNET_CONFIG_KIND_INT,    (int) offsetof( ROCKETNET_CONFIG_DESC_STRUCT, name ), (int) sizeof(int),      (int64_t) (min), (int64_t) (max), (flags) },
#define ROCKETNET_CONFIG_DESC_UINT64( name, def, flags )                { #name, ROCKETNET_CONFIG_KIND_UINT64, (int) offsetof( ROCKETNET_CONFIG_DESC_STRUCT, name ), (int) sizeof(uint64_t), 0, 0, (flags) },
#define ROCKETNET_CONFIG_DESC_BYTES( name, size, def, flags )           { #name, ROCKETNET_CONFIG_KIND_BYTES,  (int) offsetof( ROCKETNET_CONFIG_DESC_STRUCT, name ), (size), 0, 0, (flags) },
#define ROCKETNET_CONFIG_DESC_STRING( name, capacity, def, flags )      { #name, ROCKETNET_CONFIG_KIND_STRING, (int) offsetof( ROCKETNET_CONFIG_DESC_STRUCT, name ), (capacity), 0, 0, (flags) },

#define ROCKETNET_CONFIG_DESC_STRUCT struct rocketnet_client_transport_config_t

static const struct rocketnet_config_field_t rocketnet_client_config_fields[] =
{
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_DESC_INT, ROCKETNET_CONFIG_DESC_UINT64, ROCKETNET_CONFIG_DESC_BYTES, ROCKETNET_CONFIG_DESC_STRING )
    { "step_driven", ROCKETNET_CONFIG_KIND_UINT64, (int) offsetof( struct rocketnet_client_transport_config_t, step_driven ), (int) sizeof(uint64_t), 0, 0, 0 },
};

#undef ROCKETNET_CONFIG_DESC_STRUCT
#define ROCKETNET_CONFIG_DESC_STRUCT struct rocketnet_server_transport_config_t

static const struct rocketnet_config_field_t rocketnet_server_config_fields[] =
{
    ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_DESC_INT, ROCKETNET_CONFIG_DESC_UINT64, ROCKETNET_CONFIG_DESC_BYTES, ROCKETNET_CONFIG_DESC_STRING )
};

#undef ROCKETNET_CONFIG_DESC_STRUCT
#define ROCKETNET_CONFIG_DESC_STRUCT struct rocketnet_message_config_t

static const struct rocketnet_config_field_t rocketnet_message_config_field_table[] =
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_DESC_INT, ROCKETNET_CONFIG_DESC_UINT64, ROCKETNET_CONFIG_DESC_BYTES, ROCKETNET_CONFIG_DESC_STRING )
};

#undef ROCKETNET_CONFIG_DESC_STRUCT

// the layout check: rocketnet.h writes each config struct out field by field,
// for a reader; the tables are the declarations everything above derives
// from. Each struct is built again from its table here, and every table row
// must name a public field at the same offset with the same size, and the two
// structs must have the same size, so the header and the tables cannot drift.

#define ROCKETNET_CLIENT_CONFIG_STRUCT_UINT64( name, def, flags )                                           \
    void (*state_change_callback)( void * context, int previous_state, int current_state );                 \
    double (*time_callback)( void * context );                                                              \
    uint64_t name;

struct rocketnet_client_transport_config_table_t
{
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V1( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CLIENT_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
    uint64_t step_driven;
    ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE_V2( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

#undef ROCKETNET_CLIENT_CONFIG_STRUCT_UINT64

struct rocketnet_server_transport_config_table_t
{
    ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

struct rocketnet_message_config_table_t
{
    ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_STRUCT_INT, ROCKETNET_CONFIG_STRUCT_UINT64, ROCKETNET_CONFIG_STRUCT_BYTES, ROCKETNET_CONFIG_STRUCT_STRING )
};

#define ROCKETNET_CONFIG_LAYOUT_FIELD( name )                                                                                       \
    static_assert( offsetof( ROCKETNET_CONFIG_LAYOUT_PUBLIC, name ) == offsetof( ROCKETNET_CONFIG_LAYOUT_TABLE, name ) &&            \
                   sizeof( ( (ROCKETNET_CONFIG_LAYOUT_PUBLIC *) 0 )->name ) == sizeof( ( (ROCKETNET_CONFIG_LAYOUT_TABLE *) 0 )->name ), \
                   "the rocketnet.h config field " #name " matches its table row" );
#define ROCKETNET_CONFIG_LAYOUT_INT( name, def, min, max, flags )       ROCKETNET_CONFIG_LAYOUT_FIELD( name )
#define ROCKETNET_CONFIG_LAYOUT_UINT64( name, def, flags )              ROCKETNET_CONFIG_LAYOUT_FIELD( name )
#define ROCKETNET_CONFIG_LAYOUT_BYTES( name, size, def, flags )         ROCKETNET_CONFIG_LAYOUT_FIELD( name )
#define ROCKETNET_CONFIG_LAYOUT_STRING( name, capacity, def, flags )    ROCKETNET_CONFIG_LAYOUT_FIELD( name )

#define ROCKETNET_CONFIG_LAYOUT_PUBLIC struct rocketnet_client_transport_config_t
#define ROCKETNET_CONFIG_LAYOUT_TABLE struct rocketnet_client_transport_config_table_t
ROCKETNET_CLIENT_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_LAYOUT_INT, ROCKETNET_CONFIG_LAYOUT_UINT64, ROCKETNET_CONFIG_LAYOUT_BYTES, ROCKETNET_CONFIG_LAYOUT_STRING )
ROCKETNET_CONFIG_LAYOUT_FIELD( state_change_callback )
ROCKETNET_CONFIG_LAYOUT_FIELD( time_callback )
ROCKETNET_CONFIG_LAYOUT_FIELD( step_driven )
static_assert( offsetof( struct rocketnet_client_transport_config_t, step_driven ) + sizeof( uint64_t ) == ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, "client revision 1 ends at step_driven" );
static_assert( offsetof( struct rocketnet_client_transport_config_t, simulated_loss_percent ) == ROCKETNET_CLIENT_TRANSPORT_CONFIG_BYTES_1_PRE_SIMULATOR, "client revision 2 appends after revision 1" );
static_assert( sizeof( ROCKETNET_CONFIG_LAYOUT_PUBLIC ) == sizeof( ROCKETNET_CONFIG_LAYOUT_TABLE ), "the rocketnet.h client transport config matches its table" );
#undef ROCKETNET_CONFIG_LAYOUT_PUBLIC
#undef ROCKETNET_CONFIG_LAYOUT_TABLE

#define ROCKETNET_CONFIG_LAYOUT_PUBLIC struct rocketnet_server_transport_config_t
#define ROCKETNET_CONFIG_LAYOUT_TABLE struct rocketnet_server_transport_config_table_t
ROCKETNET_SERVER_TRANSPORT_CONFIG_TABLE( ROCKETNET_CONFIG_LAYOUT_INT, ROCKETNET_CONFIG_LAYOUT_UINT64, ROCKETNET_CONFIG_LAYOUT_BYTES, ROCKETNET_CONFIG_LAYOUT_STRING )
static_assert( sizeof( ROCKETNET_CONFIG_LAYOUT_PUBLIC ) == sizeof( ROCKETNET_CONFIG_LAYOUT_TABLE ), "the rocketnet.h server transport config matches its table" );
#undef ROCKETNET_CONFIG_LAYOUT_PUBLIC
#undef ROCKETNET_CONFIG_LAYOUT_TABLE

#define ROCKETNET_CONFIG_LAYOUT_PUBLIC struct rocketnet_message_config_t
#define ROCKETNET_CONFIG_LAYOUT_TABLE struct rocketnet_message_config_table_t
ROCKETNET_MESSAGE_CONFIG_TABLE( ROCKETNET_CONFIG_LAYOUT_INT, ROCKETNET_CONFIG_LAYOUT_UINT64, ROCKETNET_CONFIG_LAYOUT_BYTES, ROCKETNET_CONFIG_LAYOUT_STRING )
static_assert( sizeof( ROCKETNET_CONFIG_LAYOUT_PUBLIC ) == sizeof( ROCKETNET_CONFIG_LAYOUT_TABLE ), "the rocketnet.h message config matches its table" );
#undef ROCKETNET_CONFIG_LAYOUT_PUBLIC
#undef ROCKETNET_CONFIG_LAYOUT_TABLE

const struct rocketnet_config_field_t * rocketnet_message_config_fields( int * num_fields )
{
    rocketnet_assert( num_fields );
    *num_fields = (int) ( sizeof(rocketnet_message_config_field_table) / sizeof(rocketnet_message_config_field_table[0]) );
    return rocketnet_message_config_field_table;
}

// The message config's validation (docs/CHANNELS.md 14): the table's ranges,
// then the cross rules, each refusal a log line naming the rule. Refuses,
// never clamps. Allocation-free, safe before init: the sizing functions an
// application calls before connecting run it.

int rocketnet_message_validate_config( const struct rocketnet_message_config_t * config )
{
    return rocketnet_message_validate_config_ex( config, NULL );
}

int rocketnet_message_validate_config_ex( const struct rocketnet_message_config_t * config, int * status )
{
    // docs/ERRORS-AND-LOGGING.md 2.1: every refusal of the message config is
    // CONFIG_OUT_OF_RANGE, and an accepted config is OK.
    struct rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_message_config_fields( &num_fields );
    if ( !rocketnet_config_validate( "message", config, fields, num_fields ) )
    {
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    const int W = config->window_messages;
    const int B = config->receive_ring_bytes;
    const int F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const int fragments = ( config->max_message_bytes + F - 1 ) / F;

    // the count bound and the ring index by number % W (6.1, 6.2)
    if ( ( W & ( W - 1 ) ) != 0 )
    {
        rocketnet_error( "message config window_messages %d is not a power of two", W );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // ring placements are multiples of 16 (4)
    if ( B % 16 != 0 )
    {
        rocketnet_error( "message config receive_ring_bytes %d is not a multiple of 16", B );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // a message that can never fit the receive ring could never be admitted (6.2 liveness)
    if ( config->max_message_bytes > B )
    {
        rocketnet_error( "message config max_message_bytes %d is above receive_ring_bytes %d", config->max_message_bytes, B );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // a block takes one number per fragment, all inside the window (8)
    if ( fragments > W )
    {
        rocketnet_error( "message config max_message_bytes %d needs %d fragments, above window_messages %d", config->max_message_bytes, fragments, W );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // the store's large class must hold one maximal block whole (4, 14)
    const uint32_t large = rn_send_store_chunks_for( (uint64_t) config->send_buffer_bytes, RN_STORE_LARGE );
    if ( large < (uint32_t) fragments )
    {
        rocketnet_error( "message config send_buffer_bytes %d holds %u large chunks, below the %d a max_message_bytes %d block needs",
                         config->send_buffer_bytes, large, fragments, config->max_message_bytes );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // the clamp of 6.4 needs min <= max, and the first timeout sits inside it
    if ( config->rto_min_ms > config->rto_max_ms )
    {
        rocketnet_error( "message config rto_min_ms %d is above rto_max_ms %d", config->rto_min_ms, config->rto_max_ms );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }
    if ( config->rto_initial_ms < config->rto_min_ms || config->rto_initial_ms > config->rto_max_ms )
    {
        rocketnet_error( "message config rto_initial_ms %d is outside rto_min_ms %d to rto_max_ms %d", config->rto_initial_ms, config->rto_min_ms, config->rto_max_ms );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    // at least ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS sends of the longest first
    // delay fit before the reliable timeout (6.4; tla/RNResendTiming.tla
    // ConfigAccepted and AttemptsBeforeTimeout)
    const int first_delay_ms = ROCKETNET_MESSAGE_RESEND_INTERVAL_MS > config->rto_initial_ms ? ROCKETNET_MESSAGE_RESEND_INTERVAL_MS : config->rto_initial_ms;
    if ( (int64_t) config->reliable_timeout_ms < (int64_t) ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS * (int64_t) first_delay_ms )
    {
        rocketnet_error( "message config reliable_timeout_ms %d fits fewer than %d sends of max( the %d ms resend interval, rto_initial_ms %d )",
                         config->reliable_timeout_ms, ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS, ROCKETNET_MESSAGE_RESEND_INTERVAL_MS, config->rto_initial_ms );
        if ( status )
            *status = ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE;
        return 0;
    }

    if ( status )
        *status = ROCKETNET_STATUS_OK;
    return 1;
}

const struct rocketnet_config_field_t * rocketnet_client_transport_config_fields( int * num_fields )
{
    rocketnet_assert( num_fields );
    *num_fields = (int) ( sizeof(rocketnet_client_config_fields) / sizeof(rocketnet_client_config_fields[0]) );
    return rocketnet_client_config_fields;
}

const struct rocketnet_config_field_t * rocketnet_server_transport_config_fields( int * num_fields )
{
    rocketnet_assert( num_fields );
    *num_fields = (int) ( sizeof(rocketnet_server_config_fields) / sizeof(rocketnet_server_config_fields[0]) );
    return rocketnet_server_config_fields;
}

// ----------------------------------------------------------------
//                          the env engine
// ----------------------------------------------------------------

// the public name of a config field's env var: the table row "driver" is
// the public config's "transport" (the candidate list), so its variable is
// ROCKETNET_<ENDPOINT>_TRANSPORT. every other row is its own name.

static const char * rocketnet_config_env_field_name( const char * field_name )
{
    return strcmp( field_name, "driver" ) == 0 ? "transport" : field_name;
}

bool rocketnet_config_env_var_name( const char * field_name, char * buffer, int buffer_size )
{
    rocketnet_assert( field_name );
    rocketnet_assert( buffer );

    field_name = rocketnet_config_env_field_name( field_name );

    const char * prefix = "ROCKETNET_";
    const int prefix_length = (int) strlen( prefix );
    const int name_length = (int) strlen( field_name );

    if ( prefix_length + name_length + 1 > buffer_size )
        return false;

    memcpy( buffer, prefix, (size_t) prefix_length );
    for ( int i = 0; i < name_length; i++ )
        buffer[prefix_length + i] = (char) toupper( (unsigned char) field_name[i] );
    buffer[prefix_length + name_length] = '\0';

    return true;
}

// the per-endpoint derivation: ROCKETNET_<TRANSPORT>_<FIELD>, both parts
// uppercased. the transport name is short ("server", "client"), so the
// prefix is assembled first and the same field walk appends the field.

bool rocketnet_config_env_var_name_for( const char * transport_name, const char * field_name, char * buffer, int buffer_size )
{
    rocketnet_assert( transport_name );
    rocketnet_assert( field_name );
    rocketnet_assert( buffer );

    field_name = rocketnet_config_env_field_name( field_name );

    char prefix[64];
    const int written = snprintf( prefix, sizeof(prefix), "ROCKETNET_%s_", transport_name );
    if ( written <= 0 || written >= (int) sizeof(prefix) )
        return false;
    for ( int i = 0; prefix[i] != '\0'; i++ )
        prefix[i] = (char) toupper( (unsigned char) prefix[i] );

    const int prefix_length = (int) strlen( prefix );
    const int name_length = (int) strlen( field_name );

    if ( prefix_length + name_length + 1 > buffer_size )
        return false;

    memcpy( buffer, prefix, (size_t) prefix_length );
    for ( int i = 0; i < name_length; i++ )
        buffer[prefix_length + i] = (char) toupper( (unsigned char) field_name[i] );
    buffer[prefix_length + name_length] = '\0';

    return true;
}

// strict full-string numeric parses: the whole value or a loud failure,
// never a prefix silently accepted. the charset is checked BEFORE the
// strto* call, because strtoll/strtoull quietly skip leading whitespace
// and accept '+' and '-' spellings the strict form refuses -- validating
// after the fact would let " 5" and "+5" ride through.

static bool rocketnet_config_numeric_charset( const char * value, bool allow_negative )
{
    const char * cursor = value;
    if ( allow_negative && *cursor == '-' )
        cursor++;
    if ( *cursor == '\0' )
        return false;
    for ( ; *cursor != '\0'; cursor++ )
    {
        if ( *cursor < '0' || *cursor > '9' )
            return false;
    }
    return true;
}

static bool rocketnet_config_parse_int( const char * value, int * out )
{
    if ( !rocketnet_config_numeric_charset( value, true ) )
        return false;
    char * end = NULL;
    errno = 0;
    long long parsed = strtoll( value, &end, 10 );
    if ( end == value || *end != '\0' || errno == ERANGE )
        return false;
    if ( parsed < INT32_MIN || parsed > INT32_MAX )
        return false;
    *out = (int) parsed;
    return true;
}

bool rocketnet_config_parse_int_value( const char * value, int * out )
{
    return rocketnet_config_parse_int( value, out );
}

static bool rocketnet_config_parse_uint64( const char * value, uint64_t * out )
{
    if ( !rocketnet_config_numeric_charset( value, false ) )
        return false;
    char * end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull( value, &end, 10 );
    if ( end == value || *end != '\0' || errno == ERANGE )
        return false;
    *out = (uint64_t) parsed;
    return true;
}

// stringify a field's current value for the displaced-value log line.
// secret fields never reach this; the caller redacts first.

static void rocketnet_config_field_to_string( const void * config, const struct rocketnet_config_field_t * field, char * buffer, int buffer_size )
{
    const uint8_t * base = (const uint8_t*) config + field->offset;

    switch ( field->kind )
    {
        case ROCKETNET_CONFIG_KIND_INT:
        {
            int value;
            memcpy( &value, base, sizeof(value) );
            snprintf( buffer, (size_t) buffer_size, "%d", value );
        }
        break;

        case ROCKETNET_CONFIG_KIND_UINT64:
        {
            uint64_t value;
            memcpy( &value, base, sizeof(value) );
            snprintf( buffer, (size_t) buffer_size, "%" PRIu64, value );
        }
        break;

        case ROCKETNET_CONFIG_KIND_BYTES:
        {
            if ( rocketnet_base64_encode_data( base, (size_t) field->size, buffer, (size_t) buffer_size ) <= 0 )
                snprintf( buffer, (size_t) buffer_size, "(unprintable)" );
        }
        break;

        case ROCKETNET_CONFIG_KIND_STRING:
        {
            snprintf( buffer, (size_t) buffer_size, "%.*s", field->size, (const char*) base );
        }
        break;

        default:
        {
            snprintf( buffer, (size_t) buffer_size, "(unknown kind)" );
        }
        break;
    }
}

bool rocketnet_config_apply_env_uint64( const char * transport_name, const char * field_name, uint64_t * out )
{
    rocketnet_assert( transport_name );
    rocketnet_assert( field_name );
    rocketnet_assert( out );

    char env_name[128];
    if ( !rocketnet_config_env_var_name_for( transport_name, field_name, env_name, sizeof(env_name) ) )
    {
        rocketnet_error( "%s config field name \"%s\" does not fit an env var name", transport_name, field_name );
        return false;
    }

    const char * value = rocketnet_platform_getenv( env_name );
    if ( !value || value[0] == '\0' )
        return true;

    if ( !rocketnet_config_parse_uint64( value, out ) )
    {
        rocketnet_error( "%s config could not parse %s=\"%s\" as an unsigned 64 bit integer", transport_name, env_name, value );
        return false;
    }

    return true;
}

// the environment names the transport, never the driver, with no alias. the table row
// "driver" is read only as ROCKETNET_<ENDPOINT>_TRANSPORT. the old driver
// spellings, process-wide ROCKETNET_DRIVER and per-endpoint
// ROCKETNET_<ENDPOINT>_DRIVER, are refused aloud, naming the variable to use
// now, and their values are never applied. (the process-wide
// ROCKETNET_TRANSPORT is refused by the general legacy rule in
// rocketnet_config_apply_env.)

static void rocketnet_config_refuse_driver_names( const char * transport_name, const char * env_name )
{
    char endpoint_driver[128];
    const int written = snprintf( endpoint_driver, sizeof(endpoint_driver), "ROCKETNET_%s_DRIVER", transport_name );
    if ( written <= 0 || written >= (int) sizeof(endpoint_driver) )
        endpoint_driver[0] = '\0';
    for ( int i = 0; endpoint_driver[i] != '\0'; i++ )
        endpoint_driver[i] = (char) toupper( (unsigned char) endpoint_driver[i] );

    const char * old_names[2] = { "ROCKETNET_DRIVER", endpoint_driver };
    for ( int i = 0; i < 2; i++ )
    {
        if ( old_names[i][0] == '\0' )
            continue;
        const char * value = rocketnet_platform_getenv( old_names[i] );
        if ( value && value[0] != '\0' )
            rocketnet_error( "%s config refuses %s: the transport is chosen by %s (transport, never driver)", transport_name, old_names[i], env_name );
    }
}

bool rocketnet_config_apply_env( const char * transport_name, void * config, const struct rocketnet_config_field_t * fields, int num_fields )
{
    rocketnet_assert( transport_name );
    rocketnet_assert( config );
    rocketnet_assert( fields );

    for ( int i = 0; i < num_fields; i++ )
    {
        const struct rocketnet_config_field_t * field = &fields[i];

        char env_name[128];
        if ( !rocketnet_config_env_var_name_for( transport_name, field->name, env_name, sizeof(env_name) ) )
        {
            rocketnet_error( "%s config field name \"%s\" does not fit an env var name", transport_name, field->name );
            return false;
        }

        // the old process-wide spelling is refused aloud, naming its
        // per-endpoint replacement, once per create: an operator who still
        // exports ROCKETNET_<FIELD> is never silently ignored -- their value
        // is NOT applied, and the error tells them the name to use now.

        char legacy_name[128];
        if ( !rocketnet_config_env_var_name( field->name, legacy_name, sizeof(legacy_name) ) )
        {
            rocketnet_error( "%s config field name \"%s\" does not fit an env var name", transport_name, field->name );
            return false;
        }

        const char * legacy_value = rocketnet_platform_getenv( legacy_name );
        if ( legacy_value && legacy_value[0] != '\0' )
            rocketnet_error( "%s config ignores the process-wide %s: use the per-endpoint %s instead", transport_name, legacy_name, env_name );

        if ( strcmp( field->name, "driver" ) == 0 )
            rocketnet_config_refuse_driver_names( transport_name, env_name );

        const char * value = rocketnet_platform_getenv( env_name );
        if ( !value )
            continue;

        if ( value[0] == '\0' )
        {
            rocketnet_info( "%s config ignores empty %s (set a value, or unset the variable)", transport_name, env_name );
            continue;
        }

        const bool secret = ( field->flags & ROCKETNET_CONFIG_SECRET ) != 0;

        char displaced[192];
        if ( secret )
            snprintf( displaced, sizeof(displaced), "(redacted)" );
        else
            rocketnet_config_field_to_string( config, field, displaced, sizeof(displaced) );

        uint8_t * base = (uint8_t*) config + field->offset;

        switch ( field->kind )
        {
            case ROCKETNET_CONFIG_KIND_INT:
            {
                int parsed;
                if ( !rocketnet_config_parse_int( value, &parsed ) )
                {
                    // the failing value is never echoed for a secret field
                    // -- a mistyped secret is still a secret
                    rocketnet_error( "%s config could not parse %s=\"%s\" as an integer", transport_name, env_name, secret ? "(redacted)" : value );
                    return false;
                }
                memcpy( base, &parsed, sizeof(parsed) );
            }
            break;

            case ROCKETNET_CONFIG_KIND_UINT64:
            {
                uint64_t parsed;
                if ( !rocketnet_config_parse_uint64( value, &parsed ) )
                {
                    rocketnet_error( "%s config could not parse %s=\"%s\" as an unsigned 64 bit integer", transport_name, env_name, secret ? "(redacted)" : value );
                    return false;
                }
                memcpy( base, &parsed, sizeof(parsed) );
            }
            break;

            case ROCKETNET_CONFIG_KIND_BYTES:
            {
                uint8_t decoded[256];
                if ( field->size > (int) sizeof(decoded) )
                {
                    // a real check, not an assert compiled out of release:
                    // a table field wider than the decode staging refuses
                    // rather than truncating
                    rocketnet_error( "%s config field \"%s\" is wider than the decode staging (%d > %d)", transport_name, field->name, field->size, (int) sizeof(decoded) );
                    return false;
                }
                const int decoded_bytes = rocketnet_base64_decode_data( value, decoded, (size_t) field->size );
                if ( decoded_bytes != field->size )
                {
                    rocketnet_error( "%s config could not parse %s as base64 of exactly %d bytes", transport_name, env_name, field->size );
                    return false;
                }
                memcpy( base, decoded, (size_t) field->size );
            }
            break;

            case ROCKETNET_CONFIG_KIND_STRING:
            {
                if ( (int) strlen( value ) >= field->size )
                {
                    rocketnet_error( "%s config %s value is too long (%d character maximum)", transport_name, env_name, field->size - 1 );
                    return false;
                }
                snprintf( (char*) base, (size_t) field->size, "%s", value );
            }
            break;

            default:
            {
                rocketnet_error( "%s config field \"%s\" has an unknown kind %d", transport_name, field->name, field->kind );
                return false;
            }
        }

        // the override, logged loudly with what it displaced: the audit
        // trail that keeps the default < struct < env layering honest

        rocketnet_info( "%s config override %s=%s (displaces %s)", transport_name, env_name, secret ? "(redacted)" : value, displaced );
    }

    return true;
}

bool rocketnet_config_validate( const char * transport_name, const void * config, const struct rocketnet_config_field_t * fields, int num_fields )
{
    rocketnet_assert( transport_name );
    rocketnet_assert( config );
    rocketnet_assert( fields );

    for ( int i = 0; i < num_fields; i++ )
    {
        const struct rocketnet_config_field_t * field = &fields[i];

        const uint8_t * base = (const uint8_t*) config + field->offset;

        switch ( field->kind )
        {
            case ROCKETNET_CONFIG_KIND_INT:
            {
                int value;
                memcpy( &value, base, sizeof(value) );
                if ( value == 0 && ( field->flags & ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) != 0 )
                    break;
                if ( (int64_t) value < field->min_value || (int64_t) value > field->max_value )
                {
                    rocketnet_error( "%s config %s %d is out of range (%" PRId64 " to %" PRId64 "%s)", transport_name, field->name, value,
                                     field->min_value, field->max_value,
                                     ( field->flags & ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) != 0 ? ", or zero for the default" : "" );
                    return false;
                }
            }
            break;

            case ROCKETNET_CONFIG_KIND_UINT64:
            case ROCKETNET_CONFIG_KIND_BYTES:
            {
                // any value of the full width is structurally valid;
                // semantic checks (keys present, and so on) stay with the
                // transport that owns the meaning
            }
            break;

            case ROCKETNET_CONFIG_KIND_STRING:
            {
                bool terminated = false;
                for ( int j = 0; j < field->size; j++ )
                {
                    if ( base[j] == '\0' )
                    {
                        terminated = true;
                        break;
                    }
                }
                if ( !terminated )
                {
                    rocketnet_error( "%s config %s is not a terminated string", transport_name, field->name );
                    return false;
                }
            }
            break;

            default:
            {
                rocketnet_error( "%s config field \"%s\" has an unknown kind %d", transport_name, field->name, field->kind );
                return false;
            }
        }
    }

    return true;
}

#if ROCKETNET_FILTER_EPOCH_TEST_HOOKS
static bool rocketnet_public_link_simulator_test_hook = false;
void rocketnet_public_link_simulator_test_development( int enabled )
{
    rocketnet_public_link_simulator_test_hook = enabled != 0;
}
#endif

bool rocketnet_public_link_simulator_development( void )
{
#if ROCKETNET_FILTER_EPOCH_TEST_HOOKS
    return ROCKETNET_DEVELOPMENT != 0 || rocketnet_public_link_simulator_test_hook;
#else
    return ROCKETNET_DEVELOPMENT != 0;
#endif
}

bool rocketnet_config_validate_public_link_simulator( const char * transport_name, const void * config, const struct rocketnet_config_field_t * fields, int num_fields, bool development )
{
    rocketnet_assert( transport_name );
    rocketnet_assert( config );
    rocketnet_assert( fields );

    if ( development )
        return true;

    static const char * simulator_names[] =
    {
        "simulated_loss_percent",
        "simulated_latency_ms",
        "simulated_jitter_ms",
        "simulated_duplicate_percent"
    };

    for ( int i = 0; i < num_fields; i++ )
    {
        const struct rocketnet_config_field_t * field = &fields[i];
        if ( field->kind != ROCKETNET_CONFIG_KIND_INT )
            continue;

        bool simulator_field = false;
        for ( int j = 0; j < (int) ( sizeof(simulator_names) / sizeof(simulator_names[0]) ); j++ )
        {
            if ( strcmp( field->name, simulator_names[j] ) == 0 )
            {
                simulator_field = true;
                break;
            }
        }
        if ( !simulator_field )
            continue;

        int value;
        memcpy( &value, (const uint8_t*) config + field->offset, sizeof(value) );
        if ( value != 0 )
        {
            rocketnet_error( "%s config %s %d is development only: the public link simulator is not available in a production library", transport_name, field->name, value );
            return false;
        }
    }

    return true;
}

// ----------------------------------------------------------------
//                        the driver ladder
// ----------------------------------------------------------------

static int rocketnet_driver_id_from_name( const char * name, int name_length )
{
    for ( int i = 0; i < ROCKETNET_NUM_DRIVERS; i++ )
    {
        if ( (int) strlen( rocketnet_driver_name( i ) ) == name_length && memcmp( rocketnet_driver_name( i ), name, (size_t) name_length ) == 0 )
            return i;
    }
    return -1;
}

bool rocketnet_driver_ladder_parse( const char * value, const int * available, int num_available, struct rocketnet_driver_ladder_t * ladder )
{
    rocketnet_assert( available );
    rocketnet_assert( num_available > 0 );
    rocketnet_assert( num_available <= ROCKETNET_MAX_DRIVER_CANDIDATES );
    rocketnet_assert( ladder );

    memset( ladder, 0, sizeof(struct rocketnet_driver_ladder_t) );

    // auto -- explicitly, or as the zero value -- expands to the
    // platform's recommended order for this endpoint

    bool is_auto = value == NULL;
    if ( !is_auto )
    {
        // trimmed whole-value comparison so " auto " and "" behave
        const char * start = value;
        while ( *start == ' ' || *start == '\t' )
            start++;
        const char * end = start + strlen( start );
        while ( end > start && ( end[-1] == ' ' || end[-1] == '\t' ) )
            end--;
        if ( end == start || ( end - start == 4 && memcmp( start, "auto", 4 ) == 0 ) )
            is_auto = true;
    }

    if ( is_auto )
    {
        // Pin-only drivers are never chosen by auto; a pin selects them.
        for ( int i = 0; i < num_available; i++ )
            if ( !rocketnet_driver_pin_only( available[i] ) )
                ladder->candidates[ladder->num_candidates++] = available[i];
        return true;
    }

    const char * cursor = value;

    while ( true )
    {
        const char * comma = strchr( cursor, ',' );
        const char * token_end = comma ? comma : cursor + strlen( cursor );

        const char * name = cursor;
        while ( name < token_end && ( *name == ' ' || *name == '\t' ) )
            name++;
        const char * name_end = token_end;
        while ( name_end > name && ( name_end[-1] == ' ' || name_end[-1] == '\t' ) )
            name_end--;

        const int name_length = (int) ( name_end - name );

        if ( name_length == 0 )
        {
            rocketnet_error( "empty transport name in transport list \"%s\" (valid names: %s)", value, rocketnet_driver_names() );
            return false;
        }

        if ( name_length == 4 && memcmp( name, "auto", 4 ) == 0 )
        {
            rocketnet_error( "\"auto\" must stand alone, not ride a transport list (\"%s\")", value );
            return false;
        }

        const int driver = rocketnet_driver_id_from_name( name, name_length );
        if ( driver < 0 )
        {
            rocketnet_error( "unknown transport \"%.*s\" in transport list \"%s\" (valid names: %s)", name_length, name, value, rocketnet_driver_names() );
            return false;
        }

        bool driver_available = false;
        for ( int i = 0; i < num_available; i++ )
        {
            if ( available[i] == driver )
            {
                driver_available = true;
                break;
            }
        }

        if ( driver_available )
        {
            if ( ladder->num_candidates >= ROCKETNET_MAX_DRIVER_CANDIDATES )
            {
                rocketnet_error( "transport list \"%s\" has too many candidates (%d maximum)", value, ROCKETNET_MAX_DRIVER_CANDIDATES );
                return false;
            }
            ladder->candidates[ladder->num_candidates++] = driver;
        }
        else
        {
            // known transport, absent here: ignored so one fleet-wide
            // config line deploys unmodified to heterogeneous boxes. the
            // skip list is bounded like the candidate list -- overflowing
            // it silently would under-name the exhausted report.

            if ( ladder->num_skipped >= ROCKETNET_MAX_DRIVER_CANDIDATES )
            {
                rocketnet_error( "transport list \"%s\" has too many skipped names (%d maximum)", value, ROCKETNET_MAX_DRIVER_CANDIDATES );
                return false;
            }
            ladder->skipped[ladder->num_skipped++] = driver;
            rocketnet_info( "transport %.*s is not available here. skipped", name_length, name );
        }

        if ( !comma )
            break;

        cursor = comma + 1;
    }

    // a list whose every name was skipped names no driver for this endpoint
    // at all -- a fleet-wide line read by an endpoint it was not addressing
    // (ROCKETNET_SERVER_TRANSPORT=gso and ROCKETNET_CLIENT_TRANSPORT=gso on a box that also runs a client). bricking
    // that endpoint would break the one-line-deploys-everywhere guarantee
    // the ignore rule exists for, so the endpoint falls to its own auto
    // order, loudly. a list with at least one applicable candidate keeps
    // full pin semantics: those candidates and only those, refusal is
    // failure. (ruled on the #110 review, inside the #106 ignore rule.)

    if ( ladder->num_candidates == 0 )
    {
        char auto_names[128];
        auto_names[0] = '\0';
        for ( int i = 0; i < num_available; i++ )
        {
            if ( rocketnet_driver_pin_only( available[i] ) ) continue;
            const size_t used = strlen( auto_names );
            snprintf( auto_names + used, sizeof(auto_names) - used, "%s%s", used > 0 ? "," : "", rocketnet_driver_name( available[i] ) );
        }

        rocketnet_warn( "transport list \"%s\" names no transport for this endpoint on this platform. using auto (%s)", value, auto_names );

        // Pin-only drivers are never chosen by auto; a pin selects them.
        for ( int i = 0; i < num_available; i++ )
            if ( !rocketnet_driver_pin_only( available[i] ) )
                ladder->candidates[ladder->num_candidates++] = available[i];
    }

    return true;
}
