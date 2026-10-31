/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The environment source: where the SDK reads its environment overrides.
// Every SDK reader takes one as an argument, the way it takes time: the
// public entry points pass the process environment, and a unit case passes
// an in-memory table it owns, so no case reads or changes process state.
// The Linux socket layer (the XDP gateway address and two functional fault
// hooks) reads rocketnet_env_process directly: it runs only on real sockets,
// which no unit case opens. This header includes nothing from the platform
// layer, so a test helper can name the type without the platform headers.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_ENV_H
#define ROCKETNET_SDK_ROCKETNET_ENV_H

#include <stddef.h>

// get copies the value of name into buffer and returns 1 when it is set, 0
// when it is unset, or -1 when the value does not fit in size bytes
// (terminator included). size is at least 1.

struct rocketnet_env_source_t
{
    int ( *get )( void * context, const char * name, char * buffer, size_t size );
    void * context;
};

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

// the process environment, read through rocketnet_platform_getenv

const struct rocketnet_env_source_t * rocketnet_env_process( void );

// one read through env, with get's 1 / 0 / -1 answer; buffer is left empty
// unless the answer is 1

int rocketnet_env_get( const struct rocketnet_env_source_t * env, const char * name, char * buffer, size_t size );

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // ROCKETNET_SDK_ROCKETNET_ENV_H
