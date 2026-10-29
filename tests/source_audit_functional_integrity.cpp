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
#include <regex>
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

static std::set<std::string> functional_integrity_empty_cases( std::string generated, const std::string & registry )
{
    // CMake's quoted CONTENT carries escaped newlines. Decode those before
    // removing C++ comments, including comments inside otherwise empty bodies.
    for ( size_t pos = 0; ( pos = generated.find( "\\n", pos ) ) != std::string::npos; pos++ )
        generated.replace( pos, 2, "\n" );
    generated = functional_integrity_without_comments( generated );
    const std::string rows = functional_integrity_without_comments( registry );
    const std::regex empty_body( "\\bvoid\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*(?:void\\s*)?\\)\\s*\\{\\s*\\}" );
    const std::regex registration( "\\{\\s*\"(test_[A-Za-z0-9_]+)\"\\s*,\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*," );
    std::set<std::string> empty_functions;
    for ( std::sregex_iterator it( generated.begin(), generated.end(), empty_body ), end; it != end; ++it )
        empty_functions.insert( ( *it )[1].str() );
    std::set<std::string> failures;
    for ( std::sregex_iterator it( rows.begin(), rows.end(), registration ), end; it != end; ++it )
        if ( empty_functions.count( ( *it )[2].str() ) != 0 )
            failures.insert( ( *it )[1].str() );
    return failures;
}

static void test_functional_cases_have_no_generated_empty_bodies()
{
    const std::string row = "{ \"test_windows_delivery\", windows_delivery, NULL },\n";
    const std::set<std::string> expected = { "test_windows_delivery" };
    rocketnet_unit_check( functional_integrity_empty_cases( "inline void windows_delivery() {}", row ) == expected );
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
