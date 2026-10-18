/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#include <stdio.h>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline std::wstring rocketnet_test_utf8_to_wide( const std::string & text )
{
    if ( text.empty() )
        return L"";
    const int length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int) text.size(), NULL, 0 );
    if ( length == 0 )
        return L"";
    std::wstring wide( length, L'\0' );
    if ( MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int) text.size(), &wide[0], length ) != length )
        return L"";
    return wide;
}

inline std::string rocketnet_test_wide_to_utf8( const wchar_t * text, int length )
{
    const int bytes = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, text, length, NULL, 0, NULL, NULL );
    if ( bytes == 0 )
        return "";
    std::string utf8( bytes, '\0' );
    if ( WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, text, length, &utf8[0], bytes, NULL, NULL ) != bytes )
        return "";
    return utf8;
}

inline std::wstring rocketnet_test_wide_path( const std::string & path )
{
    std::wstring wide = rocketnet_test_utf8_to_wide( path );
    if ( wide.size() >= MAX_PATH && wide.compare( 0, 4, L"\\\\?\\" ) != 0 )
    {
        if ( wide.compare( 0, 2, L"\\\\" ) == 0 )
            return L"\\\\?\\UNC" + wide.substr( 1 );
        return L"\\\\?\\" + wide;
    }
    return wide;
}
#endif

inline FILE * rocketnet_test_fopen( const std::string & path, const char * mode )
{
#if defined(_WIN32)
    const std::wstring wide = rocketnet_test_wide_path( path );
    if ( wide.empty() )
        return NULL;
    const std::wstring wide_mode = rocketnet_test_utf8_to_wide( mode );
    if ( wide_mode.empty() )
        return NULL;
    return _wfopen( wide.c_str(), wide_mode.c_str() );
#else
    return fopen( path.c_str(), mode );
#endif
}
