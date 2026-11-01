/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    config_env: configuration precedence, default < struct < environment. It
    prints two server defaults, sets them in the config struct and creates a
    server, then shows the matching ROCKETNET_ environment variable winning,
    with the library's override log line, and a bad value refused.

    The C standard library cannot set an environment variable in-process, so
    the example runs itself again as a child process whose environment is this
    one with ROCKETNET_SERVER_MAX_CLIENTS set, once to 5 and once to 99999. The
    child is spawned directly (posix_spawnp, or CreateProcessW on Windows),
    never through a shell, so a path with spaces or shell characters runs as
    it is. Run: ./bin/example_config_env
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char ** environ;
#define child_environ environ
#endif // #if defined( _WIN32 )

#if defined( _WIN32 )
struct child_environment_entry_t
{
    const wchar_t * text;
    int name_chars;
};

static int compare_child_environment( const void * a, const void * b )
{
    const struct child_environment_entry_t * left = (const struct child_environment_entry_t *) a;
    const struct child_environment_entry_t * right = (const struct child_environment_entry_t *) b;
    return CompareStringOrdinal( left->text, left->name_chars, right->text, right->name_chars, TRUE ) - CSTR_EQUAL;
}

static int run_windows_child( const char * value )
{
    static const wchar_t name[] = L"ROCKETNET_SERVER_MAX_CLIENTS=";
    const size_t prefix_chars = sizeof( name ) / sizeof( wchar_t ) - 1;
    int result = 1, count = 0, used = 0;
    size_t block_chars = 1;
    DWORD module_chars = 0;
    wchar_t * inherited = GetEnvironmentStringsW();
    wchar_t * variable = NULL;
    wchar_t * block = NULL;
    wchar_t * module = NULL;
    wchar_t * quoted = NULL;
    struct child_environment_entry_t * entries = NULL;
    HANDLE handles[3] = { NULL, NULL, NULL };
    const DWORD standard_ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    STARTUPINFOW startup = { 0 };
    PROCESS_INFORMATION process = { 0 };
    const int value_chars = MultiByteToWideChar( CP_ACP, 0, value, -1, NULL, 0 );
    if ( !inherited || value_chars == 0 )
        goto cleanup;
    for ( const wchar_t * entry = inherited; *entry; entry += wcslen( entry ) + 1 )
        count++;
    entries = (struct child_environment_entry_t *) calloc( (size_t) count + 1, sizeof( *entries ) );
    variable = (wchar_t *) malloc( ( prefix_chars + (size_t) value_chars ) * sizeof( wchar_t ) );
    if ( !entries || !variable )
        goto cleanup;
    memcpy( variable, name, prefix_chars * sizeof( wchar_t ) );
    if ( MultiByteToWideChar( CP_ACP, 0, value, -1, variable + prefix_chars, value_chars ) == 0 )
        goto cleanup;

    // Keep all inherited strings, including any hidden per-drive entries.
    for ( const wchar_t * entry = inherited; *entry; entry += wcslen( entry ) + 1 )
        if ( wcslen( entry ) < prefix_chars ||
             CompareStringOrdinal( entry, (int) prefix_chars, name, (int) prefix_chars, TRUE ) != CSTR_EQUAL )
            entries[used++].text = entry;
    entries[used++].text = variable;
    for ( int i = 0; i < used; i++ )
    {
        const size_t chars = wcslen( entries[i].text ) + 1;
        if ( chars > SIZE_MAX / sizeof( wchar_t ) - block_chars )
            goto cleanup;
        block_chars += chars;
        // A hidden drive entry's second '=' separates its name and value.
        const wchar_t * equals = wcschr( entries[i].text + ( entries[i].text[0] == L'=' ), L'=' );
        if ( !equals )
            goto cleanup;
        entries[i].name_chars = (int) ( equals - entries[i].text );
    }

    // Windows requires case-insensitive Unicode order by name, not by NAME=value.
    qsort( entries, (size_t) used, sizeof( *entries ), compare_child_environment );
    block = (wchar_t *) malloc( block_chars * sizeof( wchar_t ) );
    // Resolve this module, including its extension, independently of argv[0].
    // The Windows maximum path fits this bound; truncation is a failure.
    module = (wchar_t *) malloc( 32768 * sizeof( wchar_t ) );
    if ( !block || !module )
        goto cleanup;
    module_chars = GetModuleFileNameW( NULL, module, 32768 );
    if ( module_chars == 0 || module_chars >= 32768 )
        goto cleanup;
    quoted = (wchar_t *) malloc( ( (size_t) module_chars + 3 ) * sizeof( wchar_t ) );
    if ( !quoted )
        goto cleanup;
    {
        wchar_t * next = block;
        for ( int i = 0; i < used; i++ )
        {
            const size_t chars = wcslen( entries[i].text ) + 1;
            memcpy( next, entries[i].text, chars * sizeof( wchar_t ) );
            next += chars;
        }
        *next = L'\0'; // one NUL after the last string's own NUL
    }
    quoted[0] = L'"';
    memcpy( quoted + 1, module, module_chars * sizeof( wchar_t ) );
    quoted[module_chars + 1] = L'"';
    quoted[module_chars + 2] = L'\0';

    // Preserve redirected output without changing the parent's handle flags.
    for ( int i = 0; i < 3; i++ )
    {
        const HANDLE handle = GetStdHandle( standard_ids[i] );
        if ( handle && handle != INVALID_HANDLE_VALUE &&
             !DuplicateHandle( GetCurrentProcess(), handle, GetCurrentProcess(), &handles[i], 0, TRUE, DUPLICATE_SAME_ACCESS ) )
            goto cleanup;
    }
    startup.cb = sizeof( startup );
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = handles[0];
    startup.hStdOutput = handles[1];
    startup.hStdError = handles[2];
    if ( CreateProcessW( module, quoted, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, block, NULL, &startup, &process ) )
    {
        DWORD exit_code = 1;
        if ( WaitForSingleObject( process.hProcess, INFINITE ) == WAIT_OBJECT_0 &&
             GetExitCodeProcess( process.hProcess, &exit_code ) && exit_code == 0 )
            result = 0;
        if ( !CloseHandle( process.hThread ) )
            result = 1;
        if ( !CloseHandle( process.hProcess ) )
            result = 1;
    }

cleanup:
    for ( int i = 0; i < 3; i++ )
        if ( handles[i] && !CloseHandle( handles[i] ) )
            result = 1;
    free( entries );
    free( variable );
    free( quoted );
    free( module );
    free( block );
    if ( inherited && !FreeEnvironmentStringsW( inherited ) )
        result = 1;
    return result;
}
#endif // #if defined( _WIN32 )

