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
        if (end - in == 1)
        {
            *pos++ = base64_table_encode[(in[0] & 0x03) << 4];
            *pos++ = '=';
        }
        else
        {
            *pos++ = base64_table_encode[((in[0] & 0x03) << 4) | (in[1] >> 4)];
            *pos++ = base64_table_encode[(in[1] & 0x0f) << 2];
        }
        *pos++ = '=';
    }

    output[output_length] = '\0';

    return int( output_length );
}

// strict decode (#110 review, both readers independently): the old table
// indexed by a plain (signed) char, so any byte >= 0x80 read below the
// table -- an out-of-bounds read steered by attacker-controlled input --
// and every unknown character silently decoded as SOMETHING, which for the
// config system's key fields meant a corrupted env var could become a
// wrong key without a word. Refuse-loudly is the law, and doubly so for
// secret material: an invalid character, misplaced padding, or an
// impossible length now refuses the whole decode (returns 0), and every
// lookup is by value range, never by table index. ONE alphabet, the
// standard one: the url-safe aliases were dropped on the #110 gate so
// that two distinct strings can never decode to the same bytes -- with
// aliases, "+w" and "-w" were the same key twice.

static int rocketnet_base64_char_value( char c )
{
    if ( c >= 'A' && c <= 'Z' ) return c - 'A';
    if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
    if ( c >= '0' && c <= '9' ) return c - '0' + 52;
    if ( c == '+' ) return 62;
    if ( c == '/' ) return 63;
    return -1;
}

int rocketnet_base64_decode_data( const char * input, uint8_t * output, size_t output_size )
{
    rocketnet_assert( input );
    rocketnet_assert( output );
    rocketnet_assert( output_size > 0 );

    const size_t input_length = strlen( input );

    // padding is REQUIRED (ruled on the #110 gate, round five): every
    // input is whole four-character groups, with one or two trailing '='
    // completing the last -- so each byte length has exactly one valid
    // spelling and the no-two-spellings-one-key property is real, not
    // aspirational ("AA" is refused beside "AA==")

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

    // canonical tails only: leftover bits below the last whole byte must
    // be zero, so two distinct strings can never decode to the same bytes
    // (without it "AB" and "AC" would both yield 0x00)

    if ( bits > 0 && ( accumulator & ( ( 1u << bits ) - 1 ) ) != 0 )
        return 0;

    rocketnet_assert( j == output_length );

    return int( output_length );
}

int rocketnet_base64_encode_string( const char * input, char * output, size_t output_size )
{
    rocketnet_assert( input );
    rocketnet_assert( output );
    rocketnet_assert( output_size > 0 );

    return rocketnet_base64_encode_data( (const uint8_t *)( input ), strlen( input ), output, output_size );
}

int rocketnet_base64_decode_string( const char * input, char * output, size_t output_size )
{
    rocketnet_assert( input );
    rocketnet_assert( output );
    rocketnet_assert( output_size > 0 );

    if ( output_size < 2 )
        return 0;       // room for nothing but the terminator; decode_data requires a real buffer

    int output_length = rocketnet_base64_decode_data( input, (uint8_t *)( output ), output_size - 1 );

    output[output_length] = '\0';

    return output_length;
}
