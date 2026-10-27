/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The environment overlay: rocketnet_config_apply_env, which is how an
// operator's keys, addresses and limits reach every config at deploy time,
// over every env-backed field of the real tables -- the client and server
// transport tables, the message table under both endpoint names, and the two
// send budgets. rocketnet_fuzz.cpp selector 3 drives the value parsers on
// their own (the integer parse, the transport ladder, the env name); this
// target drives the overlay as create runs it: one variable set in the
// process environment, the table applied to a default struct, the result
// validated.
//
// INPUT: [table][field][mode][value...]
//   table % 6: client transport, server transport, message (as "client"),
//              message (as "server"), client budget, server budget
//   field:     index into the table's real descriptors
//   mode bit 0: set the retired process-wide name ROCKETNET_<FIELD> instead
//              of the per-endpoint ROCKETNET_<ENDPOINT>_<FIELD>
//   mode bit 1: the value is the transport ladder too, parsed against both
//              the server's and the client's available transports
//   value:     the bytes up to the first NUL, at most 5000 of them, so the
//              platform's 4096-byte env read limit is crossed
// The variable is unset again before the input returns; at initialize every
// name the tables derive is unset, so the process's own environment never
// reaches an input.
//
// THE ORACLES, each from the overlay's documented contract
// (sdk/rocketnet_config.h) or its code:
//   - the defaults validate, for every table
//   - an unset or empty variable, a NO_ENV field and the retired name all
//     leave the struct byte-identical and apply returns true
//   - a set variable applies exactly when its kind's parse accepts it: an
//     INT field when rocketnet_config_parse_int_value accepts, and then the
//     field is that value; a UINT64 field exactly when
//     rocketnet_config_apply_env_uint64 accepts, with the same value; a
//     BYTES field when the value is base64 of exactly the field's length,
//     and then the field is those bytes; a STRING field when it fits the
//     capacity with its terminator, and then the field is the value
//   - a value at or past the env read limit is refused
//   - an applied field touches no other byte of the struct, and a refused
//     one leaves the struct as it was
//   - after an applied numeric field, validation passes exactly when the
//     value is in the field's declared range (or zero, where zero means the
//     default)
//   - a ladder that parses names real transports and fits its array

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_allocator.h"
#include "rocketnet_base64.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tests/portable_crt.h"

// under AddressSanitizer the harness can read the live heap, so it checks
// that an input leaves the heap as it found it (fuzz_overlay below)
#if defined( __has_feature )
#if __has_feature( address_sanitizer )
#define ROCKETNET_FUZZ_HEAP_CHECK 1
#endif // #if __has_feature( address_sanitizer )
#endif // #if defined( __has_feature )
#if !defined( ROCKETNET_FUZZ_HEAP_CHECK ) && defined( __SANITIZE_ADDRESS__ )
#define ROCKETNET_FUZZ_HEAP_CHECK 1
#endif // #if !defined( ROCKETNET_FUZZ_HEAP_CHECK ) && defined( __SANITIZE_ADDRESS__ )
#if defined( ROCKETNET_FUZZ_HEAP_CHECK ) && defined( _WIN32 )
#undef ROCKETNET_FUZZ_HEAP_CHECK  // the check is for the POSIX environ; MSVC's ASan headers differ
#endif // #if defined( ROCKETNET_FUZZ_HEAP_CHECK ) && defined( _WIN32 )
#if defined( ROCKETNET_FUZZ_HEAP_CHECK )
#if defined( __GNUC__ ) && !defined( __clang__ )
// GCC's ASan runtime exports this interface but does not install its header.
extern "C" size_t __sanitizer_get_current_allocated_bytes( void );
#else
#include <sanitizer/allocator_interface.h>
#endif // #if defined( __GNUC__ ) && !defined( __clang__ )
#endif // #if defined( ROCKETNET_FUZZ_HEAP_CHECK )

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

static void fuzz_setenv( const char * name, const char * value )
{
#if defined( _WIN32 )
    _putenv_s( name, value );
#else
    // putenv over one buffer the harness owns, not setenv: glibc's setenv
    // keeps every NAME=value string it ever made until the process exits, so
    // millions of distinct fuzz values grew the heap to libFuzzer's rss limit.
    // fuzz_overlay unsets the variable before the next set, so one buffer
    // serves every input.

    static char entry[128 + 1 + 5001];
    snprintf( entry, sizeof( entry ), "%s=%s", name, value );
    putenv( entry );
#endif // #if defined( _WIN32 )
}

