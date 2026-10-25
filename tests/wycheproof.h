/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// tests/wycheproof.h -- a strict JSON reader for the vendored Wycheproof
// known-answer files in tests/wycheproof/. The parser accepts the JSON subset
// those files use and refuses everything else: objects, arrays, strings
// (escapes \\ " \ / \b \f \n \r \t and \uXXXX only for code points below
// 0x80), integers (optional minus, digits, no fraction or exponent), true,
// false, null and whitespace. It refuses trailing data after the top value,
// a trailing comma, an unterminated string or container, a duplicate key in
// one object, nesting deeper than 16, and any other byte.
//
// The files are never read at run time: the unit build compiles them into
// ./bin/test (cmake/EmbedTestFixtures.cmake), and wycheproof_load reads the
// embedded bytes through rocketnet_test_fixture.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace wyj
{

    enum node_type_t
    {
        NODE_OBJ,
        NODE_ARR,
        NODE_STR,
        NODE_INT,
        NODE_TRUE,
        NODE_FALSE,
        NODE_NULL
    };

    struct node_t
    {
        node_type_t type;
        std::string text;                                   // NODE_STR value or NODE_INT decimal text
        std::vector<std::pair<std::string, node_t>> obj;    // NODE_OBJ members, in order
        std::vector<node_t> arr;                            // NODE_ARR elements
    };

    namespace
    {

        const int k_max_depth = 16;

        inline void skip_ws( const std::string & s, size_t & i )
        {
            while ( i < s.size() && ( s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ) )
                i++;
        }

        inline bool hex_digit( char c, int & v )
        {
            if ( c >= '0' && c <= '9' )
            {
                v = c - '0';
                return true;
            }
            if ( c >= 'a' && c <= 'f' )
            {
                v = c - 'a' + 10;
                return true;
            }
            if ( c >= 'A' && c <= 'F' )
            {
                v = c - 'A' + 10;
                return true;
            }
            return false;
        }

        bool parse_value( const std::string & s, size_t & i, node_t & out, int depth );

        bool parse_string( const std::string & s, size_t & i, std::string & out )
        {
            if ( i >= s.size() || s[i] != '"' )
                return false;
            i++;
            out.clear();
            while ( i < s.size() )
            {
                unsigned char c = (unsigned char) s[i];
                if ( c == '"' )
                {
                    i++;
                    return true;
                }
                if ( c == '\\' )
                {
                    i++;
                    if ( i >= s.size() )
                        return false;
                    char e = s[i];
                    switch ( e )
                    {
                        case '"':
                            out += '"';
                            i++;
                            break;
                        case '\\':
                            out += '\\';
                            i++;
                            break;
                        case '/':
                            out += '/';
                            i++;
                            break;
                        case 'b':
                            out += '\b';
                            i++;
                            break;
                        case 'f':
                            out += '\f';
                            i++;
                            break;
                        case 'n':
                            out += '\n';
                            i++;
                            break;
                        case 'r':
                            out += '\r';
                            i++;
                            break;
                        case 't':
                            out += '\t';
                            i++;
                            break;
                        case 'u':
                        {
                            i++;
                            if ( i + 4 > s.size() )
                                return false;
                            int value = 0;
                            for ( size_t k = 0; k < 4; k++ )
                            {
                                int v;
                                if ( !hex_digit( s[i + k], v ) )
                                    return false;
                                value = ( value << 4 ) | v;
                            }
                            if ( value >= 0x80 )
                                return false;
                            out += (char) value;
                            i += 4;
                            break;
                        }
                        default:
                            return false;
                    }
                    continue;
                }
                if ( c < 0x20 || c >= 0x80 )
                    return false;
                out += (char) c;
                i++;
            }
            return false;
        }

        bool parse_int( const std::string & s, size_t & i, node_t & out )
        {
            size_t start = i;
            if ( i < s.size() && s[i] == '-' )
                i++;
            if ( i >= s.size() || s[i] < '0' || s[i] > '9' )
                return false;
            while ( i < s.size() && s[i] >= '0' && s[i] <= '9' )
                i++;
            out.type = NODE_INT;
            out.text = s.substr( start, i - start );
            return true;
        }

        bool parse_object( const std::string & s, size_t & i, node_t & out, int depth )
        {
            if ( depth + 1 > k_max_depth )
                return false;
            depth++;
            if ( i >= s.size() || s[i] != '{' )
                return false;
            i++;
            node_t o;
            o.type = NODE_OBJ;
            skip_ws( s, i );
            if ( i < s.size() && s[i] == '}' )
            {
                i++;
                out = o;
                return true;
            }
            while ( true )
            {
                skip_ws( s, i );
                std::string key;
                if ( !parse_string( s, i, key ) )
                    return false;
                for ( size_t k = 0; k < o.obj.size(); k++ )
                    if ( o.obj[k].first == key )
                        return false;
                skip_ws( s, i );
                if ( i >= s.size() || s[i] != ':' )
                    return false;
                i++;
                skip_ws( s, i );
                node_t value;
                if ( !parse_value( s, i, value, depth ) )
                    return false;
                o.obj.push_back( std::make_pair( key, value ) );
                skip_ws( s, i );
                if ( i >= s.size() )
                    return false;
                if ( s[i] == ',' )
                {
                    i++;
                    continue;
                }
                if ( s[i] == '}' )
                {
                    i++;
                    out = o;
                    return true;
                }
                return false;
            }
        }

        bool parse_array( const std::string & s, size_t & i, node_t & out, int depth )
        {
            if ( depth + 1 > k_max_depth )
                return false;
            depth++;
            if ( i >= s.size() || s[i] != '[' )
                return false;
            i++;
            node_t o;
            o.type = NODE_ARR;
            skip_ws( s, i );
            if ( i < s.size() && s[i] == ']' )
            {
                i++;
                out = o;
                return true;
            }
            while ( true )
            {
                skip_ws( s, i );
                node_t value;
                if ( !parse_value( s, i, value, depth ) )
                    return false;
                o.arr.push_back( value );
                skip_ws( s, i );
                if ( i >= s.size() )
                    return false;
                if ( s[i] == ',' )
                {
                    i++;
                    continue;
                }
                if ( s[i] == ']' )
                {
                    i++;
                    out = o;
                    return true;
                }
                return false;
            }
        }

        bool parse_value( const std::string & s, size_t & i, node_t & out, int depth )
        {
            if ( i >= s.size() )
                return false;
            char c = s[i];
            if ( c == '{' )
                return parse_object( s, i, out, depth );
            if ( c == '[' )
                return parse_array( s, i, out, depth );
            if ( c == '"' )
            {
                std::string text;
                if ( !parse_string( s, i, text ) )
                    return false;
                out.type = NODE_STR;
                out.text = text;
                return true;
            }
            if ( c == '-' || ( c >= '0' && c <= '9' ) )
                return parse_int( s, i, out );
            if ( s.compare( i, 4, "true" ) == 0 )
            {
                out.type = NODE_TRUE;
                i += 4;
                return true;
            }
            if ( s.compare( i, 5, "false" ) == 0 )
            {
                out.type = NODE_FALSE;
                i += 5;
                return true;
            }
            if ( s.compare( i, 4, "null" ) == 0 )
            {
                out.type = NODE_NULL;
                i += 4;
                return true;
            }
            return false;
        }

        const node_t * find_member( const node_t & o, const std::string & key )
        {
            if ( o.type != NODE_OBJ )
                return NULL;
            for ( size_t k = 0; k < o.obj.size(); k++ )
                if ( o.obj[k].first == key )
                    return &o.obj[k].second;
            return NULL;
        }

    } // namespace

    inline bool parse_text( const std::string & s, node_t & root )
    {
        size_t i = 0;
        skip_ws( s, i );
        if ( !parse_value( s, i, root, 0 ) )
            return false;
        skip_ws( s, i );
        if ( i != s.size() )
            return false;
        return true;
    }

} // namespace wyj

