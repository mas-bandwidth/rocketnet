/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_BASE64_H
#define ROCKETNET_SDK_ROCKETNET_BASE64_H

#include "rocketnet_internal.h"

int rocketnet_base64_encode_data( const uint8_t * input, size_t input_length, char * output, size_t output_size );

int rocketnet_base64_decode_data( const char * input, uint8_t * output, size_t output_size );

#endif // ROCKETNET_SDK_ROCKETNET_BASE64_H
