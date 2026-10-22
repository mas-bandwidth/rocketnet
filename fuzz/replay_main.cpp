/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A libFuzzer-free replay transport for the fuzz harnesses. libFuzzer, given
// file arguments, reads each file whole and calls LLVMFuzzerTestOneInput
// once per file; this main does the same, in argument order, so a corpus
// replay runs where no libFuzzer runtime exists (Apple Clang, MSVC).
//
// The harnesses keep no state across inputs and read no clock or
// randomness, so the count is all this transport reports. Each harness's
// LLVMFuzzerInitialize is called first, as libFuzzer calls it.
//
// THE STANDALONE MODE, yojimbo's FUZZ_STANDALONE driver: with no paths named,
// it feeds FUZZ_ITERS pseudo-random inputs (default 200000) of up to 4096
// bytes from a fixed linear congruential stream (FUZZ_SEED, default
// 0x1234567), so a sanitized build shakes out crashes where no libFuzzer
// runtime exists. The stream is fixed, so a crash in this mode reproduces
// with the same two variables.
//
// Exit 1 when a file cannot be read; exit 0 after printing "replayed <n>
// inputs" (or "random <n> inputs").

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "../test/portable_crt.h"

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv );
extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size );

static bool replay_file( const char * path )
{
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( file == NULL )
    {
        fprintf( stderr, "fuzz replay: cannot open %s\n", path );
        return false;
    }

    if ( fseek( file, 0, SEEK_END ) != 0 )
    {
        fprintf( stderr, "fuzz replay: cannot seek %s\n", path );
        fclose( file );
        return false;
    }

    const long length = ftell( file );
    if ( length < 0 )
    {
        fprintf( stderr, "fuzz replay: cannot size %s\n", path );
        fclose( file );
        return false;
    }

    if ( fseek( file, 0, SEEK_SET ) != 0 )
    {
        fprintf( stderr, "fuzz replay: cannot rewind %s\n", path );
        fclose( file );
        return false;
    }

    // one byte stays allocated for an empty file, so the harness always
    // receives a readable pointer, then the exact length it holds
    std::vector<uint8_t> buffer( length > 0 ? (size_t) length : 1 );
    if ( length > 0 && fread( buffer.data(), 1, (size_t) length, file ) != (size_t) length )
    {
        fprintf( stderr, "fuzz replay: cannot read %s\n", path );
        fclose( file );
        return false;
    }

    fclose( file );

    LLVMFuzzerTestOneInput( buffer.data(), (size_t) length );
    return true;
}

int main( int argc, char ** argv )
{
    if ( LLVMFuzzerInitialize( &argc, &argv ) != 0 )
    {
        fprintf( stderr, "fuzz replay: harness initialization failed\n" );
        return 1;
    }

    if ( argc < 2 )
    {
        long iterations = 200000;
        unsigned int seed = 0x1234567u;
        const char * iterations_text = getenv( "FUZZ_ITERS" );
        const char * seed_text = getenv( "FUZZ_SEED" );
        if ( iterations_text )
            iterations = atol( iterations_text );
        if ( seed_text )
            seed = (unsigned int) strtoul( seed_text, NULL, 0 );
        static uint8_t buffer[4096];
        for ( long i = 0; i < iterations; ++i )
        {
            seed = seed * 1103515245u + 12345u;
            const size_t length = ( seed >> 8 ) % ( sizeof( buffer ) + 1 );
            for ( size_t j = 0; j < length; ++j )
            {
                seed = seed * 1103515245u + 12345u;
                buffer[j] = (uint8_t) ( seed >> 16 );
            }
            LLVMFuzzerTestOneInput( buffer, length );
        }
        printf( "random %ld inputs\n", iterations );
        return 0;
    }

    for ( int i = 1; i < argc; ++i )
    {
        if ( !replay_file( argv[i] ) )
            return 1;
    }

    printf( "replayed %d inputs\n", argc - 1 );
    return 0;
}
