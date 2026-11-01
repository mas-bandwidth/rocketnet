/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A unit case's environment: an in-memory table the case owns, handed to the
// SDK as an environment source (sdk/rocketnet_env.h) the way time is handed
// in as an argument. A case declares one, sets the variables it means to
// test, and passes &env.source to the reader under test
// (rocketnet_config_apply_env, rocketnet_client_config_resolve,
// rocketnet_client_create_with_env, rocketnet_init_ex_with_env and the rest).
// Nothing here reads or changes the process environment, and nothing is
// shared between tables. An empty value is stored present and empty, so a
// case reaches the set-but-empty branch on every platform.

#pragma once

#ifndef ROCKETNET_TESTS_TEST_ENV_SOURCE_H
#define ROCKETNET_TESTS_TEST_ENV_SOURCE_H

#include <stddef.h>
#include <string.h>
#include <map>
#include <string>
#include "../sdk/rocketnet_env.h"

struct rocketnet_test_env_t
{
    std::map<std::string, std::string> values;
    struct rocketnet_env_source_t source;

    rocketnet_test_env_t()
    {
        source.get = get_value;
        source.context = this;
    }

    // source.context points here, so a table is never copied
    rocketnet_test_env_t( const rocketnet_test_env_t & ) = delete;
    rocketnet_test_env_t & operator=( const rocketnet_test_env_t & ) = delete;

    void set( const char * name, const char * value )
    {
        values[name] = value;
    }

    void unset( const char * name )
    {
        values.erase( name );
    }

    void clear()
    {
        values.clear();
    }

    bool is_set( const char * name ) const
    {
        return values.find( name ) != values.end();
    }

    // the source's contract: 1 set and copied, 0 unset, -1 too long for size
    static int get_value( void * context, const char * name, char * buffer, size_t size )
    {
        const rocketnet_test_env_t * env = (const rocketnet_test_env_t *) context;
        buffer[0] = '\0';
        const std::map<std::string, std::string>::const_iterator entry = env->values.find( name );
        if ( entry == env->values.end() )
            return 0;
        if ( entry->second.size() >= size )
            return -1;
        memcpy( buffer, entry->second.c_str(), entry->second.size() + 1 );
        return 1;
    }
};

#endif // ROCKETNET_TESTS_TEST_ENV_SOURCE_H