struct wycheproof_test_t
{
    int tc_id;
    std::string result;
    std::string comment;
    std::vector<std::string> flags;
    std::map<std::string, std::string> fields;   // every string member of the test except result and comment
    std::map<std::string, std::string> group;    // every string or integer member of the testGroup (an integer as decimal text),
                                                 // plus every string member of a nested object as "<object>.<member>"
};

inline bool wycheproof_hex( const std::string & text, std::vector<uint8_t> & out )
{
    out.clear();
    if ( text.size() % 2 != 0 )
        return false;
    for ( size_t i = 0; i < text.size(); i += 2 )
    {
        int hi, lo;
        if ( !wyj::hex_digit( text[i], hi ) || !wyj::hex_digit( text[i + 1], lo ) )
            return false;
        out.push_back( (uint8_t) ( ( hi << 4 ) | lo ) );
    }
    return true;
}

// The test fixtures compiled into the unit binary at build time
// (cmake/EmbedTestFixtures.cmake, the generated rocketnet_test_fixtures.cpp):
// the bytes of the committed file at path, relative to the repository root
// ("tests/wycheproof/x25519_test.json"), and its size in *bytes; NULL, with
// *bytes 0, for a path that was not embedded.
const unsigned char * rocketnet_test_fixture( const char * path, size_t * bytes );

