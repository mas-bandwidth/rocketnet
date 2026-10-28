/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The C runtime calls the tests, the functional suite and the tools need,
// in a form every compiler accepts with warnings as errors. MSVC deprecates
// getenv and fopen (C4996) in favour of getenv_s and fopen_s; these helpers
// use those there and the ISO functions everywhere else, so no target turns
// the deprecation off. C and C++.

#pragma once

#ifndef ROCKETNET_TESTS_PORTABLE_CRT_H
#define ROCKETNET_TESTS_PORTABLE_CRT_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Copies the environment variable into buffer and returns 1, or returns 0,
// with buffer empty, when it is unset or its value does not fit in size bytes
// (NUL included). size must be at least 1.

static inline int rocketnet_test_getenv( const char * name, char * buffer, size_t size )
{
    buffer[0] = '\0';
#if defined( _MSC_VER )
    size_t length = 0;
    if ( getenv_s( &length, buffer, size, name ) != 0 || length == 0 )
    {
        buffer[0] = '\0';
        return 0;
    }

    return 1;
#else // #if defined( _MSC_VER )
    const char * value = getenv( name );
    if ( value == NULL )
        return 0;

    const size_t length = strlen( value );
    if ( length + 1 > size )
        return 0;
    memcpy( buffer, value, length + 1 );
    return 1;
#endif // #if defined( _MSC_VER )
}

// fopen, through fopen_s on MSVC: the open file, or NULL.

static inline FILE * rocketnet_test_fopen( const char * path, const char * mode )
{
#if defined( _MSC_VER )
    FILE * file = NULL;
    if ( fopen_s( &file, path, mode ) != 0 )
        return NULL;
    return file;
#else // #if defined( _MSC_VER )
    return fopen( path, mode );
#endif // #if defined( _MSC_VER )
}

#endif // ROCKETNET_TESTS_PORTABLE_CRT_H
