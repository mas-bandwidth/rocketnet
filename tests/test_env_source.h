/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include <stddef.h>
#include <stdlib.h>
#include "../sdk/rocketnet.h"

// Keep this helper independent of rocketnet_platform.h. On Windows that
// header brings in the SDK's platform headers, whose RPC compatibility macros
// collide with ordinary unit-local names such as small and medium.
extern "C"
{
int rocketnet_platform_getenv( const char * var, char * buffer, size_t size );
#if ROCKETNET_DEV
void rocketnet_platform_test_environment_install( void );
void rocketnet_platform_test_environment_set( const char * name, const char * value );
void rocketnet_platform_test_environment_unset( const char * name );
void rocketnet_platform_test_environment_reset( void );
#endif // #if ROCKETNET_DEV
}

#if ROCKETNET_DEV
// Every unit translation unit that changes configuration includes this file.
// These initializers run before main and install the shared empty table. The
// operation is idempotent, so initialization order cannot clear another file's
// setup, and the runner's real process environment remains under its own control.
struct rocketnet_test_environment_bootstrap_t
{
    rocketnet_test_environment_bootstrap_t()
    {
        rocketnet_platform_test_environment_install();
    }
};

static rocketnet_test_environment_bootstrap_t rocketnet_test_environment_bootstrap;

#endif // #if ROCKETNET_DEV

static inline int rocketnet_test_env_getenv( const char * name, char * buffer, size_t size )
{
    return rocketnet_platform_getenv( name, buffer, size );
}

#if ROCKETNET_DEV
static inline int rocketnet_test_env_setenv( const char * name, const char * value, int overwrite )
{
    if ( !overwrite )
    {
        char probe[1];
        if ( rocketnet_platform_getenv( name, probe, sizeof( probe ) ) != 0 )
            return 0;
    }
    // An empty value is stored present and empty on every platform, so a
    // case can reach the set-but-empty branch that the Windows CRT cannot.
    rocketnet_platform_test_environment_set( name, value );
    return 0;
}

static inline int rocketnet_test_env_unsetenv( const char * name )
{
    rocketnet_platform_test_environment_unset( name );
    return 0;
}

static inline void rocketnet_test_env_restore()
{
    rocketnet_platform_test_environment_reset();
}

#else // #if ROCKETNET_DEV

// DEV=0 has no in-memory overlay: cases that exercise the real environment
// read set the process environment directly. The runner clears every config
// name before each case (tests/test.cpp), so cases stay isolated.
static inline int rocketnet_test_env_setenv( const char * name, const char * value, int overwrite )
{
#if defined(_WIN32)
    if ( !overwrite )
    {
        char * existing = NULL;
        const int query_status = _dupenv_s( &existing, NULL, name );
        const int present = query_status == 0 && existing != NULL;
        free( existing );
        if ( query_status != 0 )
            return query_status;
        if ( present )
            return 0;
    }
    return _putenv_s( name, value );
#else
    if ( !overwrite && getenv( name ) )
        return 0;
    return setenv( name, value, 1 );
#endif
}

static inline int rocketnet_test_env_unsetenv( const char * name )
{
#if defined(_WIN32)
    return _putenv_s( name, "" );
#else
    return unsetenv( name );
#endif
}

static inline void rocketnet_test_env_restore()
{
}

#endif // #if ROCKETNET_DEV