// the vendor files: tests/wycheproof/<file_name>, from the embedded copy
inline bool wycheproof_read_file( const char * file_name, std::string & out )
{
    out.clear();
    const std::string path = std::string( "tests/wycheproof/" ) + file_name;
    size_t bytes = 0;
    const unsigned char * data = rocketnet_test_fixture( path.c_str(), &bytes );
    if ( !data )
        return false;
    out.assign( reinterpret_cast<const char *>( data ), bytes );
    return true;
}

inline void wycheproof_fill_group_map( const wyj::node_t & g, std::map<std::string, std::string> & m )
{
    m.clear();
    for ( size_t k = 0; k < g.obj.size(); k++ )
    {
        const std::string & key = g.obj[k].first;
        const wyj::node_t & v = g.obj[k].second;
        if ( v.type == wyj::NODE_STR || v.type == wyj::NODE_INT )
        {
            m[key] = v.text;
        }
        else if ( v.type == wyj::NODE_OBJ )
        {
            for ( size_t j = 0; j < v.obj.size(); j++ )
                if ( v.obj[j].second.type == wyj::NODE_STR )
                    m[key + "." + v.obj[j].first] = v.obj[j].second.text;
        }
    }
}

inline bool wycheproof_fill_test( const wyj::node_t & t, wycheproof_test_t & test )
{
    const wyj::node_t * id = wyj::find_member( t, "tcId" );
    const wyj::node_t * res = wyj::find_member( t, "result" );
    if ( !id || !res )
        return false;
    if ( id->type != wyj::NODE_INT || res->type != wyj::NODE_STR )
        return false;
    test.tc_id = std::atoi( id->text.c_str() );
    test.result = res->text;
    test.comment.clear();
    const wyj::node_t * comment = wyj::find_member( t, "comment" );
    if ( comment && comment->type == wyj::NODE_STR )
        test.comment = comment->text;
    test.flags.clear();
    const wyj::node_t * flags = wyj::find_member( t, "flags" );
    if ( flags && flags->type == wyj::NODE_ARR )
        for ( size_t i = 0; i < flags->arr.size(); i++ )
            if ( flags->arr[i].type == wyj::NODE_STR )
                test.flags.push_back( flags->arr[i].text );
    test.fields.clear();
    for ( size_t i = 0; i < t.obj.size(); i++ )
    {
        const std::string & key = t.obj[i].first;
        const wyj::node_t & v = t.obj[i].second;
        if ( key == "result" || key == "comment" )
            continue;
        if ( v.type == wyj::NODE_STR )
            test.fields[key] = v.text;
    }
    return true;
}

inline bool wycheproof_load( const char * file_name,
                             std::vector<wycheproof_test_t> & tests,
                             int * number_of_tests,
                             int * number_of_groups )
{
    tests.clear();
    *number_of_tests = 0;
    *number_of_groups = 0;

    std::string text;
    if ( !wycheproof_read_file( file_name, text ) )
        return false;

    wyj::node_t root;
    if ( !wyj::parse_text( text, root ) )
        return false;
    if ( root.type != wyj::NODE_OBJ )
        return false;

    const wyj::node_t * groups_node = wyj::find_member( root, "testGroups" );
    if ( !groups_node || groups_node->type != wyj::NODE_ARR )
        return false;

    const size_t num_groups = groups_node->arr.size();
    int total = 0;
    (void) total;
    for ( size_t gi = 0; gi < num_groups; gi++ )
    {
        const wyj::node_t & g = groups_node->arr[gi];
        if ( g.type != wyj::NODE_OBJ )
            return false;
        std::map<std::string, std::string> group_map;
        wycheproof_fill_group_map( g, group_map );
        const wyj::node_t * tg = wyj::find_member( g, "tests" );
        if ( !tg || tg->type != wyj::NODE_ARR )
            return false;
        for ( size_t ti = 0; ti < tg->arr.size(); ti++ )
        {
            const wyj::node_t & t = tg->arr[ti];
            if ( t.type != wyj::NODE_OBJ )
                return false;
            wycheproof_test_t test;
            if ( !wycheproof_fill_test( t, test ) )
                return false;
            test.group = group_map;
            tests.push_back( test );
            total++;
        }
    }

    *number_of_groups = (int) num_groups;
    const wyj::node_t * nt = wyj::find_member( root, "numberOfTests" );
    if ( nt && nt->type == wyj::NODE_INT )
        *number_of_tests = std::atoi( nt->text.c_str() );
    else
        *number_of_tests = (int) tests.size();
    return true;
}
