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
        if ( strcmp( counters_rows[i].name, name ) == 0 )
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// the source audit: which counters do the tests name, which have a code path

#ifndef ROCKETNET_SOURCE_DIR
#error "the build passes ROCKETNET_SOURCE_DIR to this file"
#endif
#ifndef ROCKETNET_COUNTER_TEST_SOURCES
#error "the build passes ROCKETNET_COUNTER_TEST_SOURCES to this file"
#endif
#ifndef ROCKETNET_COUNTER_SDK_SOURCES
#error "the build passes ROCKETNET_COUNTER_SDK_SOURCES to this file"
#endif

static std::vector<std::string> counters_split_list( const char * list )
{
    std::vector<std::string> out;
    std::string item;
    for ( const char * c = list;; c++ )
    {
        if ( *c == '|' || *c == '\0' )
        {
            if ( !item.empty() )
                out.push_back( item );
            item.clear();
            if ( *c == '\0' )
                break;
        }
        else
            item += *c;
    }
    return out;
}

static bool counters_read_file( const std::string & relative_path, std::string & contents )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
        return false;
    char buffer[65536];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        contents.append( buffer, n );
    fclose( file );
    return true;
}

static bool counters_is_identifier_char( char c )
{
    return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

// every whole-word ROCKETNET_COUNTER_<NAME> in a text, as the bare <NAME>

static void counters_collect_names( const std::string & text, std::set<std::string> & names )
{
    static const char prefix[] = "ROCKETNET_COUNTER_";
    const size_t prefix_bytes = sizeof( prefix ) - 1;
    size_t at = 0;
    while ( ( at = text.find( prefix, at ) ) != std::string::npos )
    {
        const bool starts_word = at == 0 || !counters_is_identifier_char( text[at - 1] );
        size_t end = at + prefix_bytes;
        while ( end < text.size() && counters_is_identifier_char( text[end] ) )
            end++;
        if ( starts_word && end > at + prefix_bytes )
            names.insert( text.substr( at + prefix_bytes, end - at - prefix_bytes ) );
        at = end;
    }
}

// the counters that no test source names. this file is the audit and is not
// a witness: a name that appears only here is not tested.

static std::set<std::string> counters_named_by_tests()
{
    std::set<std::string> names;
    const std::vector<std::string> files = counters_split_list( ROCKETNET_COUNTER_TEST_SOURCES );
    check( !files.empty() );
    for ( size_t i = 0; i < files.size(); i++ )
    {
        if ( files[i] == "tests/test_unit_counter_classes.cpp" )
            continue;
        std::string text;
        check( counters_read_file( files[i], text ) );
        counters_collect_names( text, names );
    }
    return names;
}

// the counters the sdk counts somewhere other than the counter list itself

static std::set<std::string> counters_named_by_sdk()
{
    std::set<std::string> names;
    const std::vector<std::string> files = counters_split_list( ROCKETNET_COUNTER_SDK_SOURCES );
    check( !files.empty() );
    for ( size_t i = 0; i < files.size(); i++ )
    {
        std::string text;
        check( counters_read_file( files[i], text ) );
        counters_collect_names( text, names );
    }
    return names;
}

// Counters with no code path in sdk/ yet. A counter whose first user has not
// landed cannot be tested by a packet, so it is listed here with the reason
// it has no path; the audit fails the moment a listed counter gains a path,
// so this list only shrinks.

struct counters_exception_t
{
    const char * name;
    const char * why;
};

static const counters_exception_t counters_no_code_path[] = {
    { NULL, NULL }
};

// Keep this list empty while every reachable counter has a test witness.
// A future unreachable-by-test counter can only enter with its code-path
// reason; the audit rejects an exception once a test names that counter.

static const counters_exception_t counters_reachable_not_done[] = { { NULL, NULL } };

static bool counters_in_list( const counters_exception_t * list, const std::string & name )
{
    for ( int i = 0; list[i].name; i++ )
        if ( name == list[i].name )
            return true;
    return false;
}

static void test_counters_every_counter_is_named_by_a_test()
{
    const std::set<std::string> named = counters_named_by_tests();
    const std::set<std::string> counted = counters_named_by_sdk();

    int missed = 0;
    int not_done = 0;
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const std::string name = rocketnet_counter_name( i );
        if ( named.count( name ) || counters_in_list( counters_no_code_path, name ) )
            continue;
        if ( counters_in_list( counters_reachable_not_done, name ) )
        {
            printf( "NOT-DONE: reachable counter named by no test: ROCKETNET_COUNTER_%s\n", name.c_str() );
            not_done++;
            continue;
        }
        printf( "counter named by no test: ROCKETNET_COUNTER_%s\n", name.c_str() );
        missed++;
    }
    if ( missed )
        printf( "%d of %d counters are named by no test\n", missed, ROCKETNET_NUM_COUNTERS );
    if ( not_done )
        printf( "NOT-DONE: %d reachable counters are named by no test and are reported, not exempted\n", not_done );
    check( missed == 0 );

    // a listed counter that gained a code path is a counter a test can reach
    for ( int i = 0; counters_no_code_path[i].name; i++ )
    {
        if ( counted.count( counters_no_code_path[i].name ) )
            printf( "counter listed as having no code path now has one: ROCKETNET_COUNTER_%s\n", counters_no_code_path[i].name );
        check( !counted.count( counters_no_code_path[i].name ) );
        check( rocketnet_counter_name( counters_find( counters_no_code_path[i].name ) ) != NULL );
    }

    // a not-done counter has its code path, and no test names it yet; the
    // reason is printed beside the NOT-DONE line
    for ( int i = 0; counters_reachable_not_done[i].name; i++ )
    {
        const char * name = counters_reachable_not_done[i].name;
        if ( !counted.count( name ) )
            printf( "counter listed as not done has no code path: ROCKETNET_COUNTER_%s\n", name );
        check( counted.count( name ) );
        if ( named.count( name ) )
            printf( "counter listed as not done is named by a test now: ROCKETNET_COUNTER_%s\n", name );
        check( !named.count( name ) );
        printf( "NOT-DONE: ROCKETNET_COUNTER_%s: %s\n", name, counters_reachable_not_done[i].why );
    }
}

REGISTER_UNIT_TEST( test_counters_every_counter_is_named_by_a_test );

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
    { "sdk/rocketnet_server_protocol.cpp", "rocketnet_server_protocol_process_connection_request", "CONNECTION_REQUEST_RECEIVED" },
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
                    return false;
                body = text.substr( open, close - open );
                return true;
            }
        }
        at += needle.size();
    }
    return false;
}

// every counter a text increments: ROCKETNET_COUNTER_<NAME> ] ++

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
            end++;
        size_t after = end;
        while ( after < text.size() && text[after] == ' ' )
            after++;
        if ( after < text.size() && text[after] == ']' )
        {
            after++;
            while ( after < text.size() && text[after] == ' ' )
                after++;
            if ( text.compare( after, 2, "++" ) == 0 )
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
            printf( "ladder function not found: %s in %s\n", ladder.function, ladder.file );
        check( counters_function_body( text, ladder.function, body ) );

        const int received = counters_find( ladder.received );
        check( received >= 0 );

        const std::vector<std::string> counted = counters_incremented( body );

        // a function that counts nothing is a function this audit lost
        check( !counted.empty() );

        for ( size_t j = 0; j < counted.size(); j++ )
        {
            const int counter = counters_find( counted[j].c_str() );
            check( counter >= 0 );
            const int family = counters_rows[counter].family;
            if ( counter == received || family == ROCKETNET_COUNTER_NONE || family == received )
                continue;
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
