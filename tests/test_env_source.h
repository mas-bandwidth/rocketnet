/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include "../sdk/rocketnet_platform.h"

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

static inline int rocketnet_test_env_getenv( const char * name, char * buffer, size_t size )
{
    return rocketnet_platform_getenv( name, buffer, size );
}

static inline int rocketnet_test_env_setenv( const char * name, const char * value, int overwrite )
{
    if ( !overwrite )
    {
        char probe[1];
        if ( rocketnet_platform_getenv( name, probe, sizeof( probe ) ) != 0 )
            return 0;
    }
    rocketnet_platform_test_environment_set( name, value );
    return 0;
}

static inline int rocketnet_test_env_unsetenv( const char * name )
{
    rocketnet_platform_test_environment_unset( name );
    return 0;
}

static inline int rocketnet_test_env_putenv_s( const char * name, const char * value )
{
    if ( value[0] )
        rocketnet_platform_test_environment_set( name, value );
    else
        rocketnet_platform_test_environment_unset( name );
    return 0;
}

static inline void rocketnet_test_env_restore()
{
    rocketnet_platform_test_environment_reset();
}
