/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The tools never cast the public client to an internal struct. struct
// rocketnet_client_t starts with a context pointer, not a message client, so
// a cast of a public client pointer to struct rocketnet_message_client_t (or
// any other internal struct) reads the wrong memory. The tools resolve the
// connection through rocketnet_client_message_client instead. This audit
// reads tests/client_server/client.c, tests/client_server/server.c, tests/load/load.c and tests/soak/soak.c, collects every name declared
// as a struct rocketnet_client_t pointer, and refuses a cast of any of those
// names to another struct pointer type. It first runs its detector on two
// fixtures, one with the cast and one without, so a detector that finds
// nothing cannot pass. Reads files only through ROCKETNET_SOURCE_DIR; no
// sockets, threads, clocks or network.

#include "test_unit_registry.h"
#include "portable_crt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

static bool client_cast_read_file( const char * relative, std::string & text )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( file == NULL )
        return false;
    char buffer[4096];
    size_t got;
    text.clear();
    while ( ( got = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        text.append( buffer, got );
    fclose( file );
    return true;
}

// Comments and string or character literals become spaces, newlines kept, so
// offsets map to the same lines and nothing quoted can match.
static std::string client_cast_code_only( const std::string & source )
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
    return code;
}

static bool client_cast_is_identifier_char( char c )
{
    return isalnum( (unsigned char) c ) || c == '_';
}

static void client_cast_skip_space( const std::string & code, size_t & i )
{
    while ( i < code.size() && isspace( (unsigned char) code[i] ) )
        i++;
}

static std::string client_cast_identifier_at( const std::string & code, size_t & i )
{
    const size_t start = i;
    while ( i < code.size() && client_cast_is_identifier_char( code[i] ) )
        i++;
    return code.substr( start, i - start );
}

// The word starting at i, if code[i] begins a whole word.
static bool client_cast_word_at( const std::string & code, size_t i, const char * word )
{
    const size_t n = strlen( word );
    if ( code.compare( i, n, word ) != 0 )
        return false;
    if ( i > 0 && client_cast_is_identifier_char( code[i - 1] ) )
        return false;
    return i + n >= code.size() || !client_cast_is_identifier_char( code[i + n] );
}

static int client_cast_line_of( const std::string & code, size_t offset )
{
    int line = 1;
    for ( size_t i = 0; i < offset && i < code.size(); i++ )
        if ( code[i] == '\n' )
            line++;
    return line;
}

// Every name declared as a pointer to the public client: a parameter, a
// local, a global or a member, "struct rocketnet_client_t * name" with any
// const qualifiers.
static std::set<std::string> client_cast_client_names( const std::string & code )
{
    std::set<std::string> names;
    for ( size_t i = 0; i < code.size(); i++ )
    {
        if ( !client_cast_word_at( code, i, "struct" ) )
            continue;
        size_t j = i + 6;
        client_cast_skip_space( code, j );
        if ( !client_cast_word_at( code, j, "rocketnet_client_t" ) )
            continue;
        j += strlen( "rocketnet_client_t" );
        client_cast_skip_space( code, j );
        if ( j >= code.size() || code[j] != '*' )
            continue;
        j++;
        client_cast_skip_space( code, j );
        while ( client_cast_word_at( code, j, "const" ) )
        {
            j += 5;
            client_cast_skip_space( code, j );
        }
        const std::string name = client_cast_identifier_at( code, j );
        if ( !name.empty() )
            names.insert( name );
    }
    return names;
}

