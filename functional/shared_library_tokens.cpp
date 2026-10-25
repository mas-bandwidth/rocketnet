/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Shared-library export checks belong in the functional suite because they
    load a separately built library through the operating system loader.
*/

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"
#include "rocketnet_test_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void rocketnet_shared_library_token_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                                  \
{                                                                                                                   \
    if ( !(condition) )                                                                                             \
        rocketnet_shared_library_token_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
} while(0)

// The functional suite also links the development library. Load the separate
// shared library under test and call its public minter through the exported
// signature so its token-string contract and error logging remain covered.
typedef rocketnet_status_t ( *api_polish_dev_minter_t )( const char *, const char *, const uint8_t *, int, int, uint64_t, uint64_t, uint64_t, const uint8_t *, char * );
typedef void ( *api_polish_dev_set_log_t )( rocketnet_log_function_t, void * );

#if ROCKETNET_DEV
static_assert( std::is_same<decltype( &rocketnet_create_dev_connect_token ), api_polish_dev_minter_t>::value, "dev token minter signature" );
#endif

static api_polish_dev_set_log_t api_polish_dev_set_log = NULL;
static int api_polish_dev_error_lines = 0;

static void api_polish_dev_log_capture( void *, enum rocketnet_log_level_t level, const char *, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR )
        api_polish_dev_error_lines++;
}

static api_polish_dev_minter_t api_polish_dev_minter()
{
    api_polish_dev_minter_t minter = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_DEV_LIBRARY_PATH );
    rocketnet_check( library != NULL );
    minter = (api_polish_dev_minter_t) (void *) GetProcAddress( library, "rocketnet_create_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) (void *) GetProcAddress( library, "rocketnet_log_function" );
#else
    void * library = dlopen( ROCKETNET_DEV_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_check( library != NULL );
    minter = (api_polish_dev_minter_t) dlsym( library, "rocketnet_create_dev_connect_token" );
    api_polish_dev_set_log = (api_polish_dev_set_log_t) dlsym( library, "rocketnet_log_function" );
#endif
    rocketnet_check( minter != NULL );
    rocketnet_check( api_polish_dev_set_log != NULL );
    return minter;
}

void test_api_polish_dev_token_string()
{
    api_polish_dev_minter_t create_dev = api_polish_dev_minter();
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memset( server_key, 0x42, sizeof( server_key ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES + 1];
    memset( token, 0x5A, sizeof( token ) );
    api_polish_dev_error_lines = 0;
    api_polish_dev_set_log( api_polish_dev_log_capture, NULL );

    // no address at all: refused and named, nothing written
    rocketnet_check( create_dev( NULL, NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_check( api_polish_dev_error_lines == 1 );
    rocketnet_check( token[0] == 0x5A && token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1] == 0x5A );

    // an address that does not parse
    rocketnet_check( create_dev( "not an address", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ADDRESS );
    rocketnet_check( token[0] == 0x5A );

    // a dead lifetime is refused
    rocketnet_check( create_dev( "127.0.0.1:40000", NULL, server_key, 0, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_check( token[0] == 0x5A );

    // the string fills the public buffer exactly, NUL-terminated, nothing past it
    rocketnet_check( create_dev( "127.0.0.1:40000", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_OK );
    rocketnet_check( strlen( token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    rocketnet_check( token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES] == 0x5A );
    api_polish_dev_set_log( NULL, NULL );
}

// Load the dev library's exported token writer and pin the encoded timeout
// and expiry fields with the same deterministic timestamp and known keys.
typedef enum rocketnet_status_t ( *ctv_dev_token_minter_t )( const char * server_address_ipv4, const char * server_address_ipv6,
                                                             const uint8_t * server_public_key, int expire_seconds,
                                                             int timeout_seconds, uint64_t client_id, uint64_t protocol_id,
                                                             uint64_t unix_timestamp, const uint8_t * user_data,
                                                             char * connect_token );

static ctv_dev_token_minter_t ctv_dev_token_minter()
{
    ctv_dev_token_minter_t minter = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_DEV_LIBRARY_PATH );
    rocketnet_check( library != NULL );
    minter = (ctv_dev_token_minter_t) (void *) GetProcAddress( library, "rocketnet_create_dev_connect_token" );
#else
    void * library = dlopen( ROCKETNET_DEV_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_check( library != NULL );
    minter = (ctv_dev_token_minter_t) dlsym( library, "rocketnet_create_dev_connect_token" );
#endif
    rocketnet_check( minter != NULL );
    return minter;
}

void test_dev_connect_token_timeout_is_the_named_constant()
{
    const uint64_t now = 1000;
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    ctv_dev_token_minter_t create_dev = ctv_dev_token_minter();
    rocketnet_check( create_dev( "127.0.0.1:40000", NULL, rocketnet_test_server_public_key,
                                 ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS, ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS,
                                 1, ROCKETNET_DEV_PROTOCOL_ID, now, NULL, token_string ) == ROCKETNET_STATUS_OK );
    rocketnet_check( strlen( token_string ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    rocketnet_check( rocketnet_base64_decode_data( token_string, token, sizeof( token ) ) == ROCKETNET_CONNECT_TOKEN_BYTES );

    struct rocketnet_connect_token_t read_token;
    rocketnet_read_connect_token( &read_token, token );
    rocketnet_check( rocketnet_verify_signed_connect_data( read_token.signed_connect_data, read_token.signature,
                                                           rocketnet_dev_backend_public_key ) );

    struct rocketnet_signed_connect_data_t parsed;
    rocketnet_read_signed_connect_data( &parsed, read_token.signed_connect_data );
    rocketnet_check( parsed.create_timestamp == now );
    rocketnet_check( parsed.protocol_id == ROCKETNET_DEV_PROTOCOL_ID );
    rocketnet_check( parsed.timeout_seconds == ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS );
    rocketnet_check( parsed.expire_timestamp - parsed.create_timestamp == ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS );
}

// The functional executable links rocketnet_dev in both build modes. Select
// this case's release-library branch from the CMake ROCKETNET_DEV option.
void test_dev_token_minting_absent_in_release()
{
#if ROCKETNET_FUNCTIONAL_RELEASE_LIBRARY_IS_DEV
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memcpy( server_key, rocketnet_dev_server_public_key, sizeof( server_key ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( "127.0.0.1:40000", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 0, NULL, token ) == ROCKETNET_STATUS_OK );
    rocketnet_check( strlen( token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
#else
    printf( "release library probe: %s\n", ROCKETNET_RELEASE_LIBRARY_PATH );
    fflush( stdout );
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    HMODULE library = LoadLibraryA( ROCKETNET_RELEASE_LIBRARY_PATH );
    rocketnet_check( library != NULL );
    if ( library != NULL )
    {
        rocketnet_check( GetProcAddress( library, "rocketnet_create_dev_connect_token" ) == NULL );
        FreeLibrary( library );
    }
#else
    // Probe the exact release target built alongside this functional binary.
    void * library = dlopen( ROCKETNET_RELEASE_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_check( library != NULL );
    if ( library != NULL )
    {
        rocketnet_check( dlsym( library, "rocketnet_create_dev_connect_token" ) == NULL );
        dlclose( library );
    }
#endif
#endif
}