static void fuzz_unsetenv( const char * name )
{
#if defined( _WIN32 )
    _putenv_s( name, "" );
#else
    unsetenv( name );
#endif // #if defined( _WIN32 )
}

struct fuzz_table_t
{
    const char * endpoint;
    const struct rocketnet_config_field_t * fields;
    int num_fields;
    size_t struct_bytes;
};

#define FUZZ_NUM_TABLES 6

static struct fuzz_table_t fuzz_tables[FUZZ_NUM_TABLES];

// the struct a table overlays, at its defaults; storage wide enough for any
union fuzz_config_storage_t
{
    struct rocketnet_client_transport_config_t client;
    struct rocketnet_server_transport_config_t server;
    struct rocketnet_message_config_t message;
    int budget;
};

static void fuzz_defaults( int table, union fuzz_config_storage_t * config )
{
    memset( config, 0, sizeof( *config ) );

    switch ( table )
    {
        case 0:  rocketnet_client_transport_default_config_current( &config->client ); break;
        case 1:  rocketnet_server_transport_default_config_current( &config->server ); break;
        case 2:
        case 3:  rocketnet_message_default_config( &config->message ); break;
        default: config->budget = 0; break;
    }
}

static bool fuzz_in_range( const struct rocketnet_config_field_t * field, int64_t value )
{
    if ( value == 0 && ( field->flags & ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) != 0 )
        return true;
    return value >= field->min_value && value <= field->max_value;
}

static void fuzz_check_ladder( const char * value )
{
    for ( int side = 0; side < 2; side++ )
    {
        int num_available = 0;
        const int * available = side == 0 ? rocketnet_server_transport_io_availability( &num_available )
                                          : rocketnet_client_transport_io_availability( &num_available );

        struct rocketnet_transport_ladder_t ladder;
        memset( &ladder, 0, sizeof( ladder ) );
        if ( !rocketnet_transport_ladder_parse( value, available, num_available, &ladder ) )
            continue;
        rocketnet_fuzz_require( ladder.num_candidates >= 1 && ladder.num_candidates <= ROCKETNET_MAX_TRANSPORT_CANDIDATES, "a parsed ladder holds one to the maximum candidates" );
        rocketnet_fuzz_require( ladder.num_skipped >= 0 && ladder.num_skipped <= ROCKETNET_MAX_TRANSPORT_CANDIDATES, "a parsed ladder's skips fit its array" );
        for ( int i = 0; i < ladder.num_candidates; i++ )
        {
            rocketnet_fuzz_require( ladder.candidates[i] >= 0 && ladder.candidates[i] < ROCKETNET_NUM_TRANSPORTS, "a ladder candidate is a real transport" );

            bool offered = false;

            for ( int j = 0; j < num_available; j++ )
            {
                offered = offered || available[j] == ladder.candidates[i];
            }

            rocketnet_fuzz_require( offered, "a ladder candidate is an available transport" );
        }
    }
}

