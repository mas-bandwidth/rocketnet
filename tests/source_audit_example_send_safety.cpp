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

#include <ctype.h>
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
    {
        return false;
    }
    char buffer[4096];
    size_t n;
    contents.clear();
    while ( ( n = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
    {
        contents.append( buffer, n );
    }
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
            {
                line.erase( line.size() - 1 );
            }
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
    {
        a++;
    }
    while ( b > a && ( s[b - 1] == ' ' || s[b - 1] == '\t' ) )
    {
        b--;
    }
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
    { "examples/fixed_memory.c", "c" },
    { "examples/game_loop.c", "c" },
    { "examples/hello.c", "c" },
    { "examples/reliability.c", "c" },
    { "examples/soak.c", "c" },
    { "examples/stats.c", "c" },
    { "examples/transports.c", "c" },
    { "examples/shutdown.h", "c" },
    { "examples/thread.h", "c" },
    { "examples/dev_keys.h", "c" },
    { "examples/backend/check_token.cpp", "c" },
    { "examples/backend/README.md", "md" },
    { "examples/backend/go/main.go", "inventory" },
    { "examples/backend/csharp/Backend/Program.cs", "inventory" },
    { "docs/AGENTS.md", "md" },
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
    { "docs/examples/fixed_memory.md", "md" },
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
        {
            return (int) i;
        }
        if ( !opened && i > from + 1 )
        {
            break;
        }
    }
    return -1;
}

// whether name occurs in line as a whole identifier
static bool uses_name( const std::string & line, const std::string & name )
{
    for ( size_t at = line.find( name ); at != std::string::npos; at = line.find( name, at + 1 ) )
    {
        const bool before = at > 0 && ( isalnum( (unsigned char) line[at - 1] ) || line[at - 1] == '_' );
        const size_t after_at = at + name.size();
        const bool after = after_at < line.size() && ( isalnum( (unsigned char) line[after_at] ) || line[after_at] == '_' );
        if ( !before && !after )
        {
            return true;
        }
    }
    return false;
}

