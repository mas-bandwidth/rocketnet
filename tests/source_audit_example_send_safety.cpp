/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Example send safety: every authoritative example and copied application
// snippet follows the canonical reservation idiom. For each start_message:
// a matching can_send_message preflight appears in the bounded region
// before it, the returned pointer is assigned, the next statement is
// `if ( data )`, payload writes and the matching finish_message occur only
// inside that guard, finish outcomes are not silently discarded, and
// lifecycle/token outcomes are not silently discarded. The manifest below
// is explicit; CMake passes the globbed example/snippet file list and any
// globbed file not in the manifest fails, so a new example cannot bypass
// review. Reads files only through ROCKETNET_SOURCE_DIR; no sockets,
// threads, clocks or network.

#include "test_unit_registry.h"
#include "portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

void rocketnet_example_send_safety_audit_run();

static bool read_source_file( const std::string & relative_path, std::string & contents )
{
    std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( !file )
        return false;
    char buffer[4096];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        contents.append( buffer, n );
    fclose( file );
    return true;
}

static std::vector<std::string> split_lines( const std::string & text )
{
    std::vector<std::string> lines;
    size_t start = 0;
    for ( size_t i = 0; i <= text.size(); i++ )
    {
        if ( i == text.size() || text[i] == '\n' )
        {
            std::string line = text.substr( start, i - start );
            if ( !line.empty() && line[line.size() - 1] == '\r' )
                line.erase( line.size() - 1 );
            lines.push_back( line );
            start = i + 1;
        }
    }
    return lines;
}

static std::string trim( const std::string & s )
{
    size_t a = 0, b = s.size();
    while ( a < b && ( s[a] == ' ' || s[a] == '\t' ) )
        a++;
    while ( b > a && ( s[b - 1] == ' ' || s[b - 1] == '\t' ) )
        b--;
    return s.substr( a, b - a );
}

static bool contains( const std::string & s, const char * needle )
{
    return s.find( needle ) != std::string::npos;
}

// The authoritative manifest: every example program, helper and copied
// snippet the audit covers. kind: "c" = full call-region rules,
// "md" = rules on fenced C snippets, "inventory" = existence only
// (backend programs in other languages that mint tokens, not sends).
struct manifest_row_t
{
    const char * file;
    const char * kind;
};

static const manifest_row_t manifest[] = {
    { "examples/ack_receipts.c", "c" },
    { "examples/backend_client.c", "c" },
    { "examples/backend_server.c", "c" },
    { "examples/big_messages.c", "c" },
    { "examples/budgets.c", "c" },
    { "examples/channels.c", "c" },
    { "examples/client_server_client.c", "c" },
    { "examples/client_server_server.c", "c" },
    { "examples/config_env.c", "c" },
    { "examples/game_loop.c", "c" },
    { "examples/hello.c", "c" },
    { "examples/reliability.c", "c" },
    { "examples/soak.c", "c" },
    { "examples/stats.c", "c" },
    { "examples/transports.c", "c" },
    { "examples/shutdown.h", "c" },
    { "examples/dev_keys.h", "c" },
    { "examples/backend/check_token.cpp", "c" },
    { "examples/backend/README.md", "md" },
    { "examples/backend/go/main.go", "inventory" },
    { "examples/backend/csharp/Backend/Program.cs", "inventory" },
    { "AGENTS.md", "md" },
    { "README.md", "md" },
    { "docs/GETTING-STARTED.md", "md" },
    { "docs/CHANNELS.md", "md" },
    { "docs/FLOWS.md", "md" },
    { "docs/ERRORS-AND-LOGGING.md", "md" },
    { "docs/examples/ack_receipts.md", "md" },
    { "docs/examples/big_messages.md", "md" },
    { "docs/examples/budgets.md", "md" },
    { "docs/examples/channels.md", "md" },
    { "docs/examples/client_server.md", "md" },
    { "docs/examples/config_env.md", "md" },
    { "docs/examples/game_loop.md", "md" },
    { "docs/examples/hello.md", "md" },
    { "docs/examples/reliability.md", "md" },
    { "docs/examples/soak.md", "md" },
    { "docs/examples/stats.md", "md" },
    { "docs/examples/transports.md", "md" },
};

static const size_t manifest_count = sizeof( manifest ) / sizeof( manifest[0] );

struct audit_t
{
    std::vector<std::string> failures;
    void fail( const std::string & file, size_t line, const std::string & what )
    {
        char buf[32];
        snprintf( buf, sizeof( buf ), "%zu", line + 1 );
        failures.push_back( file + ":" + buf + ": " + what );
    }
};

