/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The owned storage manifest in docs/SEQUENCE-BUFFERS.md against the source.
// Every row names the family whose links were removed and the symbol that
// replaced them; this case reads each row's file and requires that symbol in
// its code (comments and literals blanked), requires the fourteen SDK
// families and the four suite, functional and soak rows, and fails on every
// row still marked residual. Every external ABI traversal row must name a
// file holding its function, its link field and its release call. The
// token-level proof that no owned link remains is
// tools/check_no_owned_linked_lists.py; this case keeps the manifest honest
// beside it. It reads files only.

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "test_unit_registry.h"
#include "portable_crt.h"

static const char * const sequence_audit_case = "test_sequence_buffers_manifest_matches_source";

static bool sequence_audit_read( const std::string & relative, std::string & text )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( file == NULL )
        return false;
    char buffer[4096];
    size_t got;
    while ( ( got = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
        text.append( buffer, got );
    fclose( file );
    return true;
}

// comments, string and character literals blanked, newlines kept
static std::string sequence_audit_code( const std::string & source )
{
    std::string code = source;
    size_t i = 0;
    while ( i < code.size() )
    {
        const char c = code[i];
        const char next = i + 1 < code.size() ? code[i + 1] : '\0';
        if ( c == '/' && next == '/' )
        {
            while ( i < code.size() && code[i] != '\n' )
                code[i++] = ' ';
        }
        else if ( c == '/' && next == '*' )
        {
            code[i++] = ' ';
            code[i++] = ' ';
            while ( i < code.size() && !( code[i] == '*' && i + 1 < code.size() && code[i + 1] == '/' ) )
            {
                if ( code[i] != '\n' )
                    code[i] = ' ';
                i++;
            }
            if ( i + 1 < code.size() )
            {
                code[i++] = ' ';
                code[i++] = ' ';
            }
        }
        else if ( c == '"' || c == '\'' )
        {
            const char quote = c;
            code[i++] = ' ';
            while ( i < code.size() && code[i] != quote && code[i] != '\n' )
            {
                if ( code[i] == '\\' && i + 1 < code.size() )
                    code[i++] = ' ';
                code[i++] = ' ';
            }
            if ( i < code.size() && code[i] == quote )
                code[i++] = ' ';
        }
        else
        {
            i++;
        }
    }
    return code;
}

static bool sequence_audit_word( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

// the identifier as a whole token in code
static bool sequence_audit_has_token( const std::string & code, const std::string & name )
{
    for ( size_t at = code.find( name ); at != std::string::npos; at = code.find( name, at + 1 ) )
    {
        const bool left = at == 0 || !sequence_audit_word( code[at - 1] );
        const bool right = at + name.size() >= code.size() || !sequence_audit_word( code[at + name.size()] );
        if ( left && right )
            return true;
    }
    return false;
}

static std::string sequence_audit_trim( const std::string & text )
{
    size_t first = 0;
    size_t last = text.size();
    while ( first < last && ( text[first] == ' ' || text[first] == '`' ) )
        first++;
    while ( last > first && ( text[last - 1] == ' ' || text[last - 1] == '`' || text[last - 1] == '\r' ) )
        last--;
    return text.substr( first, last - first );
}

static std::vector<std::string> sequence_audit_cells( const std::string & line )
{
    std::vector<std::string> cells;
    size_t start = line.find( '|' );
    while ( start != std::string::npos )
    {
        const size_t end = line.find( '|', start + 1 );
        if ( end == std::string::npos )
            break;
        cells.push_back( line.substr( start + 1, end - start - 1 ) );
        start = end;
    }
    return cells;
}

// the table rows of one "## " section, header and rule rows skipped
static std::vector<std::vector<std::string>> sequence_audit_table( const std::string & doc, const char * heading )
{
    std::vector<std::vector<std::string>> rows;
    const size_t section = doc.find( std::string( "\n## " ) + heading + "\n" );
    if ( section == std::string::npos )
        return rows;
    size_t end = doc.find( "\n## ", section + 4 );
    if ( end == std::string::npos )
        end = doc.size();
    bool header = true;
    size_t at = section + 1;
    while ( at < end )
    {
        size_t eol = doc.find( '\n', at );
        if ( eol == std::string::npos || eol > end )
            eol = end;
        const std::string line = doc.substr( at, eol - at );
        if ( line.size() > 0 && line[0] == '|' )
        {
            if ( header )
                header = false;
            else if ( line.find( "---" ) == std::string::npos )
                rows.push_back( sequence_audit_cells( line ) );
        }
        at = eol + 1;
    }
    return rows;
}

// the first backticked identifier of a cell, and the first path in it
static std::string sequence_audit_symbol( const std::string & cell )
{
    const size_t open = cell.find( '`' );
    const size_t close = open == std::string::npos ? std::string::npos : cell.find( '`', open + 1 );
    return close == std::string::npos ? std::string() : cell.substr( open + 1, close - open - 1 );
}

static std::string sequence_audit_path( const std::string & cell )
{
    static const char * const roots[] = { "sdk/", "tests/", "functional/", "examples/", "tools/" };
    for ( size_t r = 0; r < sizeof( roots ) / sizeof( roots[0] ); r++ )
    {
        const size_t at = cell.find( roots[r] );
        if ( at == std::string::npos )
            continue;
        size_t end = at;
        while ( end < cell.size() && cell[end] != ':' && cell[end] != ' ' )
            end++;
        return cell.substr( at, end - at );
    }
    return std::string();
}

static void test_sequence_buffers_manifest_matches_source()
{
    std::string doc;
    if ( !sequence_audit_read( "docs/SEQUENCE-BUFFERS.md", doc ) )
    {
        printf( "%s: cannot read docs/SEQUENCE-BUFFERS.md\n", sequence_audit_case );
        fflush( stdout );
        rocketnet_unit_check( false );
        return;
    }

    int problems = 0;
    int sdk_rows = 0;
    bool suite_unit = false;
    bool suite_bound = false;
    bool functional_harness = false;
    bool soak_allocator = false;

    const std::vector<std::vector<std::string>> rows = sequence_audit_table( doc, "Owned storage manifest" );
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        const std::vector<std::string> & row = rows[i];
        if ( row.size() != 9 )
        {
            printf( "%s: manifest row %d has %d cells, not 9\n", sequence_audit_case, (int) i + 1, (int) row.size() );
            problems++;
            continue;
        }
        const std::string scope = sequence_audit_trim( row[0] );
        const std::string family = sequence_audit_trim( row[1] );
        const std::string status = sequence_audit_trim( row[8] );
        if ( scope == "sdk" )
            sdk_rows++;
        else if ( scope == "suite" && family == "unit registry" )
            suite_unit = true;
        else if ( scope == "suite" && family == "suite RSS bound registry" )
            suite_bound = true;
        else if ( scope == "functional" )
            functional_harness = true;
        else if ( scope == "soak" )
            soak_allocator = true;

        for ( int cell = 4; cell <= 7; cell++ )
        {
            if ( sequence_audit_trim( row[(size_t) cell] ).empty() )
            {
                printf( "%s: %s has no capacity, identity, admission or bound\n", sequence_audit_case, family.c_str() );
                problems++;
                break;
            }
        }

        if ( status != "replaced" )
        {
            printf( "%s: %s is %s: an owned linked list remains (%s)\n", sequence_audit_case, family.c_str(), status.c_str(), sequence_audit_trim( row[2] ).c_str() );
            problems++;
            continue;
        }

        const std::string symbol = sequence_audit_symbol( row[3] );
        const std::string path = sequence_audit_path( row[3] );
        std::string source;
        if ( symbol.empty() || path.empty() || !sequence_audit_read( path, source ) )
        {
            printf( "%s: %s names no readable replacement (%s)\n", sequence_audit_case, family.c_str(), sequence_audit_trim( row[3] ).c_str() );
            problems++;
            continue;
        }
        if ( !sequence_audit_has_token( sequence_audit_code( source ), symbol ) )
        {
            printf( "%s: %s replacement %s is not in the code of %s\n", sequence_audit_case, family.c_str(), symbol.c_str(), path.c_str() );
            problems++;
        }
    }

    if ( sdk_rows != 14 )
    {
        printf( "%s: the manifest has %d SDK families, not 14\n", sequence_audit_case, sdk_rows );
        problems++;
    }
    if ( !suite_unit || !suite_bound || !functional_harness || !soak_allocator )
    {
        printf( "%s: the manifest lacks a unit registry, suite bound registry, functional harness or soak allocator row\n", sequence_audit_case );
        problems++;
    }

    const std::vector<std::vector<std::string>> external = sequence_audit_table( doc, "External ABI traversal sites" );
    if ( external.empty() )
    {
        printf( "%s: the manifest has no external ABI traversal sites\n", sequence_audit_case );
        problems++;
    }
    for ( size_t i = 0; i < external.size(); i++ )
    {
        const std::vector<std::string> & row = external[i];
        if ( row.size() != 4 )
        {
            printf( "%s: external row %d has %d cells, not 4\n", sequence_audit_case, (int) i + 1, (int) row.size() );
            problems++;
            continue;
        }
        const std::string path = sequence_audit_trim( row[0] );
        std::string source;
        if ( !sequence_audit_read( path, source ) )
        {
            printf( "%s: external site file %s cannot be read\n", sequence_audit_case, path.c_str() );
            problems++;
            continue;
        }
        const std::string code = sequence_audit_code( source );
        for ( size_t cell = 1; cell < 4; cell++ )
        {
            const std::string name = sequence_audit_trim( row[cell] );
            if ( !sequence_audit_has_token( code, name ) )
            {
                printf( "%s: external site %s does not contain %s\n", sequence_audit_case, path.c_str(), name.c_str() );
                problems++;
            }
        }
    }

    fflush( stdout );
    rocketnet_unit_check( problems == 0 );
}

REGISTER_UNIT_TEST( test_sequence_buffers_manifest_matches_source );
