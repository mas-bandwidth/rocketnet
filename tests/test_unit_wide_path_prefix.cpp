/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Pure string tests for rocketnet_test_wide_path (tests/resource_file.h):
// forward slashes converted to backslashes before adding \\?\, UNC paths
// converted to \\?\UNC\server\share\..., and already-prefixed paths returned
// unchanged. No file access, no sockets, no clock.

#include "test_unit_registry.h"
#include "resource_file.h"

#include <string>

static void test_wide_path_prefix_forward_slashes()
{
    const std::wstring short_forward = rocketnet_test_wide_path( "C:/a/b" );
    rocketnet_unit_check( short_forward.compare( 0, 4, L"\\\\?\\" ) == 0 );
    rocketnet_unit_check( short_forward.find( L'/' ) == std::wstring::npos );
    rocketnet_unit_check( short_forward == L"\\\\?\\C:\\a\\b" );
    rocketnet_unit_check( rocketnet_test_wide_path( "C:\\a\\b" ) == L"\\\\?\\C:\\a\\b" );

    const std::string segment( 70, 'a' );
    const std::string long_forward =
        "C:/" + segment + "/" + segment + "/" + segment + "/" + segment + "/resource.bin";
    const std::wstring wsegment( 70, L'a' );
    const std::wstring expected_long =
        L"\\\\?\\C:\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\resource.bin";
    rocketnet_unit_check( long_forward.size() > 260 );
    const std::wstring actual_long = rocketnet_test_wide_path( long_forward );
    rocketnet_unit_check( actual_long.compare( 0, 4, L"\\\\?\\" ) == 0 );
    rocketnet_unit_check( actual_long.find( L'/' ) == std::wstring::npos );
    rocketnet_unit_check( actual_long == expected_long );
}

static void test_wide_path_prefix_unc()
{
    const std::wstring unc_backslash = rocketnet_test_wide_path( "\\\\server\\share\\x" );
    rocketnet_unit_check( unc_backslash.compare( 0, 21, L"\\\\?\\UNC\\server\\share\\" ) == 0 );
    rocketnet_unit_check( unc_backslash.find( L'/' ) == std::wstring::npos );
    rocketnet_unit_check( unc_backslash == L"\\\\?\\UNC\\server\\share\\x" );

    const std::wstring unc_forward = rocketnet_test_wide_path( "//server/share/x" );
    rocketnet_unit_check( unc_forward.compare( 0, 21, L"\\\\?\\UNC\\server\\share\\" ) == 0 );
    rocketnet_unit_check( unc_forward.find( L'/' ) == std::wstring::npos );
    rocketnet_unit_check( unc_forward == L"\\\\?\\UNC\\server\\share\\x" );

    const std::string segment( 70, 'b' );
    const std::string long_unc =
        "//server/share/" + segment + "/" + segment + "/" + segment + "/" + segment + "/x";
    const std::wstring wsegment( 70, L'b' );
    const std::wstring expected_long_unc =
        L"\\\\?\\UNC\\server\\share\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\x";
    rocketnet_unit_check( long_unc.size() > 260 );
    const std::wstring actual_long_unc = rocketnet_test_wide_path( long_unc );
    rocketnet_unit_check( actual_long_unc.compare( 0, 21, L"\\\\?\\UNC\\server\\share\\" ) == 0 );
    rocketnet_unit_check( actual_long_unc.find( L'/' ) == std::wstring::npos );
    rocketnet_unit_check( actual_long_unc == expected_long_unc );
}

static void test_wide_path_prefix_already_prefixed()
{
    const std::string segment( 70, 'c' );
    const std::string long_prefixed =
        "\\\\?\\C:\\" + segment + "\\" + segment + "\\" + segment + "\\" + segment + "\\x";
    const std::wstring wsegment( 70, L'c' );
    const std::wstring expected_long_prefixed =
        L"\\\\?\\C:\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\" + wsegment + L"\\x";
    rocketnet_unit_check( long_prefixed.size() > 260 );
    rocketnet_unit_check( rocketnet_test_wide_path( "\\\\?\\C:\\a\\b" ) == L"\\\\?\\C:\\a\\b" );
    rocketnet_unit_check( rocketnet_test_wide_path( "\\\\?\\UNC\\server\\share\\x" ) == L"\\\\?\\UNC\\server\\share\\x" );
    rocketnet_unit_check( rocketnet_test_wide_path( long_prefixed ) == expected_long_prefixed );
}

REGISTER_UNIT_TEST( test_wide_path_prefix_forward_slashes );
REGISTER_UNIT_TEST( test_wide_path_prefix_unc );
REGISTER_UNIT_TEST( test_wide_path_prefix_already_prefixed );