// Find the closing brace line of a block whose opening brace is on or
// after line `from` (searching at most 2 lines), counting braces.
static int guard_block_end( const std::vector<std::string> & lines, size_t from )
{
    int depth = 0;
    bool opened = false;
    for ( size_t i = from; i < lines.size() && i < from + 80; i++ )
    {
        for ( size_t k = 0; k < lines[i].size(); k++ )
        {
            if ( lines[i][k] == '{' )
            {
                depth++;
                opened = true;
            }
            else if ( lines[i][k] == '}' )
            {
                depth--;
            }
        }
        if ( opened && depth <= 0 )
            return (int) i;
        if ( !opened && i > from + 1 )
            break;
    }
    return -1;
}

static void audit_blob( audit_t & audit, const std::string & file, const std::vector<std::string> & lines, size_t line_offset )
{
    struct guard_t
    {
        std::string side;
        std::string var;
        size_t begin;
        size_t end;
    };
    std::vector<guard_t> guards;

    for ( size_t i = 0; i < lines.size(); i++ )
    {
        const std::string & line = lines[i];
        const bool is_client = contains( line, "rocketnet_client_start_message(" );
        const bool is_server = contains( line, "rocketnet_server_start_message(" );
        if ( !is_client && !is_server )
            continue;
        const std::string side = is_client ? "client" : "server";

        // assignment of the returned pointer
        const size_t eq = line.find( '=' );
        if ( eq == std::string::npos || eq > line.find( "_start_message(" ) )
        {
            audit.fail( file, line_offset + i, "start_message result is not assigned to a pointer" );
            continue;
        }
        std::string lhs = trim( line.substr( 0, eq ) );
        const size_t star = lhs.rfind( '*' );
        if ( star != std::string::npos )
            lhs = trim( lhs.substr( star + 1 ) );
        const size_t space = lhs.rfind( ' ' );
        const std::string var = space == std::string::npos ? lhs : lhs.substr( space + 1 );
        if ( var.empty() )
        {
            audit.fail( file, line_offset + i, "start_message pointer variable not found" );
            continue;
        }

        // matching can-send preflight in the bounded region before the start
        const std::string preflight = "rocketnet_" + side + "_can_send_message(";
        bool has_preflight = false;
        for ( size_t j = i > 16 ? i - 16 : 0; j < i; j++ )
            if ( contains( lines[j], preflight.c_str() ) )
                has_preflight = true;
        // a negated preflight guard (`if ( !can_send ) goto/return`) just above also counts
        if ( !has_preflight )
            audit.fail( file, line_offset + i, "policy-capable start_message without matching " + preflight + " preflight" );

        // next non-empty line must be `if ( var )` (or the same line, for a one-line snippet)
        const std::string want_if = "if ( " + var + " )";
        const std::string want_if2 = "if (" + var + ")";
        if ( contains( line, want_if.c_str() ) || contains( line, want_if2.c_str() ) )
        {
            guard_t g;
            g.side = side;
            g.var = var;
            g.begin = i;
            g.end = i;
            guards.push_back( g );
            continue;
        }
        size_t next = i + 1;
        while ( next < lines.size() && trim( lines[next] ).empty() )
            next++;
        if ( next >= lines.size() || ( !contains( lines[next], want_if.c_str() ) && !contains( lines[next], want_if2.c_str() ) ) )
        {
            audit.fail( file, line_offset + i, "start_message is not followed by if ( " + var + " )" );
            continue;
        }
        const int end = guard_block_end( lines, next );
        if ( end < 0 )
        {
            audit.fail( file, line_offset + i, "guard block for " + var + " has no closing brace" );
            continue;
        }
        guard_t g;
        g.side = side;
        g.var = var;
        g.begin = next;
        g.end = (size_t) end;
        guards.push_back( g );

        // the matching finish must occur inside the guard
        const std::string finish = "rocketnet_" + side + "_finish_message(";
        bool has_finish = false;
        for ( size_t j = next; j <= (size_t) end; j++ )
            if ( contains( lines[j], finish.c_str() ) )
                has_finish = true;
        if ( !has_finish )
            audit.fail( file, line_offset + i, "no matching finish_message inside the if ( " + var + " ) guard" );

        // payload writes through var between start and guard must be inside
        for ( size_t j = i + 1; j < next; j++ )
        {
            if ( contains( lines[j], ( "memcpy( " + var ).c_str() ) || contains( lines[j], ( "memset( " + var ).c_str() ) ||
                 contains( lines[j], ( var + "[" ).c_str() ) )
                audit.fail( file, line_offset + j, "write through " + var + " before its guard" );
        }
    }

    // every finish must be inside a guard and must not be discarded
    for ( size_t i = 0; i < lines.size(); i++ )
    {
        const bool fc = contains( lines[i], "rocketnet_client_finish_message(" );
        const bool fs = contains( lines[i], "rocketnet_server_finish_message(" );
        if ( !fc && !fs )
            continue;
        const std::string side = fc ? "client" : "server";
        bool inside = false;
        for ( size_t g = 0; g < guards.size(); g++ )
            if ( guards[g].side == side && i >= guards[g].begin && i <= guards[g].end )
                inside = true;
        if ( !inside )
            audit.fail( file, line_offset + i, "finish_message outside a successful start guard" );
        const std::string t = trim( lines[i] );
        const bool handled = t.find( '=' ) != std::string::npos || t.compare( 0, 3, "if " ) == 0 ||
                             t.compare( 0, 3, "if(" ) == 0 || contains( t, "CHECK(" ) || contains( t, "check(" );
        if ( !handled )
            audit.fail( file, line_offset + i, "finish_message outcome is silently discarded" );
    }

    // lifecycle and token outcomes must not be bare discarded statements
    static const char * const lifecycle[] = {
        "rocketnet_init(", "rocketnet_server_default_config(", "rocketnet_client_default_config(", "rocketnet_term(",
        "rocketnet_client_connect(", "rocketnet_server_start(", "rocketnet_create_dev_connect_token(",
        "rocketnet_client_create(", "rocketnet_server_create("
    };
    for ( size_t i = 0; i < lines.size(); i++ )
    {
        const std::string t = trim( lines[i] );
        // a continuation of a multi-line condition is checked by its opening line
        std::string prev;
        for ( size_t j = i; j-- > 0; )
            if ( !trim( lines[j] ).empty() )
            {
                prev = trim( lines[j] );
                break;
            }
        const bool continuation = prev.size() >= 2 && ( prev.compare( prev.size() - 2, 2, "||" ) == 0 || prev.compare( prev.size() - 2, 2, "&&" ) == 0 );
        if ( continuation )
            continue;
        for ( size_t k = 0; k < sizeof( lifecycle ) / sizeof( lifecycle[0] ); k++ )
        {
            if ( t.compare( 0, strlen( lifecycle[k] ), lifecycle[k] ) == 0 )
                audit.fail( file, line_offset + i, std::string( "silently discarded lifecycle outcome: " ) + lifecycle[k] );
        }
    }
}

