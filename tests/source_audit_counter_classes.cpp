/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The counter source audits. These cases read the repository's own test and
// sdk sources through ROCKETNET_SOURCE_DIR to check that every counter is
// named by a test and that no outcome sits in more than one family. They are
// source-policy checks, not unit logic, so they live in the source-audit
// binary. Every case runs in memory except the file reads themselves (no
// directory walk, no network, no clock).

#include "test_unit_registry.h"
#include "resource_file.h"

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"
#include "source_audit_text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// the rows, read back out of the counter list

struct counters_row_t
{
    const char * name;
    int counter_class;
    int family;                 // the RECEIVED counter this outcome sums into, or ROCKETNET_COUNTER_NONE
};

static const counters_row_t counters_rows[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_COUNTER_TEST_ROW( name, klass, family ) { #name, ROCKETNET_COUNTER_CLASS_##klass, ROCKETNET_COUNTER_##family },
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_TEST_ROW )
#undef ROCKETNET_COUNTER_TEST_ROW
};

static int counters_find( const char * name )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        if ( strcmp( counters_rows[i].name, name ) == 0 )
        {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// the source audit: the counter ladders' own source

#ifndef ROCKETNET_SOURCE_DIR
#error "the build passes ROCKETNET_SOURCE_DIR to this file"
#endif // #ifndef ROCKETNET_SOURCE_DIR

static bool counters_read_file( const std::string & relative_path, std::string & contents )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
    {
        return false;
    }

    char buffer[65536];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
    {
        contents.append( buffer, n );
    }
    fclose( file );

    return true;
}

