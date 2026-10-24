/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The shared-library export cases, moved from the unit suite: each loads a
// separately built shared library through the OS loader at run time (dlopen,
// or LoadLibraryA on Windows) and looks a symbol up in it, which is an outside
// dependency rather than unit logic, so they run in the functional suite. The
// bodies and checks are as they were; only the place they run changed.
//
//   test_api_polish_dev_token_string
//   test_dev_connect_token_timeout_is_the_named_constant
//       load the development library (ROCKETNET_DEV_LIBRARY_PATH) and mint a
//       connect token through its rocketnet_create_dev_connect_token
//   test_dev_token_minting_absent_in_release
//       load the release library (ROCKETNET_RELEASE_LIBRARY_PATH) and check it
//       carries no rocketnet_create_dev_connect_token

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void rocketnet_shared_library_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                               \
do                                                                                                                  \
{                                                                                                                   \
    if ( !(condition) )                                                                                             \
    {                                                                                                               \
        rocketnet_shared_library_check_handler( #condition, (const char*) __FUNCTION__,                             \
                                                (const char*) __FILE__, __LINE__ );                                  \
    }                                                                                                               \
} while( 0 )

// The development minter's public signature, which both development cases
// call it through once loaded.
typedef enum rocketnet_status_t ( *shared_library_dev_minter_t )( const char * server_address_ipv4, const char * server_address_ipv6,
                                                                  const uint8_t * server_public_key, int expire_seconds,
                                                                  int timeout_seconds, uint64_t client_id, uint64_t protocol_id,
                                                                  uint64_t unix_timestamp, const uint8_t * user_data,
                                                                  char * connect_token );

// this target links the development archive in every build, so the minter is
// declared here whatever the release library was built with
static_assert( std::is_same<decltype( &rocketnet_create_dev_connect_token ), shared_library_dev_minter_t>::value, "dev token minter signature" );

// the development library keeps its own log state, apart from the sink the
// suite installs in the archive it links, so the test captures the refusal
// line in that library
typedef void ( *shared_library_dev_set_log_t )( rocketnet_log_function_t, void * );
static shared_library_dev_set_log_t shared_library_dev_set_log = NULL;
static int shared_library_dev_error_lines = 0;

static void shared_library_dev_log_capture( void *, enum rocketnet_log_level_t level, const char *, size_t )
{
    if ( level == ROCKETNET_LOG_LEVEL_ERROR )
        shared_library_dev_error_lines++;
}

static shared_library_dev_minter_t shared_library_dev_minter()
{
    shared_library_dev_minter_t minter = NULL;
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_DEV_LIBRARY_PATH );
    rocketnet_check( library != NULL );
    minter = (shared_library_dev_minter_t) (void *) GetProcAddress( library, "rocketnet_create_dev_connect_token" );
    shared_library_dev_set_log = (shared_library_dev_set_log_t) (void *) GetProcAddress( library, "rocketnet_log_function" );
#else
    void * library = dlopen( ROCKETNET_DEV_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_check( library != NULL );
    minter = (shared_library_dev_minter_t) dlsym( library, "rocketnet_create_dev_connect_token" );
    shared_library_dev_set_log = (shared_library_dev_set_log_t) dlsym( library, "rocketnet_log_function" );
#endif
    rocketnet_check( minter != NULL );
    rocketnet_check( shared_library_dev_set_log != NULL );
    return minter;
}

void rocketnet_test_api_polish_dev_token_string()
{
    shared_library_dev_minter_t create_dev = shared_library_dev_minter();
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memset( server_key, 0x42, sizeof( server_key ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES + 1];
    memset( token, 0x5A, sizeof( token ) );
    shared_library_dev_error_lines = 0;
    shared_library_dev_set_log( shared_library_dev_log_capture, NULL );

    // no address at all: refused and named, nothing written
    rocketnet_check( create_dev( NULL, NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 1000, NULL, token ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_check( shared_library_dev_error_lines == 1 );
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
    shared_library_dev_set_log( NULL, NULL );
}

// The dev connect token carries the lifetime and connection timeout it is
// given. Mint one with the development minter at timestamp 1000 using the
// typical values (the named constants), decode the string and read its signed
// fields back with the development backend key, pinning both values.
void rocketnet_test_dev_connect_token_timeout_is_the_named_constant()
{
    const uint64_t now = 1000;
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    shared_library_dev_minter_t create_dev = shared_library_dev_minter();
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

// Verify that rocketnet_create_dev_connect_token is absent from the release
// surface. In a production build (ROCKETNET_DEV=OFF) the development token
// minting helper and its backend secret key are compiled out, so the shipped
// shared library must not carry the symbol. This target links the development
// archive in every build, so which library is probed comes from
// ROCKETNET_FUNCTIONAL_RELEASE_LIBRARY_IS_DEV, which CMakeLists.txt sets from
// the ROCKETNET_DEV option, never from the ROCKETNET_DEV macro. In a
// development build the helper must remain callable -- the functional suite
// mints its tokens with it -- so that configuration asserts the call succeeds
// instead.
void rocketnet_test_dev_token_minting_absent_in_release()
{
#if ROCKETNET_FUNCTIONAL_RELEASE_LIBRARY_IS_DEV
    printf( "    release library is a development build: asserting the minter is callable\n" );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    memcpy( server_key, rocketnet_dev_server_public_key, sizeof( server_key ) );
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_check( rocketnet_create_dev_connect_token( "127.0.0.1:40000", NULL, server_key, 30, 10, 1, ROCKETNET_DEV_PROTOCOL_ID, 0, NULL, token ) == ROCKETNET_STATUS_OK );
    rocketnet_check( strlen( token ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
#else
    printf( "    probing release library %s for rocketnet_create_dev_connect_token\n", ROCKETNET_RELEASE_LIBRARY_PATH );
#if defined(_WIN32)
    HMODULE library = LoadLibraryA( ROCKETNET_RELEASE_LIBRARY_PATH );
    rocketnet_check( library != NULL );
    if ( library != NULL )
    {
        rocketnet_check( GetProcAddress( library, "rocketnet_create_dev_connect_token" ) == NULL );
        FreeLibrary( library );
    }
#else
    // CMake may place the shared library outside the source tree. Probe the
    // exact release target built alongside this test, independent of cwd.
    void * library = dlopen( ROCKETNET_RELEASE_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL );
    rocketnet_check( library != NULL );
    if ( library != NULL )
    {
        rocketnet_check( dlsym( library, "rocketnet_create_dev_connect_token" ) == NULL );
        dlclose( library );
    }
#endif // #if defined(_WIN32)
#endif // #if ROCKETNET_FUNCTIONAL_RELEASE_LIBRARY_IS_DEV
}