// Every cast "( [const] struct <type> * [const] ) [&(] <client name>" whose
// type is not rocketnet_client_t, as "file:line: cast of <name> to <type>".
static void client_cast_find_casts( const char * file, const std::string & code, std::vector<std::string> & failures )
{
    const std::set<std::string> names = client_cast_client_names( code );
    for ( size_t i = 0; i < code.size(); i++ )
    {
        if ( code[i] != '(' )
            continue;
        size_t j = i + 1;
        client_cast_skip_space( code, j );
        if ( client_cast_word_at( code, j, "const" ) )
        {
            j += 5;
            client_cast_skip_space( code, j );
        }
        if ( !client_cast_word_at( code, j, "struct" ) )
            continue;
        j += 6;
        client_cast_skip_space( code, j );
        const std::string type = client_cast_identifier_at( code, j );
        if ( type.empty() || type == "rocketnet_client_t" )
            continue;
        client_cast_skip_space( code, j );
        if ( j >= code.size() || code[j] != '*' )
            continue;
        j++;
        client_cast_skip_space( code, j );
        if ( client_cast_word_at( code, j, "const" ) )
        {
            j += 5;
            client_cast_skip_space( code, j );
        }
        if ( j >= code.size() || code[j] != ')' )
            continue;
        j++;
        client_cast_skip_space( code, j );
        while ( j < code.size() && code[j] == '(' )
        {
            j++;
            client_cast_skip_space( code, j );
        }
        const std::string operand = client_cast_identifier_at( code, j );
        if ( operand.empty() || names.find( operand ) == names.end() )
            continue;
        char line[512];
        snprintf( line, sizeof( line ), "%s:%d: cast of public client '%s' to struct %s",
                  file, client_cast_line_of( code, i ), operand.c_str(), type.c_str() );
        failures.push_back( line );
    }
}

static void test_tools_never_cast_public_client_to_internal()
{
    std::vector<std::string> failures;

    // The detector's witnesses: the cast the client tool carried must be found,
    // through a const qualifier and parentheses too; the resolved form, a cast
    // in a comment and a cast of another name must not be.
    {
        const char * with_cast =
            "static void sample( struct rocketnet_client_t * client )\n"
            "{\n"
            "    struct rocketnet_message_client_t * message_client = (struct rocketnet_message_client_t *) client;\n"
            "    const struct rocketnet_message_client_t * other = ( const struct rocketnet_message_client_t * ) ( client );\n"
            "}\n";
        const char * without_cast =
            "static void sample( struct rocketnet_client_t * client, void * context )\n"
            "{\n"
            "    struct rocketnet_message_client_t * message_client = rocketnet_client_message_client( client );\n"
            "    // (struct rocketnet_message_client_t *) client\n"
            "    struct rocketnet_server_t * server = (struct rocketnet_server_t *) context;\n"
            "    struct rocketnet_client_t * same = (struct rocketnet_client_t *) client;\n"
            "}\n";
        std::vector<std::string> found;
        client_cast_find_casts( "fixture-with-cast", client_cast_code_only( with_cast ), found );
        if ( found.size() != 2 )
        {
            char line[128];
            snprintf( line, sizeof( line ), "detector witness: expected 2 casts in the cast fixture, found %zu", found.size() );
            failures.push_back( line );
        }
        found.clear();
        client_cast_find_casts( "fixture-without-cast", client_cast_code_only( without_cast ), found );
        for ( size_t i = 0; i < found.size(); i++ )
            failures.push_back( "detector witness: false cast " + found[i] );
    }

    static const char * const files[] = { "tests/client_server/client.c", "tests/client_server/server.c", "tests/load/load.c", "tests/soak/soak.c" };
    for ( size_t f = 0; f < sizeof( files ) / sizeof( files[0] ); f++ )
    {
        std::string text;
        if ( !client_cast_read_file( files[f], text ) )
        {
            failures.push_back( std::string( "cannot read " ) + files[f] );
            continue;
        }
        client_cast_find_casts( files[f], client_cast_code_only( text ), failures );
    }

    if ( !failures.empty() )
    {
        for ( size_t i = 0; i < failures.size(); i++ )
            printf( "%s\n", failures[i].c_str() );
        printf( "test_tools_never_cast_public_client_to_internal: %zu failure(s)\n", failures.size() );
        fflush( stdout );
        exit( 1 );
    }
}

REGISTER_UNIT_TEST( test_tools_never_cast_public_client_to_internal );