static void audit_md( audit_t & audit, const std::string & file, const std::string & text )
{
    // audit each fenced c/cpp block as a snippet blob
    std::vector<std::string> lines = split_lines( text );
    bool in_fence = false;
    std::vector<std::string> blob;
    size_t blob_start = 0;
    for ( size_t i = 0; i <= lines.size(); i++ )
    {
        const std::string trimmed = i < lines.size() ? trim( lines[i] ) : std::string();
        // a line holding both an opening and a closing fence is a one-line snippet: no toggle
        const bool fence = i < lines.size() && trimmed.compare( 0, 3, "```" ) == 0 &&
                           trimmed.find( "```", 3 ) == std::string::npos;
        if ( fence && !in_fence )
        {
            in_fence = true;
            blob.clear();
            blob_start = i + 1;
        }
        else if ( fence && in_fence )
        {
            in_fence = false;
            audit_blob( audit, file, blob, blob_start );
            blob.clear();
        }
        else if ( in_fence && i < lines.size() )
            blob.push_back( lines[i] );
    }
}

void rocketnet_example_send_safety_audit_run()
{
    audit_t audit;

    // CMake glob vs manifest: a globbed example/snippet not in the manifest fails
#ifdef ROCKETNET_EXAMPLE_MANIFEST
    {
        const std::string globbed = ROCKETNET_EXAMPLE_MANIFEST;
        size_t pos = 0;
        while ( pos <= globbed.size() )
        {
            const size_t sep = globbed.find( '|', pos );
            const std::string entry = globbed.substr( pos, sep == std::string::npos ? std::string::npos : sep - pos );
            if ( !entry.empty() )
            {
                bool listed = false;
                for ( size_t m = 0; m < manifest_count; m++ )
                    if ( entry == manifest[m].file )
                        listed = true;
                if ( !listed )
                    audit.failures.push_back( entry + ": globbed example/snippet is not in the send-safety manifest" );
            }
            if ( sep == std::string::npos )
                break;
            pos = sep + 1;
        }
    }
#endif

    for ( size_t m = 0; m < manifest_count; m++ )
    {
        std::string text;
        if ( !read_source_file( manifest[m].file, text ) )
        {
            audit.failures.push_back( std::string( "manifest file cannot be read: " ) + manifest[m].file );
            continue;
        }
        if ( strcmp( manifest[m].kind, "inventory" ) == 0 )
            continue;
        if ( strcmp( manifest[m].kind, "md" ) == 0 )
            audit_md( audit, manifest[m].file, text );
        else
            audit_blob( audit, manifest[m].file, split_lines( text ), 0 );
    }

    if ( !audit.failures.empty() )
    {
        for ( size_t i = 0; i < audit.failures.size(); i++ )
            printf( "%s\n", audit.failures[i].c_str() );
        printf( "test_example_send_safety_every_example_and_snippet: %zu failure(s)\n", audit.failures.size() );
        fflush( stdout );
        exit( 1 );
    }
}

static void test_example_send_safety_every_example_and_snippet()
{
    rocketnet_example_send_safety_audit_run();
}

REGISTER_UNIT_TEST( test_example_send_safety_every_example_and_snippet );