// whether line writes through name: a memcpy or memset into it, or an index of it
static bool writes_through( const std::string & line, const std::string & name )
{
    for ( size_t at = line.find( name ); at != std::string::npos; at = line.find( name, at + 1 ) )
    {
        const bool before = at > 0 && ( isalnum( (unsigned char) line[at - 1] ) || line[at - 1] == '_' );
        const size_t after_at = at + name.size();
        if ( before || after_at >= line.size() || isalnum( (unsigned char) line[after_at] ) || line[after_at] == '_' )
        {
            continue;
        }
        if ( line[after_at] == '[' )
        {
            return true;
        }
        const std::string head = trim( line.substr( 0, at ) );
        const size_t call = head.size() >= 7 ? head.size() - 7 : 0;
        if ( head.size() >= 7 && ( head.compare( call, 7, "memcpy(" ) == 0 || head.compare( call, 7, "memset(" ) == 0 ) )
        {
            return true;
        }
    }
    return false;
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
        {
            continue;
        }
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
        {
            lhs = trim( lhs.substr( star + 1 ) );
        }
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
        {
            if ( contains( lines[j], preflight.c_str() ) )
            {
                has_preflight = true;
            }
        }
        // a start made from a game thread has no preflight:
        // rocketnet_server_can_send_message is a main thread call. The comment
        // marks the site; what the audit holds it to is checked below: the
        // pointer is tested against NULL before any write through it, and
        // the path that gets NULL neither writes nor finishes
        bool game_thread_start = false;
        if ( !has_preflight && side == "server" )
        {
            for ( size_t j = i > 16 ? i - 16 : 0; j < i; j++ )
            {
                if ( contains( lines[j], "game thread start: no can_send_message preflight" ) )
                {
                    has_preflight = true;
                    game_thread_start = true;
                }
            }
        }
        // a negated preflight guard (`if ( !can_send ) goto/return`) just above also counts
        if ( !has_preflight )
        {
            audit.fail( file, line_offset + i, "policy-capable start_message without matching " + preflight + " preflight" );
        }

        // next non-empty line must be `if ( var )` (or the same line, for a one-line snippet)
        const std::string want_if = "if ( " + var + " )";
        const std::string want_if2 = "if (" + var + ")";
        size_t next = i;
        if ( !contains( line, want_if.c_str() ) && !contains( line, want_if2.c_str() ) )
        {
            next = i + 1;
            while ( next < lines.size() && trim( lines[next] ).empty() )
            {
                next++;
            }
            if ( next >= lines.size() || ( !contains( lines[next], want_if.c_str() ) && !contains( lines[next], want_if2.c_str() ) ) )
            {
                audit.fail( file, line_offset + i, "start_message is not followed by if ( " + var + " )" );
                continue;
            }
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

        // a game thread start: nothing after the guard's block, to the end of
        // its function, may use the pointer (the path that got NULL writes
        // and finishes nothing), and the guard has no else that does
        if ( game_thread_start )
        {
            for ( size_t j = (size_t) end + 1; j < lines.size() && lines[j].compare( 0, 1, "}" ) != 0; j++ )
            {
                if ( uses_name( lines[j], var ) || contains( lines[j], "rocketnet_server_finish_" ) )
                {
                    audit.fail( file, line_offset + j, "a game thread start's NULL path uses " + var + " or finishes: it must do nothing" );
                }
            }
            for ( size_t j = next + 1; j < (size_t) end; j++ )
            {
                if ( trim( lines[j] ).compare( 0, 6, "} else" ) == 0 )
                {
                    audit.fail( file, line_offset + j, "a game thread start's guard has an else: the NULL path must do nothing" );
                }
            }
        }

        // after the guard's block, to the end of its function or the next
        // start, nothing writes through the pointer: the path that got NULL
        // writes nothing (a one-line snippet: nor does the rest of its line)
        if ( next == i )
        {
            const size_t if_at = contains( line, want_if.c_str() ) ? line.find( want_if ) : line.find( want_if2 );
            int depth = 0;
            for ( size_t k = if_at; k < line.size(); k++ )
            {
                if ( line[k] == '{' )
                {
                    depth++;
                }
                else if ( line[k] == '}' )
                {
                    depth--;
                    if ( depth == 0 )
                    {
                        if ( writes_through( line.substr( k + 1 ), var ) )
                        {
                            audit.fail( file, line_offset + i, "write through " + var + " after its guard" );
                        }
                        break;
                    }
                }
            }
        }
        for ( size_t j = (size_t) end + 1; j < lines.size() && lines[j].compare( 0, 1, "}" ) != 0 && !contains( lines[j], "_start_message(" ); j++ )
        {
            if ( writes_through( lines[j], var ) )
            {
                audit.fail( file, line_offset + j, "write through " + var + " after its guard" );
            }
        }

        // the matching finish must occur inside the guard
        const std::string finish = "rocketnet_" + side + "_finish_message(";
        bool has_finish = false;
        for ( size_t j = next; j <= (size_t) end; j++ )
        {
            if ( contains( lines[j], finish.c_str() ) )
            {
                has_finish = true;
            }
        }
        if ( !has_finish )
        {
            audit.fail( file, line_offset + i, "no matching finish_message inside the if ( " + var + " ) guard" );
        }

        // payload writes through var between start and guard must be inside
        for ( size_t j = i + 1; j < next; j++ )
        {
            if ( contains( lines[j], ( "memcpy( " + var ).c_str() ) || contains( lines[j], ( "memset( " + var ).c_str() ) ||
                 contains( lines[j], ( var + "[" ).c_str() ) )
            {
                audit.fail( file, line_offset + j, "write through " + var + " before its guard" );
            }
        }
    }

    // every finish must be inside a guard and must not be discarded
    for ( size_t i = 0; i < lines.size(); i++ )
    {
        const bool fc = contains( lines[i], "rocketnet_client_finish_message(" );
        const bool fs = contains( lines[i], "rocketnet_server_finish_message(" );
        if ( !fc && !fs )
        {
            continue;
        }
        const std::string side = fc ? "client" : "server";
        bool inside = false;
        for ( size_t g = 0; g < guards.size(); g++ )
        {
            if ( guards[g].side == side && i >= guards[g].begin && i <= guards[g].end )
            {
                inside = true;
            }
        }
        if ( !inside )
        {
            audit.fail( file, line_offset + i, "finish_message outside a successful start guard" );
        }
        const std::string t = trim( lines[i] );
        const bool handled = t.find( '=' ) != std::string::npos || t.compare( 0, 3, "if " ) == 0 ||
                             t.compare( 0, 3, "if(" ) == 0 || contains( t, "CHECK(" ) || contains( t, "check(" );
        if ( !handled )
        {
            audit.fail( file, line_offset + i, "finish_message outcome is silently discarded" );
        }
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
        {
            if ( !trim( lines[j] ).empty() )
            {
                prev = trim( lines[j] );
                break;
            }
        }
        const bool continuation = prev.size() >= 2 && ( prev.compare( prev.size() - 2, 2, "||" ) == 0 || prev.compare( prev.size() - 2, 2, "&&" ) == 0 );
        if ( continuation )
        {
            continue;
        }
        for ( size_t k = 0; k < sizeof( lifecycle ) / sizeof( lifecycle[0] ); k++ )
        {
            if ( t.compare( 0, strlen( lifecycle[k] ), lifecycle[k] ) == 0 )
            {
                audit.fail( file, line_offset + i, std::string( "silently discarded lifecycle outcome: " ) + lifecycle[k] );
            }
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
        {
            blob.push_back( lines[i] );
        }
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
                {
                    if ( entry == manifest[m].file )
                    {
                        listed = true;
                    }
                }
                if ( !listed )
                {
                    audit.failures.push_back( entry + ": globbed example/snippet is not in the send-safety manifest" );
                }
            }
            if ( sep == std::string::npos )
            {
                break;
            }
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
        {
            continue;
        }
        if ( strcmp( manifest[m].kind, "md" ) == 0 )
        {
            audit_md( audit, manifest[m].file, text );
        }
        else
        {
            audit_blob( audit, manifest[m].file, split_lines( text ), 0 );
        }
    }

    if ( !audit.failures.empty() )
    {
        for ( size_t i = 0; i < audit.failures.size(); i++ )
        {
            printf( "%s\n", audit.failures[i].c_str() );
        }
        printf( "test_example_send_safety_every_example_and_snippet: %zu failure(s)\n", audit.failures.size() );
        fflush( stdout );
        exit( 1 );
    }
}

