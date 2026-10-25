/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every XDP counter has a case. Each ROCKETNET_SERVER_XDP_COUNTER_* that
// sdk/rocketnet_wire.h defines (other than the count) must appear in
// tests/test_unit_xdp_filter_host.cpp as a row's XDP_HOST_COUNTS(...), so a
// new counter cannot land without a case that makes it move. It reads the two
// files through ROCKETNET_SOURCE_DIR, so it is a source-policy check and runs
// in the source-audit binary.

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_unit_registry.h"
#include "portable_crt.h"
#include "../sdk/rocketnet_wire.h"

static std::string xdp_host_read_file( const char * relative )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    std::string text;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( file == NULL )
    {
        printf( "test_xdp_transport_host_every_counter_has_a_case: cannot read %s\n", path.c_str() );
        fflush( stdout );
        return text;
    }
    char buffer[4096];
    size_t got;
    while ( ( got = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        text.append( buffer, got );
    fclose( file );
    return text;
}

static void test_xdp_transport_host_every_counter_has_a_case()
{
    const std::string wire = xdp_host_read_file( "sdk/rocketnet_wire.h" );
    const std::string self = xdp_host_read_file( "tests/test_unit_xdp_filter_host.cpp" );
    rocketnet_unit_check( !wire.empty() );
    rocketnet_unit_check( !self.empty() );

    const std::string define = "#define ROCKETNET_SERVER_XDP_COUNTER_";
    std::vector<std::string> names;
    size_t at = 0;
    while ( ( at = wire.find( define, at ) ) != std::string::npos )
    {
        size_t end = at + strlen( "#define " );
        size_t stop = end;
        while ( stop < wire.size() && ( isalnum( (unsigned char) wire[stop] ) || wire[stop] == '_' ) )
            stop++;
        names.push_back( wire.substr( end, stop - end ) );
        at = stop;
    }

    rocketnet_unit_check( (int) names.size() == ROCKETNET_SERVER_XDP_NUM_COUNTERS );

    int missing = 0;
    for ( size_t i = 0; i < names.size(); i++ )
    {
        const std::string needle = std::string( "XDP_HOST_" ) + "COUNTS( " + names[i] + " )";
        if ( self.find( needle ) == std::string::npos )
        {
            printf( "test_xdp_transport_host_every_counter_has_a_case: %s has no case\n", names[i].c_str() );
            fflush( stdout );
            missing++;
        }
    }
    rocketnet_unit_check( missing == 0 );
}

REGISTER_UNIT_TEST( test_xdp_transport_host_every_counter_has_a_case );
