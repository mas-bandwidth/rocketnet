/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Lexical inventory only: runtime coverage is established by the registered
// host-filter unit that executes every registered case and checks counters.

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

static std::string xdp_host_lexical_code( const std::string & source )
{
    std::string code = source;
    bool line_comment = false;
    bool block_comment = false;
    bool string_literal = false;
    bool char_literal = false;
    bool escaped = false;
    for ( size_t i = 0; i < code.size(); i++ )
    {
        const char c = code[i];
        const char next = i + 1 < code.size() ? code[i + 1] : '\0';
        if ( line_comment )
        {
            if ( c == '\n' )
                line_comment = false;
            else
                code[i] = ' ';
            continue;
        }
        if ( block_comment )
        {
            if ( c == '*' && next == '/' )
            {
                code[i] = ' ';
                code[++i] = ' ';
                block_comment = false;
            }
            else if ( c != '\n' )
                code[i] = ' ';
            continue;
        }
        if ( string_literal || char_literal )
        {
            if ( c != '\n' )
                code[i] = ' ';
            if ( escaped )
                escaped = false;
            else if ( c == '\\' )
                escaped = true;
            else if ( ( string_literal && c == '"' ) || ( char_literal && c == '\'' ) )
            {
                string_literal = false;
                char_literal = false;
            }
            continue;
        }
        if ( c == '/' && next == '/' )
        {
            code[i] = ' ';
            code[++i] = ' ';
            line_comment = true;
        }
        else if ( c == '/' && next == '*' )
        {
            code[i] = ' ';
            code[++i] = ' ';
            block_comment = true;
        }
        else if ( c == '"' )
        {
            code[i] = ' ';
            string_literal = true;
        }
        else if ( c == '\'' )
        {
            code[i] = ' ';
            char_literal = true;
        }
    }

    std::string active;
    int disabled_depth = 0;
    size_t line_start = 0;
    while ( line_start < code.size() )
    {
        size_t line_end = code.find( '\n', line_start );
        if ( line_end == std::string::npos )
            line_end = code.size();
        const std::string line = code.substr( line_start, line_end - line_start );
        const size_t first = line.find_first_not_of( " \t\r" );
        const std::string directive = first == std::string::npos ? std::string() : line.substr( first );
        if ( directive.compare( 0, 6, "#if 0" ) == 0 )
            disabled_depth++;
        else if ( disabled_depth > 0 && directive.compare( 0, 3, "#if" ) == 0 )
            disabled_depth++;
        else if ( disabled_depth > 0 && directive.compare( 0, 6, "#endif" ) == 0 )
            disabled_depth--;
        else if ( disabled_depth == 0 )
        {
            active.append( line );
            active.push_back( '\n' );
        }
        line_start = line_end + 1;
    }
    return active;
}

static void test_xdp_transport_host_every_counter_has_a_case()
{
    const std::string wire = xdp_host_lexical_code( xdp_host_read_file( "sdk/rocketnet_wire.h" ) );
    const std::string self = xdp_host_lexical_code( xdp_host_read_file( "tests/test_unit_xdp_filter_host.cpp" ) );
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
        const std::string needle = std::string( "XDP_HOST_" ) + "COUNTS";
        bool present = false;
        for ( size_t occurrence = self.find( needle ); occurrence != std::string::npos;
              occurrence = self.find( needle, occurrence + needle.size() ) )
        {
            const size_t open = self.find( '(', occurrence + needle.size() );
            const size_t close = open == std::string::npos ? std::string::npos : self.find( ')', open + 1 );
            if ( close == std::string::npos )
                continue;
            const size_t token = self.find( names[i], open + 1 );
            if ( token != std::string::npos && token < close &&
                 ( token == open + 1 || !( isalnum( (unsigned char) self[token - 1] ) || self[token - 1] == '_' ) ) &&
                 ( token + names[i].size() == close ||
                   !( isalnum( (unsigned char) self[token + names[i].size()] ) || self[token + names[i].size()] == '_' ) ) )
            {
                present = true;
                break;
            }
        }
        if ( !present )
        {
            printf( "test_xdp_transport_host_every_counter_has_a_case: %s has no case\n", names[i].c_str() );
            fflush( stdout );
            missing++;
        }
    }
    rocketnet_unit_check( missing == 0 );
}

REGISTER_UNIT_TEST( test_xdp_transport_host_every_counter_has_a_case );
