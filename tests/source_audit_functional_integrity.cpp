/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A generated empty replacement must never stand in for a registered
// functional case. Check every platform's source on every audit host: the
// Windows production substitutions previously returned success on no work.
// This checks that specific source-policy boundary, not general C++ behavior.

#include "test_unit_registry.h"
#include "portable_crt.h"

#include <stdio.h>
#include <set>
#include <string>

static std::string functional_integrity_read( const char * relative )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    rocketnet_unit_check( file != NULL );
    std::string text;
    char buffer[4096];
    size_t bytes;
    while ( ( bytes = fread( buffer, 1, sizeof( buffer ), file ) ) != 0 )
        text.append( buffer, bytes );
    const bool read_ok = ferror( file ) == 0;
    const int close_status = fclose( file );
    rocketnet_unit_check( read_ok && close_status == 0 );
    return text;
}

static std::string functional_integrity_without_comments( const std::string & text )
{
    // libstdc++'s regex engine recurses per character for an unbounded comment
    // match. Repository-sized inputs can exhaust a Debug audit's stack. Skip
    // comments iteratively and retain a separator so identifiers never join.
    std::string code;
    code.reserve( text.size() );
    for ( size_t i = 0; i < text.size(); )
    {
        if ( text.compare( i, 2, "//" ) == 0 )
        {
            const size_t end = text.find( '\n', i + 2 );
            code += ' ';
            i = end == std::string::npos ? text.size() : end;
        }
        else if ( text.compare( i, 2, "/*" ) == 0 )
        {
            const size_t end = text.find( "*/", i + 2 );
            code += ' ';
            i = end == std::string::npos ? text.size() : end + 2;
        }
        else
        {
            code += text[i++];
        }
    }
    return code;
}

static bool functional_integrity_word_char( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

static void functional_integrity_skip_space( const std::string & text, size_t & pos )
{
    while ( pos < text.size() && ( text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                   text[pos] == '\r' || text[pos] == '\f' || text[pos] == '\v' ) )
        pos++;
}

static bool functional_integrity_take( const std::string & text, size_t & pos, char c )
{
    functional_integrity_skip_space( text, pos );
    if ( pos == text.size() || text[pos] != c )
        return false;
    pos++;
    return true;
}

static bool functional_integrity_identifier( const std::string & text, size_t & pos, std::string & name )
{
    if ( pos == text.size() || !functional_integrity_word_char( text[pos] ) || ( text[pos] >= '0' && text[pos] <= '9' ) )
        return false;
    const size_t first = pos++;
    while ( pos < text.size() && functional_integrity_word_char( text[pos] ) )
        pos++;
    name = text.substr( first, pos - first );
    return true;
}

static std::set<std::string> functional_integrity_empty_cases( std::string generated, const std::string & registry )
{
    // CMake's quoted CONTENT carries escaped newlines. Decode those before
    // removing C++ comments, including comments inside otherwise empty bodies.
    for ( size_t pos = 0; ( pos = generated.find( "\\n", pos ) ) != std::string::npos; pos++ )
        generated.replace( pos, 2, "\n" );
    generated = functional_integrity_without_comments( generated );
    const std::string rows = functional_integrity_without_comments( registry );
    // Scan the two small source forms directly. Besides avoiding regex
    // recursion, this keeps GCC's instrumented libstdc++ regex implementation
    // out of a strict-warning sanitizer build of the source audit itself.
    std::set<std::string> empty_functions;
    for ( size_t start = 0; start < generated.size(); start++ )
    {
        if ( start != 0 && functional_integrity_word_char( generated[start - 1] ) )
            continue;
        size_t pos = start;
        std::string result, name;
        if ( !functional_integrity_identifier( generated, pos, result ) || result != "void" )
            continue;
        const size_t before_space = pos;
        functional_integrity_skip_space( generated, pos );
        if ( pos == before_space || !functional_integrity_identifier( generated, pos, name ) ||
             !functional_integrity_take( generated, pos, '(' ) )
            continue;
        functional_integrity_skip_space( generated, pos );
        if ( generated.compare( pos, 4, "void" ) == 0 )
            pos += 4;
        if ( functional_integrity_take( generated, pos, ')' ) && functional_integrity_take( generated, pos, '{' ) &&
             functional_integrity_take( generated, pos, '}' ) )
            empty_functions.insert( name );
    }
    std::set<std::string> failures;
    for ( size_t start = 0; ( start = rows.find( '{', start ) ) != std::string::npos; start++ )
    {
        size_t pos = start + 1;
        std::string test, function;
        if ( !functional_integrity_take( rows, pos, '"' ) || !functional_integrity_identifier( rows, pos, test ) ||
             test.size() <= 5 || test.compare( 0, 5, "test_" ) != 0 || pos == rows.size() || rows[pos] != '"' )
            continue;
        pos++;
        if ( !functional_integrity_take( rows, pos, ',' ) )
            continue;
        functional_integrity_skip_space( rows, pos );
        if ( functional_integrity_identifier( rows, pos, function ) && functional_integrity_take( rows, pos, ',' ) &&
             empty_functions.count( function ) != 0 )
            failures.insert( test );
    }
    return failures;
}

static void test_functional_cases_have_no_generated_empty_bodies()
{
    const std::string row = "{ \"test_windows_delivery\", windows_delivery, NULL },\n";
    const std::set<std::string> expected = { "test_windows_delivery" };
    rocketnet_unit_check( functional_integrity_empty_cases( "inline void windows_delivery() {}", row ) == expected );
    rocketnet_unit_check( functional_integrity_empty_cases( "void\twindows_delivery ( void ) { }", row ) == expected );
    rocketnet_unit_check( functional_integrity_empty_cases( "avoid windows_delivery() {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery(voidish) {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery() {}", "{ \"test_\", windows_delivery, NULL }," ).empty() );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery(void) {\\n/* no checks */\\n}", row ) == expected );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery() { check(delivered); }", row ).empty() );
    rocketnet_unit_check( functional_integrity_empty_cases( "void unrelated_callback() {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery() {}", "// " + row ).empty() );
    const std::string long_comment( 512 * 1024, 'x' );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery() {/*" + long_comment + "*/}", row ) == expected );
    rocketnet_unit_check( functional_integrity_empty_cases( "//" + long_comment + "\\nvoid windows_delivery() {}", row ) == expected );
    rocketnet_unit_check( functional_integrity_empty_cases( "void windows_delivery() {/*" + long_comment + "*/ check(delivered); }", row ).empty() );

    const std::string cmake = functional_integrity_read( "CMakeLists.txt" );
    const std::string registry = functional_integrity_read( "tests/functional/rocketnet_functional_tests.cpp" );
    // A missing or renamed input must not silently turn this into an empty scan.
    rocketnet_unit_check( cmake.find( "ROCKETNET_FUNCTIONAL_PRODUCTION_SHIM_HEADER" ) != std::string::npos );
    rocketnet_unit_check( registry.find( "rocketnet_functional_tests[]" ) != std::string::npos );
    const std::set<std::string> failures = functional_integrity_empty_cases( cmake, registry );
    for ( std::set<std::string>::const_iterator it = failures.begin(); it != failures.end(); ++it )
        printf( "functional case %s has a generated empty body\n", it->c_str() );
    rocketnet_unit_check( failures.empty() );
}

REGISTER_UNIT_TEST( test_functional_cases_have_no_generated_empty_bodies );
