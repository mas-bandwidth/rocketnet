/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#ifndef SHARED_BASE64_H
#define SHARED_BASE64_H

#include <stdint.h>
#include <stddef.h>

int shared_base64_decode_data( const char * input, uint8_t * output, size_t output_size );

int shared_base64_decode_string( const char * input, char * output, size_t output_size );

#endif // #ifndef SHARED_BASE64_H