static void test_example_send_safety_every_example_and_snippet()
{
    rocketnet_example_send_safety_audit_run();
}

// the audit's own cases, in memory: a game thread start passes only when its
// result is tested before any write and the NULL path does nothing; then an
// ordinary start's cases (send_safety_writes_after_the_guard_cases, below)
static size_t send_safety_failures( const char * text )
{
    audit_t audit;
    audit_blob( audit, "case", split_lines( text ), 0 );
    return audit.failures.size();
}

static void send_safety_writes_after_the_guard_cases();

static void test_example_send_safety_game_thread_start_is_held_to_its_null_test()
{
    static const char * const good =
        "// game thread start: no can_send_message preflight\n"
        "uint8_t * p = rocketnet_server_start_message( server, 1, 0, mode, 12 );\n"
        "if ( p )\n"
        "{\n"
        "    const uint32_t serial = rocketnet_server_finish_message( server, write( p ) );\n"
        "    (void) serial;\n"
        "}\n";
    static const char * const unchecked =
        "// game thread start: no can_send_message preflight\n"
        "uint8_t * p = rocketnet_server_start_message( server, 1, 0, mode, 12 );\n"
        "memcpy( p, data, 12 );\n"
        "if ( p )\n"
        "{\n"
        "    const uint32_t serial = rocketnet_server_finish_message( server, 12 );\n"
        "    (void) serial;\n"
        "}\n";
    static const char * const no_guard =
        "// game thread start: no can_send_message preflight\n"
        "uint8_t * p = rocketnet_server_start_message( server, 1, 0, mode, 12 );\n"
        "write( p );\n";
    static const char * const null_path_writes =
        "void f()\n"
        "{\n"
        "    // game thread start: no can_send_message preflight\n"
        "    uint8_t * p = rocketnet_server_start_message( server, 1, 0, mode, 12 );\n"
        "    if ( p )\n"
        "    {\n"
        "        const uint32_t serial = rocketnet_server_finish_message( server, write( p ) );\n"
        "        (void) serial;\n"
        "    }\n"
        "    p[0] = 1;\n"
        "}\n";
    static const char * const no_marker =
        "uint8_t * p = rocketnet_server_start_message( server, 1, 0, mode, 12 );\n"
        "if ( p )\n"
        "{\n"
        "    const uint32_t serial = rocketnet_server_finish_message( server, write( p ) );\n"
        "    (void) serial;\n"
        "}\n";

    rocketnet_unit_check( send_safety_failures( good ) == 0 );
    rocketnet_unit_check( send_safety_failures( unchecked ) > 0 );
    rocketnet_unit_check( send_safety_failures( no_guard ) > 0 );
    rocketnet_unit_check( send_safety_failures( null_path_writes ) > 0 );
    rocketnet_unit_check( send_safety_failures( no_marker ) > 0 );
    send_safety_writes_after_the_guard_cases();
}