// runs this program again with this environment plus ROCKETNET_SERVER_MAX_CLIENTS
// set to value, and waits for it: 0 when the child exits 0, else 1

static int rerun_with_max_clients( char * program, const char * value )
{
#if defined( _WIN32 )
    (void) program; // Windows resolves this executable independently of argv[0].
    const int result = run_windows_child( value );
    if ( result != 0 )
        printf( "FAIL: the run with ROCKETNET_SERVER_MAX_CLIENTS=%s did not exit 0\n", value );
    return result;
#else
    static const char name[] = "ROCKETNET_SERVER_MAX_CLIENTS=";

    int count = 0;
    while ( child_environ[count] )
        count++;

    char ** environment = (char **) calloc( (size_t) count + 2, sizeof( char * ) );
    char * variable = (char *) malloc( sizeof( name ) + strlen( value ) );
    if ( !environment || !variable )
    {
        free( environment );
        free( variable );
        printf( "FAIL: child environment: out of memory\n" );
        return 1;
    }

    // every variable but an inherited ROCKETNET_SERVER_MAX_CLIENTS, then ours

    int used = 0;
    for ( int i = 0; i < count; i++ )
        if ( strncmp( child_environ[i], name, sizeof( name ) - 1 ) != 0 )
            environment[used++] = child_environ[i];

    memcpy( variable, name, sizeof( name ) - 1 );
    memcpy( variable + sizeof( name ) - 1, value, strlen( value ) + 1 );
    environment[used++] = variable;
    environment[used] = NULL;

    char * child_argv[2] = { program, NULL };
    int result = 1;

    pid_t pid;
    if ( posix_spawnp( &pid, program, NULL, NULL, child_argv, environment ) == 0 )
    {
        int wait_status = 0;
        if ( waitpid( pid, &wait_status, 0 ) == pid && WIFEXITED( wait_status ) && WEXITSTATUS( wait_status ) == 0 )
            result = 0;
    }

    if ( result != 0 )
        printf( "FAIL: the run with ROCKETNET_SERVER_MAX_CLIENTS=%s did not exit 0\n", value );

    free( variable );
    free( environment );
    return result;
#endif // #if defined( _WIN32 )
}

int main( int argc, char ** argv )
{
    (void) argc;

    const char * environment = getenv( "ROCKETNET_SERVER_MAX_CLIENTS" );
    if ( !environment )
    {
        environment = "";
    }

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the in-memory loopback transport keeps its endpoints in a hub the caller
    // owns, sized here for one server and one client; it is destroyed after them

    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, &hub_status );
    if ( !hub || hub_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: loopback hub create: %s\n", rocketnet_status_name( hub_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = hub;

    if ( environment[0] == '\0' )
    {
        printf( "default max_clients: %d\n", config.max_clients );
        printf( "default max_timeout_seconds: %d\n", config.max_timeout_seconds );
    }

    config.max_clients = 16;
    config.max_timeout_seconds = 90;

    // create reads the environment, which overrides the struct

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );

    int result = 1;

    if ( environment[0] == '\0' && server )
    {
        printf( "struct max_clients: %d\n", rocketnet_server_max_clients( server ) );

        // run again with the variable set: a good value, then one out of range

        const char * const values[2] = { "5", "99999" };
        int children = 0;

        for ( int i = 0; i < 2; i++ )
        {
            fflush( stdout );
            children += rerun_with_max_clients( argv[0], values[i] );
        }

        result = children == 0 ? 0 : 1;
    }
    else if ( strcmp( environment, "5" ) == 0 && server )
    {
        printf( "environment max_clients: %d\n", rocketnet_server_max_clients( server ) );
        result = rocketnet_server_max_clients( server ) == 5 ? 0 : 1;
    }
    else if ( strcmp( environment, "99999" ) == 0 && !server )
    {
        printf( "bad value refused: %s\n", rocketnet_status_name( status ) );
        result = 0;
    }
    else
    {
        printf( "FAIL: ROCKETNET_SERVER_MAX_CLIENTS=\"%s\": server create %s\n", environment, server ? "accepted it" : rocketnet_status_name( status ) );
    }

    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    if ( result != 0 )
    {
        printf( "FAIL: max_clients is the default, then the struct, then the environment\n" );
    }
    else if ( environment[0] == '\0' )
    {
        printf( "PASS\n" );
    }

    return result;
}
