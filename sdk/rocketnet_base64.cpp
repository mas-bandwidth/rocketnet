/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_base64.h"
#include <limits.h>
#include <string.h>

static const char base64_table_encode[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int rocketnet_base64_encode_data( const uint8_t * input, size_t input_length, char * output, size_t output_size )
{
    rocketnet_assert( input );
    rocketnet_assert( output );
    rocketnet_assert( output_size > 0 );

    char * pos;
    const uint8_t * end;
    const uint8_t * in;

    size_t output_length = 4 * ( ( input_length + 2 ) / 3 ); // 3-byte blocks to 4-byte

    if ( output_length < input_length )
    {
        return -1; // integer overflow
    }

    if ( output_length > (size_t) INT_MAX )
    {
        return -1; // the encoded length would not fit the int return
    }

    if ( output_length >= output_size )
    {
        return -1; // not enough room in output buffer
    }

    end = input + input_length;
    in = input;
    pos = output;

    while ( end - in >= 3 )
    {
        *pos++ = base64_table_encode[in[0] >> 2];
        *pos++ = base64_table_encode[( ( in[0] & 0x03 ) << 4 ) | ( in[1] >> 4 )];
        *pos++ = base64_table_encode[( ( in[1] & 0x0f ) << 2 ) | ( in[2] >> 6 )];
        *pos++ = base64_table_encode[in[2] & 0x3f];
        in += 3;
    }

    if ( end - in )
    {
        *pos++ = base64_table_encode[in[0] >> 2];

        if ( end - in == 1 )
        {
            *pos++ = base64_table_encode[( in[0] & 0x03 ) << 4];
            *pos++ = '=';
        }
        else
        {
            *pos++ = base64_table_encode[( ( in[0] & 0x03 ) << 4 ) | ( in[1] >> 4 )];
            *pos++ = base64_table_encode[( in[1] & 0x0f ) << 2];
        }

        *pos++ = '=';
    }

    output[output_length] = '\0';

    return int( output_length );
}

// strict decode: one alphabet, no invalid characters, padding required and
// tails canonical, so two distinct strings never decode to the same key.

static int rocketnet_base64_char_value( char c )
{
    if ( c >= 'A' && c <= 'Z' )
        return c - 'A';
    if ( c >= 'a' && c <= 'z' )
        return c - 'a' + 26;
    if ( c >= '0' && c <= '9' )
        return c - '0' + 52;
    if ( c == '+' )
        return 62;
    if ( c == '/' )
        return 63;

    return -1;
}

int rocketnet_base64_decode_data( const char * input, uint8_t * output, size_t output_size )
{
    rocketnet_assert( input );
    rocketnet_assert( output );
    rocketnet_assert( output_size > 0 );

    const size_t input_length = strlen( input );

    // whole four-character groups only, so each length has one spelling

    if ( ( input_length % 4 ) != 0 )
        return 0;

    size_t data_length = input_length;
    int pad_chars = 0;

    while ( data_length > 0 && input[data_length - 1] == '=' )
    {
        data_length--;
        pad_chars++;
    }

    if ( pad_chars > 2 )
        return 0;

    size_t output_length = ( data_length / 4 ) * 3;

    if ( ( data_length % 4 ) == 2 )
        output_length += 1;
    else if ( ( data_length % 4 ) == 3 )
        output_length += 2;

    if ( output_length > output_size )
        return 0;

    if ( output_length > (size_t) INT_MAX )
        return 0;       // the decoded length would not fit the int return

    size_t j = 0;
    uint32_t accumulator = 0;
    int bits = 0;

    for ( size_t i = 0; i < data_length; i++ )
    {
        const int value = rocketnet_base64_char_value( input[i] );
        if ( value < 0 )
            return 0;

        accumulator = ( accumulator << 6 ) | (uint32_t) value;
        bits += 6;
        if ( bits >= 8 )
        {
            bits -= 8;
            output[j++] = (uint8_t) ( ( accumulator >> bits ) & 0xFF );
        }
    }

    // leftover bits must be zero (otherwise "AB" and "AC" decode alike)

    if ( bits > 0 && ( accumulator & ( ( 1u << bits ) - 1 ) ) != 0 )
        return 0;

    rocketnet_assert( j == output_length );

    return int( output_length );
}