REGISTER_UNIT_TEST( test_example_send_safety_game_thread_start_is_held_to_its_null_test );

// an ordinary start: a write through the pointer after its guard's block
// is the NULL path writing, and a one-line snippet is held to the same rules
// (a matching finish inside the guard, nothing written after it)
static void send_safety_writes_after_the_guard_cases()
{
    static const char * const good =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "{\n"
        "    uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 );\n"
        "    if ( p )\n"
        "    {\n"
        "        memcpy( p, data, 12 );\n"
        "        sent = rocketnet_client_finish_message( client, 12 );\n"
        "    }\n"
        "}\n";
    static const char * const index_after =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "{\n"
        "    uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 );\n"
        "    if ( p )\n"
        "    {\n"
        "        sent = rocketnet_client_finish_message( client, 12 );\n"
        "    }\n"
        "    p[0] = 1;\n"
        "}\n";
    static const char * const memcpy_after =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "{\n"
        "    uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 );\n"
        "    if ( p )\n"
        "    {\n"
        "        sent = rocketnet_client_finish_message( client, 12 );\n"
        "    }\n"
        "    memcpy( p, data, 12 );\n"
        "}\n";
    static const char * const one_line_good =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "    { uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 ); if ( p ) { memcpy( p, data, 12 ); sent = rocketnet_client_finish_message( client, 12 ); } }\n";
    static const char * const one_line_no_finish =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "    { uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 ); if ( p ) { memcpy( p, data, 12 ); } }\n";
    static const char * const one_line_write_after =
        "if ( rocketnet_client_can_send_message( client, 0, mode, 12 ) )\n"
        "    { uint8_t * p = rocketnet_client_start_message( client, 0, mode, 12 ); if ( p ) { sent = rocketnet_client_finish_message( client, 12 ); } p[0] = 1; }\n";

    rocketnet_unit_check( send_safety_failures( good ) == 0 );
    rocketnet_unit_check( send_safety_failures( index_after ) > 0 );
    rocketnet_unit_check( send_safety_failures( memcpy_after ) > 0 );
    rocketnet_unit_check( send_safety_failures( one_line_good ) == 0 );
    rocketnet_unit_check( send_safety_failures( one_line_no_finish ) > 0 );
    rocketnet_unit_check( send_safety_failures( one_line_write_after ) > 0 );
}

REGISTER_UNIT_TEST( test_example_send_safety_every_example_and_snippet );
