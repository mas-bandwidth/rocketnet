/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Each fuzz/corpus/prefilter_* seed is named for what the prefilter does with
// it. A seed is the fuzz selector byte, then the datagram; the datagram goes
// through rocketnet_prefilter_check under the test filter value, as
// fuzz/rocketnet_fuzz.cpp does, and the outcome must be the one the name says.
// File input only: the seeds are read beside this source, no sockets, no time.

#include "test_unit_registry.h"
#include "resource_file.h"

#include "rocketnet_internal.h"
#include "rocketnet_protocol.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#define check rocketnet_unit_check

#define SEED_ACCEPTED_ANY_TYPE   1000       // accepted, with any packet type byte

struct prefilter_seed_t
{
    const char * name;
    int expected;       // a refusal code, a packet type, or SEED_ACCEPTED_ANY_TYPE
    int expected_bytes; // the size of the datagram, or -1 when the name does not say
};

static const prefilter_seed_t prefilter_seeds[] =
{
    { "prefilter_empty",          ROCKETNET_PREFILTER_TOO_SHORT,            0 },
    { "prefilter_too_short",      ROCKETNET_PREFILTER_TOO_SHORT,           -1 },
    { "prefilter_payload_near_ceiling", ROCKETNET_CONNECTION_PAYLOAD_PACKET, 1433 },
    { "prefilter_wrong_epoch",    ROCKETNET_PREFILTER_WRONG_EPOCH,         -1 },
    { "prefilter_wrong_value",    ROCKETNET_PREFILTER_WRONG_VALUE,         -1 },
    { "prefilter_reserved_type",  SEED_ACCEPTED_ANY_TYPE,                  -1 },
    { "prefilter_keep_alive_min", ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET,  -1 },
    { "prefilter_payload_full",   ROCKETNET_CONNECTION_PAYLOAD_PACKET,     -1 },
    { "prefilter_request",        ROCKETNET_CONNECTION_REQUEST_PACKET,     -1 },
};

static std::string prefilter_seed_path( const char * name )
{
    // this source is tests/<file>: the repository root is its directory's parent.
    // No "..": the Windows long-path prefix (tests/resource_file.h) turns off
    // ".." resolution, so the root is cut from the path itself.
    std::string source = __FILE__;
    size_t slash = source.find_last_of( "/\\" );
    std::string tests = ( slash == std::string::npos ) ? std::string( "." ) : source.substr( 0, slash );
    slash = tests.find_last_of( "/\\" );
    std::string root = ( slash != std::string::npos ) ? tests.substr( 0, slash ) : ( tests == "." ? std::string( ".." ) : std::string( "." ) );
    return root + "/fuzz/corpus/" + name;
}

static void test_fuzz_prefilter_seed_names_match_outcomes()
{
    const uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    for ( size_t i = 0; i < sizeof(prefilter_seeds) / sizeof(prefilter_seeds[0]); i++ )
    {
        const prefilter_seed_t & seed = prefilter_seeds[i];
        FILE * file = rocketnet_test_fopen( prefilter_seed_path( seed.name ), "rb" );
        if ( !file )
            printf( "fuzz prefilter seed %s is missing\n", seed.name );
        check( file != NULL );
        std::vector<uint8_t> bytes;
        uint8_t chunk[512];
        size_t got;
        while ( ( got = fread( chunk, 1, sizeof(chunk), file ) ) > 0 )
            bytes.insert( bytes.end(), chunk, chunk + got );
        fclose( file );

        check( !bytes.empty() );                            // the fuzz selector byte
        const int datagram_bytes = (int) bytes.size() - 1;  // the datagram follows it
        std::vector<uint8_t> datagram( bytes.begin() + 1, bytes.end() );
        datagram.push_back( 0 );                            // a readable byte for the empty datagram
        const int result = rocketnet_prefilter_check( datagram.data(), datagram_bytes, packet_filter );

        const bool matches = seed.expected == SEED_ACCEPTED_ANY_TYPE
            ? result > ROCKETNET_CONNECTION_FRAGMENT_PACKET
            : result == seed.expected;
        if ( !matches || ( seed.expected_bytes >= 0 && datagram_bytes != seed.expected_bytes ) )
            printf( "fuzz prefilter seed %s: %d bytes, prefilter result %d, the name says %d\n",
                    seed.name, datagram_bytes, result, seed.expected );
        check( matches );
        check( seed.expected_bytes < 0 || datagram_bytes == seed.expected_bytes );
    }
}

REGISTER_UNIT_TEST( test_fuzz_prefilter_seed_names_match_outcomes );