static bool counters_is_identifier_char( char c )
{
    return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

// Which counters a test observes is tools/check_counter_observed.py: it reads
// the test sources from the build (every source a test binary compiles), not
// a glob, and counts a counter only when a check reads its value. The audit
// that stood here counted any name in a globbed file, a comment's included,
// and could not see a test source outside the glob (VERIFY-3 W16, E26).

// ---------------------------------------------------------------------------
// 2: an outcome sits in at most one family

// An outcome counted from two ladders balances only while it stays zero: the
// request ladder and the response ladder both counted
// CONNECTION_REQUEST_INTERNAL_ERROR, so the two families held only while it
// was 0. The family column gives each counter one family; this test reads the
// ladders' own source and fails when a ladder counts a counter that sums into
// another family. The ladder functions named here are the ones that own a
// RECEIVED counter in their own body; the sealed types share one table-driven
// ladder in each protocol file, and a counter in a table row has one place by
// construction.

struct counters_ladder_t
{
    const char * file;
    const char * function;
    const char * received;      // the RECEIVED counter whose outcomes this function counts
};

static const counters_ladder_t counters_ladders[] = {
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_protocol_process_connection_request_known", "CONNECTION_REQUEST_RECEIVED" },
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_process_opened_request", "CONNECTION_REQUEST_RECEIVED" },
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_process_connection_response", "CONNECTION_RESPONSE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_cookie", "CLIENT_COOKIE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_challenge", "CLIENT_CHALLENGE_RECEIVED" },
    { "sdk/rocketnet_client_protocol.cpp", "rocketnet_client_process_denied", "CLIENT_DENIED_RECEIVED" },
};

// the text of a function defined at the start of a line: from its name to the
// closing brace at the start of a line

static bool counters_function_body( const std::string & text, const char * function, std::string & body )
{
    const std::string needle = std::string( " " ) + function + "(";
    size_t at = 0;
    while ( ( at = text.find( needle, at ) ) != std::string::npos )
    {
        const size_t line_start = text.rfind( '\n', at ) + 1;
        const char first = text[line_start];

        // a definition starts a line with its return type; a call or a prototype does not

        if ( first != ' ' && first != '\t' && first != '/' && first != '#' && first != '\n' )
        {
            const size_t open = text.find( "\n{\n", at );
            const size_t semicolon = text.find( ';', at );
            if ( open != std::string::npos && ( semicolon == std::string::npos || open < semicolon ) )
            {
                const size_t close = text.find( "\n}\n", open );
                if ( close == std::string::npos )
                {
                    return false;
                }
                body = text.substr( open, close - open );
                return true;
            }
        }

        at += needle.size();
    }

    return false;
}

// every counter a text counts: any ROCKETNET_COUNTER_<NAME> in the code, whatever
// follows it. Matching `] ++` alone passed the same count written `+= 1` and
// one handed to a helper (VERIFY-3 W18); a ladder body names a counter only
// to count it, so every name in it is a count. The caller passes code with
// comments and literals blanked.

static std::vector<std::string> counters_incremented( const std::string & text )
{
    std::vector<std::string> names;
    static const char prefix[] = "ROCKETNET_COUNTER_";
    const size_t prefix_bytes = sizeof( prefix ) - 1;
    size_t at = 0;
    while ( ( at = text.find( prefix, at ) ) != std::string::npos )
    {
        size_t end = at + prefix_bytes;
        while ( end < text.size() && counters_is_identifier_char( text[end] ) )
        {
            end++;
        }

        const bool whole_name = at == 0 || !counters_is_identifier_char( text[at - 1] );
        if ( whole_name && end > at + prefix_bytes )
        {
            names.push_back( text.substr( at + prefix_bytes, end - at - prefix_bytes ) );
        }

        at = end;
    }

    return names;
}

static void test_counters_every_outcome_in_at_most_one_family()
{
    int shared = 0;

    for ( size_t i = 0; i < sizeof( counters_ladders ) / sizeof( counters_ladders[0] ); i++ )
    {
        const counters_ladder_t & ladder = counters_ladders[i];

        std::string text;
        check( counters_read_file( ladder.file, text ) );

        std::string body;
        if ( !counters_function_body( text, ladder.function, body ) )
        {
            printf( "ladder function not found: %s in %s\n", ladder.function, ladder.file );
        }
        check( counters_function_body( text, ladder.function, body ) );

        const int received = counters_find( ladder.received );
        check( received >= 0 );

        const std::vector<std::string> counted = counters_incremented( source_audit_code_only( body ) );

        // a function that counts nothing is a function this audit lost

        check( !counted.empty() );

        for ( size_t j = 0; j < counted.size(); j++ )
        {
            const int counter = counters_find( counted[j].c_str() );
            check( counter >= 0 );

            const int family = counters_rows[counter].family;
            if ( counter == received || family == ROCKETNET_COUNTER_NONE || family == received )
            {
                continue;
            }
            printf( "ROCKETNET_COUNTER_%s is counted in %s (the %s ladder) but sums into %s\n",
                    counted[j].c_str(), ladder.function, ladder.received, counters_rows[family].name );
            shared++;
        }
    }

    check( shared == 0 );

    // the two connection ladders each have an internal error of their own

    check( counters_rows[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR].family == ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED );
    check( counters_rows[ROCKETNET_COUNTER_CONNECTION_RESPONSE_INTERNAL_ERROR].family == ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED );
}

REGISTER_UNIT_TEST( test_counters_every_outcome_in_at_most_one_family );

// ---------------------------------------------------------------------------
// 3: every server process outcome lives with its siblings, documented

// ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED is one of the outcomes returned by
// the request ladder, so it belongs beside the other ROCKETNET_SERVER_PROCESS_*
// defines and in their comments, not in the counters header. This reads the
// two headers and fails when the define is out of place or undocumented.

static std::string counters_trim( const std::string & text )
{
    const size_t first = text.find_first_not_of( " \t\r" );
    if ( first == std::string::npos )
    {
        return "";
    }
    const size_t last = text.find_last_not_of( " \t\r" );
    return text.substr( first, last - first + 1 );
}

static std::vector<std::string> counters_lines( const std::string & text )
{
    std::vector<std::string> lines;
    size_t start = 0;
    for ( ;; )
    {
        const size_t end = text.find( '\n', start );
        lines.push_back( text.substr( start, end == std::string::npos ? std::string::npos : end - start ) );
        if ( end == std::string::npos )
        {
            break;
        }
        start = end + 1;
    }
    return lines;
}

static int counters_line_with( const std::vector<std::string> & lines, const char * needle )
{
    for ( size_t i = 0; i < lines.size(); i++ )
    {
        if ( lines[i].find( needle ) != std::string::npos )
        {
            return (int) i;
        }
    }
    return -1;
}

// The // comment block immediately above a line, allowing one blank line
// between the comment and the declaration it documents.

static std::string counters_comment_above( const std::vector<std::string> & lines, int line )
{
    std::string comment;
    int i = line - 1;
    if ( i >= 0 && counters_trim( lines[(size_t) i] ).empty() )
    {
        i--;
    }
    while ( i >= 0 )
    {
        const std::string trimmed = counters_trim( lines[(size_t) i] );
        if ( trimmed.compare( 0, 2, "//" ) != 0 )
        {
            break;
        }
        comment = trimmed + "\n" + comment;
        i--;
    }
    return comment;
}

static void test_server_process_outcomes_budget_exceeded_with_siblings()
{
    std::string protocol_text;
    std::string counters_text;
    check( counters_read_file( "sdk/rocketnet_server_protocol.h", protocol_text ) );
    check( counters_read_file( "sdk/rocketnet_counters.h", counters_text ) );

    const std::vector<std::string> protocol_lines = counters_lines( protocol_text );

    // the define is in the protocol header, with value 6

    const int define_line = counters_line_with( protocol_lines, "#define ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED" );
    if ( define_line < 0 )
    {
        printf( "ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED is not defined in sdk/rocketnet_server_protocol.h\n" );
    }
    check( define_line >= 0 );

    const char * const define_text = protocol_lines[(size_t) define_line].c_str();
    const char * const name = strstr( define_text, "ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED" );
    check( name != NULL );
    const char * const after_name = name + strlen( "ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED" );
    char * after_value = NULL;
    const long value = strtol( after_name, &after_value, 10 );
    check( after_value != after_name );
    check( value == 6 );

    // it is no longer in the counters header

    const int counters_define = counters_line_with( counters_lines( counters_text ),
                                                    "#define ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED" );
    if ( counters_define >= 0 )
    {
        printf( "ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED is still defined in sdk/rocketnet_counters.h\n" );
    }
    check( counters_define < 0 );

    // the outcome list comment names it

    const int ignored_line = counters_line_with( protocol_lines, "#define ROCKETNET_SERVER_PROCESS_IGNORED" );
    check( ignored_line >= 0 );
    const std::string outcome_comment = counters_comment_above( protocol_lines, ignored_line );
    if ( outcome_comment.find( "BUDGET_EXCEEDED" ) == std::string::npos )
    {
        printf( "the ROCKETNET_SERVER_PROCESS_* outcome list comment does not mention BUDGET_EXCEEDED\n" );
    }
    check( outcome_comment.find( "BUDGET_EXCEEDED" ) != std::string::npos );

    // the request entry's doc comment names it

    const int request_line = counters_line_with( protocol_lines, "rocketnet_server_protocol_process_connection_request(" );
    check( request_line >= 0 );
    const std::string request_comment = counters_comment_above( protocol_lines, request_line );
    if ( request_comment.find( "BUDGET_EXCEEDED" ) == std::string::npos )
    {
        printf( "the request entry's doc comment does not mention BUDGET_EXCEEDED\n" );
    }
    check( request_comment.find( "BUDGET_EXCEEDED" ) != std::string::npos );
}

REGISTER_UNIT_TEST( test_server_process_outcomes_budget_exceeded_with_siblings );