static void fuzz_overlay( int table_index, int field_index, uint8_t mode, const char * value )
{
    const struct fuzz_table_t * table = &fuzz_tables[table_index];
    if ( table->num_fields <= 0 )
        return;

    const struct rocketnet_config_field_t * field = &table->fields[field_index % table->num_fields];

    char name[128];
    const bool retired = ( mode & 1 ) != 0;
    const bool named = retired ? rocketnet_config_env_var_name( field->name, name, (int) sizeof( name ) )
                               : rocketnet_config_env_var_name_for( table->endpoint, field->name, name, (int) sizeof( name ) );
    rocketnet_fuzz_require( named, "every table field derives an env name" );

    union fuzz_config_storage_t before;
    union fuzz_config_storage_t config;
    fuzz_defaults( table_index, &before );
    config = before;

    fuzz_setenv( name, value );

    const bool applied = rocketnet_config_apply_env( table->endpoint, &config, table->fields, table->num_fields );

    // the uint64 overlay, the same variable through its own entry point

    uint64_t overlay_value = 0x5A5A5A5A5A5A5A5AULL;
    bool overlay_applied = false;

    if ( !retired && field->kind == ROCKETNET_CONFIG_KIND_UINT64 )
    {
        overlay_applied = rocketnet_config_apply_env_uint64( table->endpoint, field->name, &overlay_value );
    }

    fuzz_unsetenv( name );

    const size_t length = strlen( value );
    const uint8_t * a = (const uint8_t *) &before;
    const uint8_t * b = (const uint8_t *) &config;
    const size_t start = (size_t) field->offset;
    const size_t end = start + (size_t) field->size;

    const bool unchanged = memcmp( &before, &config, table->struct_bytes ) == 0;

    if ( retired || ( field->flags & ROCKETNET_CONFIG_NO_ENV ) != 0 || length == 0 )
    {
        rocketnet_fuzz_require( applied && unchanged, "an ignored variable leaves the config as it was" );
        return;
    }

    if ( length >= ROCKETNET_PLATFORM_GETENV_MAX_BYTES )
    {
        rocketnet_fuzz_require( !applied && unchanged, "a value past the env read limit is refused" );
        return;
    }

    if ( !applied )
    {
        rocketnet_fuzz_require( unchanged, "a refused value leaves the config as it was" );
    }
    else
    {
        rocketnet_fuzz_require( memcmp( a, b, start ) == 0 && memcmp( a + end, b + end, table->struct_bytes - end ) == 0,
                                "an applied variable touches only its own field" );
    }

    switch ( field->kind )
    {
        case ROCKETNET_CONFIG_KIND_INT:
        {
            int parsed = 0;
            const bool parses = rocketnet_config_parse_int_value( value, &parsed );
            rocketnet_fuzz_require( applied == parses, "an INT field applies exactly when the strict parse accepts" );
            if ( applied )
            {
                int stored = 0;
                memcpy( &stored, b + start, sizeof( stored ) );
                rocketnet_fuzz_require( stored == parsed, "an applied INT field holds the parsed value" );
                rocketnet_fuzz_require( rocketnet_config_validate( table->endpoint, &config, table->fields, table->num_fields ) == fuzz_in_range( field, stored ),
                                        "validation passes exactly when the applied INT is in range" );
            }
        }
        break;

        case ROCKETNET_CONFIG_KIND_UINT64:
        {
            rocketnet_fuzz_require( applied == overlay_applied, "the table overlay and the uint64 overlay agree on acceptance" );
            if ( applied )
            {
                uint64_t stored = 0;
                memcpy( &stored, b + start, sizeof( stored ) );
                rocketnet_fuzz_require( stored == overlay_value, "the table overlay and the uint64 overlay agree on the value" );

                const bool ranged = field->max_value > field->min_value;
                const bool in_range = !ranged || ( stored >= (uint64_t) field->min_value && stored <= (uint64_t) field->max_value );
                rocketnet_fuzz_require( rocketnet_config_validate( table->endpoint, &config, table->fields, table->num_fields ) == in_range,
                                        "validation passes exactly when the applied UINT64 is in range" );
            }
        }
        break;

        case ROCKETNET_CONFIG_KIND_BYTES:
        {
            uint8_t decoded[256];
            rocketnet_fuzz_require( field->size <= (int) sizeof( decoded ), "a BYTES field fits the harness's staging" );

            const int decoded_bytes = rocketnet_base64_decode_data( value, decoded, (size_t) field->size );
            rocketnet_fuzz_require( applied == ( decoded_bytes == field->size ), "a BYTES field applies exactly when the value is base64 of its length" );
            if ( applied )
            {
                rocketnet_fuzz_require( memcmp( b + start, decoded, (size_t) field->size ) == 0, "an applied BYTES field holds the decoded bytes" );
            }
        }
        break;

        case ROCKETNET_CONFIG_KIND_STRING:
        {
            rocketnet_fuzz_require( applied == ( (int) length < field->size ), "a STRING field applies exactly when it fits with its terminator" );
            if ( applied )
            {
                rocketnet_fuzz_require( strcmp( (const char *) ( b + start ), value ) == 0, "an applied STRING field holds the value" );
            }

            rocketnet_fuzz_require( rocketnet_config_validate( table->endpoint, &config, table->fields, table->num_fields ), "a string overlay leaves a valid config" );
        }
        break;

        default:
            rocketnet_fuzz_require( false, "every field has a known kind" );
    }
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir> writes, for each table,
//  one seed per field kind carrying the field's own default as the env
//  would spell it (the integer in decimal, the bytes through the repo's
//  base64 encoder, the string as is), the first INT field at its declared
//  minimum and maximum, and the transport field set to the ladder of every
//  available transport by name. ROCKETNET_FUZZ_SEED_FROM=<fuzz/corpus>
//  additionally rewraps the committed config_* seeds of rocketnet_fuzz as
//  the server's transport variable. The replay then continues as usual.
// -----------------------------------------------------------------------

static void fuzz_write_seed( const char * dir, const char * name, int table, int field, uint8_t mode, const char * value )
{
    char path[1024];
    if ( snprintf( path, sizeof( path ), "%s/%s", dir, name ) >= (int) sizeof( path ) )
        abort(); // a truncated path would write some other file

    FILE * file = rocketnet_test_fopen( path, "wb" );
    if ( !file )
    {
        abort();
    }

    const uint8_t head[3] = { (uint8_t) table, (uint8_t) field, mode };
    fwrite( head, 1, 3, file );
    fwrite( value, 1, strlen( value ), file );
    fclose( file );
}

static const char * fuzz_table_names[FUZZ_NUM_TABLES] = { "client_transport", "server_transport", "message_client", "message_server", "client_budget", "server_budget" };

static void fuzz_write_seeds( const char * dir, const char * from )
{
    for ( int t = 0; t < FUZZ_NUM_TABLES; t++ )
    {
        union fuzz_config_storage_t config;
        fuzz_defaults( t, &config );

        const uint8_t * base = (const uint8_t *) &config;
        bool kind_written[4] = { false, false, false, false };
        bool range_written = false;

        for ( int i = 0; i < fuzz_tables[t].num_fields; i++ )
        {
            const struct rocketnet_config_field_t * field = &fuzz_tables[t].fields[i];
            if ( ( field->flags & ROCKETNET_CONFIG_NO_ENV ) != 0 || field->kind < 0 || field->kind > 3 )
                continue;

            char value[1024];
            char name[256];

            if ( field->kind == ROCKETNET_CONFIG_KIND_INT && !range_written )
            {
                snprintf( value, sizeof( value ), "%lld", (long long) field->min_value );
                snprintf( name, sizeof( name ), "%s_%s_min", fuzz_table_names[t], field->name );
                fuzz_write_seed( dir, name, t, i, 0, value );
                snprintf( value, sizeof( value ), "%lld", (long long) field->max_value );
                snprintf( name, sizeof( name ), "%s_%s_max", fuzz_table_names[t], field->name );
                fuzz_write_seed( dir, name, t, i, 0, value );
                range_written = true;
            }

            if ( kind_written[field->kind] )
                continue;

            switch ( field->kind )
            {
                case ROCKETNET_CONFIG_KIND_INT:
                {
                    int v = 0;
                    memcpy( &v, base + field->offset, sizeof( v ) );
                    snprintf( value, sizeof( value ), "%d", v );
                }
                break;
                case ROCKETNET_CONFIG_KIND_UINT64:
                {
                    uint64_t v = 0;
                    memcpy( &v, base + field->offset, sizeof( v ) );
                    snprintf( value, sizeof( value ), "%llu", (unsigned long long) v );
                }
                break;
                case ROCKETNET_CONFIG_KIND_BYTES:
                    if ( rocketnet_base64_encode_data( base + field->offset, (size_t) field->size, value, sizeof( value ) ) <= 0 )
                    {
                        abort();
                    }

                    break;
                default:
                    snprintf( value, sizeof( value ), "%s", (const char *) ( base + field->offset ) );
                    break;
            }

            if ( value[0] == '\0' )
                continue;
            snprintf( name, sizeof( name ), "%s_%s_default", fuzz_table_names[t], field->name );
            fuzz_write_seed( dir, name, t, i, 0, value );
            kind_written[field->kind] = true;
        }
    }

    // the server's transport field, by its index in the real table

    int transport_field = -1;

    for ( int i = 0; i < fuzz_tables[1].num_fields; i++ )
        if ( strcmp( fuzz_tables[1].fields[i].name, "transport" ) == 0 )
        {
            transport_field = i;
        }

    if ( transport_field < 0 )
    {
        abort();
    }

    int num_available = 0;
    const int * available = rocketnet_server_transport_io_availability( &num_available );
    char ladder[1024];
    ladder[0] = '\0';
    for ( int i = 0; i < num_available; i++ )
    {
        const char * transport = rocketnet_transport_name( available[i] );
        if ( !transport )
            continue;

        const size_t used = strlen( ladder );
        snprintf( ladder + used, sizeof( ladder ) - used, "%s%s", ladder[0] != '\0' ? "," : "", transport );
    }

    fuzz_write_seed( dir, "server_transport_ladder_available", 1, transport_field, 2, ladder );

    if ( from == NULL || from[0] == '\0' )
        return;
    const char * committed[] = { "config_auto", "config_auto_in_list", "config_empty_element", "config_ladder", "config_long_list",
                                 "config_pin_portable", "config_spaced", "config_unknown_name" };

    for ( size_t c = 0; c < sizeof( committed ) / sizeof( committed[0] ); c++ )
    {
        char path[1024];
        if ( snprintf( path, sizeof( path ), "%s/%s", from, committed[c] ) >= (int) sizeof( path ) )
            continue;

        FILE * file = rocketnet_test_fopen( path, "rb" );
        if ( !file )
            continue;

        char value[1024];
        const size_t got = fread( value, 1, sizeof( value ) - 1, file );

        fclose( file );
        if ( got < 1 )
            continue;
        value[got] = '\0';

        char name[256];
        snprintf( name, sizeof( name ), "server_transport_%s", committed[c] );
        fuzz_write_seed( dir, name, 1, transport_field, 2, value + 1 );     // past rocketnet_fuzz's selector byte
    }
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    int n = 0;
    const struct rocketnet_config_field_t * f = rocketnet_client_transport_config_fields( &n );
    fuzz_tables[0] = { "client", f, n, sizeof( struct rocketnet_client_transport_config_t ) };
    f = rocketnet_server_transport_config_fields( &n );
    fuzz_tables[1] = { "server", f, n, sizeof( struct rocketnet_server_transport_config_t ) };
    f = rocketnet_message_config_fields( &n );
    fuzz_tables[2] = { "client", f, n, sizeof( struct rocketnet_message_config_t ) };
    fuzz_tables[3] = { "server", f, n, sizeof( struct rocketnet_message_config_t ) };
    f = rocketnet_client_budget_config_fields( &n );
    fuzz_tables[4] = { "client", f, n, sizeof( int ) };
    f = rocketnet_server_budget_config_fields( &n );
    fuzz_tables[5] = { "server", f, n, sizeof( int ) };

    // no variable of the process's own environment reaches an input, and the
    // defaults are a valid config for every table

    for ( int t = 0; t < FUZZ_NUM_TABLES; t++ )
    {
        for ( int i = 0; i < fuzz_tables[t].num_fields; i++ )
        {
            char name[128];
            if ( rocketnet_config_env_var_name_for( fuzz_tables[t].endpoint, fuzz_tables[t].fields[i].name, name, (int) sizeof( name ) ) )
            {
                fuzz_unsetenv( name );
            }

            if ( rocketnet_config_env_var_name( fuzz_tables[t].fields[i].name, name, (int) sizeof( name ) ) )
            {
                fuzz_unsetenv( name );
            }
        }

        union fuzz_config_storage_t config;
        fuzz_defaults( t, &config );
        rocketnet_fuzz_require( rocketnet_config_validate( fuzz_tables[t].endpoint, &config, fuzz_tables[t].fields, fuzz_tables[t].num_fields ),
                                "the defaults validate" );
    }

    // the first variable set moves environ onto the heap, once per process;
    // do it here so the per-input heap check sees only what an input leaves

    fuzz_setenv( "ROCKETNET_FUZZ_ENVIRON_WARM", "1" );
    fuzz_unsetenv( "ROCKETNET_FUZZ_ENVIRON_WARM" );

    char seed_dir[1024];
    char seed_from[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        fuzz_write_seeds( seed_dir, rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_FROM", seed_from, sizeof( seed_from ) ) ? seed_from : NULL );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 3 )
        return 0;

    static char value[5001];
    size_t n = 0;
    for ( size_t i = 3; i < size && n + 1 < sizeof( value ) && data[i] != 0; i++ )
    {
        value[n++] = (char) data[i];
    }

    value[n] = '\0';

#if defined( ROCKETNET_FUZZ_HEAP_CHECK )
    const size_t heap_before = __sanitizer_get_current_allocated_bytes();
#endif // #if defined( ROCKETNET_FUZZ_HEAP_CHECK )
    fuzz_overlay( data[0] % FUZZ_NUM_TABLES, data[1], data[2], value );
#if defined( ROCKETNET_FUZZ_HEAP_CHECK )
    rocketnet_fuzz_require( __sanitizer_get_current_allocated_bytes() <= heap_before,
                            "an overlay leaves the heap as it found it" );
#endif // #if defined( ROCKETNET_FUZZ_HEAP_CHECK )
    if ( data[2] & 2 )
    {
        fuzz_check_ladder( value );
    }

    return 0;
}
