/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_SOURCE_AUDIT_TEXT_H
#define ROCKETNET_TESTS_SOURCE_AUDIT_TEXT_H

#include <stddef.h>
#include <string>

// The source audits read C and C++ text. A name in a comment is not code: an
// audit that matched comments passed with the code it guards commented out
// (VERIFY-3 W16, W18). Comments become spaces, newlines kept, so offsets map
// to the same lines. With blank_literals, string and character literals become
// spaces too, so nothing quoted can match; an audit that reads names a table
// holds as strings keeps them.

static inline std::string source_audit_code_only( const std::string & source, bool blank_literals = true )
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
            {
                line_comment = false;
            }
            else
            {
                code[i] = ' ';
            }
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
            {
                code[i] = ' ';
            }
            continue;
        }
        if ( string_literal || char_literal )
        {
            if ( c != '\n' && blank_literals )
            {
                code[i] = ' ';
            }
            if ( escaped )
            {
                escaped = false;
            }
            else if ( c == '\\' )
            {
                escaped = true;
            }
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
            if ( blank_literals )
            {
                code[i] = ' ';
            }
            string_literal = true;
        }
        else if ( c == '\'' )
        {
            if ( blank_literals )
            {
                code[i] = ' ';
            }
            char_literal = true;
        }
    }
    return code;
}

#endif // ROCKETNET_TESTS_SOURCE_AUDIT_TEXT_H
