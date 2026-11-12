/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A registered functional case must do work. An empty body, or one that only
// returns or casts its arguments away, passes on every host and tests
// nothing. This audit reads every tests/functional source (CMake globs the
// list into ROCKETNET_FUNCTIONAL_SOURCES) and the production shim CMake
// generates, finds every `void name()` definition in them, and fails each
// `{ "test_...", function, ... }` row, in any table in any of those sources,
// whose function has an empty definition anywhere. The preprocessor is not
// evaluated: a body under a Windows-only or Linux-only #if is read on every
// audit host. This checks that specific source-policy boundary, not general
// C++ behavior.

#include "test_unit_registry.h"
#include "portable_crt.h"

#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::string functional_integrity_read( const std::string & relative )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    rocketnet_unit_check( file != NULL );
    std::string text;
    char buffer[4096];
    size_t bytes;
    while ( ( bytes = fread( buffer, 1, sizeof( buffer ), file ) ) != 0 )
    {
        text.append( buffer, bytes );
    }
    const bool read_ok = ferror( file ) == 0;
    const int close_status = fclose( file );
    rocketnet_unit_check( read_ok && close_status == 0 );
    return text;
}

static bool functional_integrity_word_char( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

static std::string functional_integrity_without_comments( const std::string & text )
{
    // libstdc++'s regex engine recurses per character for an unbounded comment
    // match. Repository-sized inputs can exhaust a Debug audit's stack. Skip
    // comments iteratively and retain a separator so identifiers never join.
    // String and character literals are copied whole, so a "//" inside one is
    // not a comment. A comment's newlines are kept, so positions keep their
    // line numbers. A quote after a word character is a digit separator.
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
            const size_t found = text.find( "*/", i + 2 );
            const size_t end = found == std::string::npos ? text.size() : found + 2;
            code += ' ';
            for ( size_t j = i; j < end; j++ )
            {
                if ( text[j] == '\n' )
                {
                    code += '\n';
                }
            }
            i = end;
        }
        else if ( text[i] == '"' || ( text[i] == '\'' && ( i == 0 || !functional_integrity_word_char( text[i - 1] ) ) ) )
        {
            const char quote = text[i];
            code += text[i++];
            while ( i < text.size() && text[i] != quote && text[i] != '\n' )
            {
                if ( text[i] == '\\' && i + 1 < text.size() )
                {
                    code += text[i++];
                }
                code += text[i++];
            }
            if ( i < text.size() && text[i] == quote )
            {
                code += text[i++];
            }
        }
        else
        {
            code += text[i++];
        }
    }
    return code;
}

static void functional_integrity_skip_space( const std::string & text, size_t & pos )
{
    while ( pos < text.size() && ( text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                   text[pos] == '\r' || text[pos] == '\f' || text[pos] == '\v' ) )
    {
        pos++;
    }
}

static bool functional_integrity_take( const std::string & text, size_t & pos, char c )
{
    functional_integrity_skip_space( text, pos );
    if ( pos == text.size() || text[pos] != c )
    {
        return false;
    }
    pos++;
    return true;
}

static bool functional_integrity_identifier( const std::string & text, size_t & pos, std::string & name )
{
    if ( pos == text.size() || !functional_integrity_word_char( text[pos] ) || ( text[pos] >= '0' && text[pos] <= '9' ) )
    {
        return false;
    }
    const size_t first = pos++;
    while ( pos < text.size() && functional_integrity_word_char( text[pos] ) )
    {
        pos++;
    }
    name = text.substr( first, pos - first );
    return true;
}

// The word at pos, if it is exactly `word` and not the start of a longer one.
static bool functional_integrity_keyword( const std::string & text, size_t & pos, const char * word )
{
    functional_integrity_skip_space( text, pos );
    size_t at = pos;
    std::string name;
    if ( !functional_integrity_identifier( text, at, name ) || name != word )
    {
        return false;
    }
    pos = at;
    return true;
}

// One statement that does no work: `return;`, `return 0;` or `(void) name;`.
static bool functional_integrity_no_work_statement( const std::string & code, size_t & pos )
{
    size_t at = pos;
    if ( functional_integrity_keyword( code, at, "return" ) )
    {
        const size_t after_return = at;
        if ( functional_integrity_take( code, at, ';' ) )
        {
            pos = at;
            return true;
        }
        at = after_return;
        if ( functional_integrity_take( code, at, '0' ) && ( at == code.size() || !functional_integrity_word_char( code[at] ) ) &&
             functional_integrity_take( code, at, ';' ) )
        {
            pos = at;
            return true;
        }
        return false;
    }
    at = pos;
    std::string name;
    if ( functional_integrity_take( code, at, '(' ) && functional_integrity_keyword( code, at, "void" ) &&
         functional_integrity_take( code, at, ')' ) )
    {
        functional_integrity_skip_space( code, at );
        if ( functional_integrity_identifier( code, at, name ) && functional_integrity_take( code, at, ';' ) )
        {
            pos = at;
            return true;
        }
    }
    return false;
}

static int functional_integrity_line( const std::string & code, size_t pos )
{
    int line = 1;
    for ( size_t i = 0; i < pos && i < code.size(); i++ )
    {
        if ( code[i] == '\n' )
        {
            line++;
        }
    }
    return line;
}

// Every `void name()` / `void name( void )` definition in comment-stripped
// code: the function's name, and where its body is, for each definition whose
// body does no work.
struct functional_integrity_definitions_t
{
    std::set<std::string> defined;
    std::map<std::string, std::vector<std::string>> empty;
};

static void functional_integrity_scan_definitions( const std::string & code, const std::string & file, int fixed_line,
                                                   functional_integrity_definitions_t & definitions )
{
    for ( size_t start = 0; ( start = code.find( "void", start ) ) != std::string::npos; start++ )
    {
        if ( start != 0 && functional_integrity_word_char( code[start - 1] ) )
        {
            continue;
        }
        size_t pos = start;
        std::string result, name;
        if ( !functional_integrity_identifier( code, pos, result ) || result != "void" )
        {
            continue;
        }
        const size_t before_space = pos;
        functional_integrity_skip_space( code, pos );
        if ( pos == before_space || !functional_integrity_identifier( code, pos, name ) || !functional_integrity_take( code, pos, '(' ) )
        {
            continue;
        }
        size_t at = pos;
        if ( functional_integrity_keyword( code, at, "void" ) )
        {
            pos = at;
        }
        if ( !functional_integrity_take( code, pos, ')' ) || !functional_integrity_take( code, pos, '{' ) )
        {
            continue;
        }
        definitions.defined.insert( name );
        const size_t body = pos - 1;
        while ( functional_integrity_no_work_statement( code, pos ) )
        {
        }
        if ( functional_integrity_take( code, pos, '}' ) )
        {
            const int line = fixed_line != 0 ? fixed_line : functional_integrity_line( code, body );
            definitions.empty[name].push_back( file + ":" + std::to_string( line ) );
        }
    }
}

// The functional sources generate whole families of cases from function-like
// macros (one `void name##_cell()` per transport cell). A definition a macro
// writes is only visible once the macro is expanded, so the audit expands
// the macros that write definitions, at each place they are invoked, and
// reads the expansion. It expands nothing else: no object-like macro, no
// conditional, no include.
struct functional_integrity_macro_t
{
    std::vector<std::string> params;
    std::string body;
};

typedef std::map<std::string, functional_integrity_macro_t> functional_integrity_macros_t;

static bool functional_integrity_continued( const std::string & code, size_t line_end )
{
    size_t at = line_end;
    if ( at > 0 && code[at - 1] == '\r' )
    {
        at--;
    }
    return at > 0 && code[at - 1] == '\\';
}

// The code with every preprocessor directive (and its continuation lines)
// blanked, newlines kept; each `#define NAME( params ) body` is recorded.
static std::string functional_integrity_split_directives( const std::string & code, functional_integrity_macros_t & macros )
{
    std::string rest = code;
    size_t line_start = 0;
    while ( line_start < code.size() )
    {
        size_t first = line_start;
        while ( first < code.size() && ( code[first] == ' ' || code[first] == '\t' ) )
        {
            first++;
        }
        size_t end = code.find( '\n', line_start );
        end = end == std::string::npos ? code.size() : end;
        if ( first == code.size() || code[first] != '#' )
        {
            line_start = end + 1;
            continue;
        }
        while ( end < code.size() && functional_integrity_continued( code, end ) )
        {
            const size_t next = code.find( '\n', end + 1 );
            end = next == std::string::npos ? code.size() : next;
        }
        std::string logical;
        for ( size_t i = first + 1; i < end; i++ )
        {
            if ( code[i] == '\\' && ( code.compare( i + 1, 1, "\n" ) == 0 || code.compare( i + 1, 2, "\r\n" ) == 0 ) )
            {
                logical += ' ';
                i += code[i + 1] == '\r' ? size_t( 2 ) : size_t( 1 );
                continue;
            }
            logical += code[i];
        }
        for ( size_t i = line_start; i < end; i++ )
        {
            if ( rest[i] != '\n' )
            {
                rest[i] = ' ';
            }
        }
        line_start = end + 1;
        size_t pos = 0;
        std::string directive, name;
        functional_integrity_skip_space( logical, pos );
        if ( !functional_integrity_identifier( logical, pos, directive ) || directive != "define" )
        {
            continue;
        }
        functional_integrity_skip_space( logical, pos );
        if ( !functional_integrity_identifier( logical, pos, name ) || pos == logical.size() || logical[pos] != '(' )
        {
            continue;
        }
        pos++;
        functional_integrity_macro_t macro;
        std::string param;
        bool closed = functional_integrity_take( logical, pos, ')' );
        while ( !closed )
        {
            functional_integrity_skip_space( logical, pos );
            if ( !functional_integrity_identifier( logical, pos, param ) )
            {
                break;
            }
            macro.params.push_back( param );
            if ( functional_integrity_take( logical, pos, ')' ) )
            {
                closed = true;
            }
            else if ( !functional_integrity_take( logical, pos, ',' ) )
            {
                break;
            }
        }
        if ( closed )
        {
            macro.body = logical.substr( pos );
            macros[name] = macro;
        }
    }
    return rest;
}

// A macro writes definitions when its body holds `void <identifier>`.
static bool functional_integrity_writes_definitions( const functional_integrity_macro_t & macro )
{
    const std::string & body = macro.body;
    for ( size_t start = 0; ( start = body.find( "void", start ) ) != std::string::npos; start++ )
    {
        size_t pos = start;
        std::string word, name;
        if ( ( start != 0 && functional_integrity_word_char( body[start - 1] ) ) || !functional_integrity_identifier( body, pos, word ) ||
             word != "void" )
        {
            continue;
        }
        functional_integrity_skip_space( body, pos );
        if ( functional_integrity_identifier( body, pos, name ) )
        {
            return true;
        }
    }
    return false;
}

static const functional_integrity_macro_t * functional_integrity_generator( const std::string & name, const functional_integrity_macros_t & file_macros,
                                                                            const functional_integrity_macros_t & header_macros,
                                                                            const std::set<std::string> & generators )
{
    if ( generators.count( name ) == 0 )
    {
        return NULL;
    }
    functional_integrity_macros_t::const_iterator it = file_macros.find( name );
    if ( it != file_macros.end() )
    {
        return &it->second;
    }
    it = header_macros.find( name );
    return it != header_macros.end() ? &it->second : NULL;
}

// At pos, just past a generator's name: its parenthesised arguments, split at
// top-level commas and trimmed. pos ends past the closing parenthesis.
static bool functional_integrity_arguments( const std::string & code, size_t & pos, std::vector<std::string> & args )
{
    if ( !functional_integrity_take( code, pos, '(' ) )
    {
        return false;
    }
    int depth = 0;
    std::string arg;
    while ( pos < code.size() )
    {
        const char c = code[pos++];
        if ( c == '"' || c == '\'' )
        {
            arg += c;
            while ( pos < code.size() && code[pos] != c )
            {
                if ( code[pos] == '\\' && pos + 1 < code.size() )
                {
                    arg += code[pos++];
                }
                arg += code[pos++];
            }
            if ( pos < code.size() )
            {
                arg += code[pos++];
            }
            continue;
        }
        if ( depth == 0 && ( c == ',' || c == ')' ) )
        {
            size_t a = 0;
            size_t b = arg.size();
            while ( a < b && ( arg[a] == ' ' || arg[a] == '\t' || arg[a] == '\n' || arg[a] == '\r' ) )
            {
                a++;
            }
            while ( b > a && ( arg[b - 1] == ' ' || arg[b - 1] == '\t' || arg[b - 1] == '\n' || arg[b - 1] == '\r' ) )
            {
                b--;
            }
            args.push_back( arg.substr( a, b - a ) );
            arg.clear();
            if ( c == ')' )
            {
                return true;
            }
            continue;
        }
        depth += c == '(' ? 1 : c == ')' ? -1
                                         : 0;
        arg += c;
    }
    return false;
}

static std::string functional_integrity_expand_text( const std::string & text, const functional_integrity_macros_t & file_macros,
                                                     const functional_integrity_macros_t & header_macros, const std::set<std::string> & generators,
                                                     int depth );

// One generator invocation: the parameters substituted (with # and ##), then
// any generator the result invokes expanded in turn.
static std::string functional_integrity_expand_one( const functional_integrity_macro_t & macro, const std::vector<std::string> & args,
                                                    const functional_integrity_macros_t & file_macros,
                                                    const functional_integrity_macros_t & header_macros, const std::set<std::string> & generators,
                                                    int depth )
{
    const std::string & body = macro.body;
    std::string out;
    for ( size_t i = 0; i < body.size(); )
    {
        if ( body[i] == '"' )
        {
            out += body[i++];
            while ( i < body.size() && body[i] != '"' )
            {
                if ( body[i] == '\\' && i + 1 < body.size() )
                {
                    out += body[i++];
                }
                out += body[i++];
            }
            if ( i < body.size() )
            {
                out += body[i++];
            }
            continue;
        }
        if ( body.compare( i, 2, "##" ) == 0 )
        {
            while ( !out.empty() && ( out[out.size() - 1] == ' ' || out[out.size() - 1] == '\t' ) )
            {
                out.erase( out.size() - 1 );
            }
            i += 2;
            while ( i < body.size() && ( body[i] == ' ' || body[i] == '\t' ) )
            {
                i++;
            }
            continue;
        }
        std::string word;
        size_t at = i;
        if ( body[i] == '#' )
        {
            at = i + 1;
            functional_integrity_skip_space( body, at );
            if ( functional_integrity_identifier( body, at, word ) )
            {
                for ( size_t p = 0; p < macro.params.size(); p++ )
                {
                    if ( macro.params[p] == word && p < args.size() )
                    {
                        out += "\"" + args[p] + "\"";
                        i = at;
                        break;
                    }
                }
            }
            if ( i != at )
            {
                out += body[i++];
            }
            continue;
        }
        if ( ( i == 0 || !functional_integrity_word_char( body[i - 1] ) ) && functional_integrity_identifier( body, at, word ) )
        {
            bool substituted = false;
            for ( size_t p = 0; p < macro.params.size(); p++ )
            {
                if ( macro.params[p] == word && p < args.size() )
                {
                    out += args[p];
                    substituted = true;
                    break;
                }
            }
            if ( !substituted )
            {
                out += word;
            }
            i = at;
            continue;
        }
        out += body[i++];
    }
    return functional_integrity_expand_text( out, file_macros, header_macros, generators, depth + 1 );
}

// Every generator invocation in text replaced by its expansion; depth bounds
// a macro that invokes itself.
static std::string functional_integrity_expand_text( const std::string & text, const functional_integrity_macros_t & file_macros,
                                                     const functional_integrity_macros_t & header_macros, const std::set<std::string> & generators,
                                                     int depth )
{
    if ( depth > 8 )
    {
        return text;
    }
    std::string out;
    for ( size_t i = 0; i < text.size(); )
    {
        size_t at = i;
        std::string word;
        std::vector<std::string> args;
        const functional_integrity_macro_t * macro = NULL;
        if ( ( i == 0 || !functional_integrity_word_char( text[i - 1] ) ) && functional_integrity_identifier( text, at, word ) )
        {
            macro = functional_integrity_generator( word, file_macros, header_macros, generators );
            size_t args_at = at;
            if ( macro != NULL && functional_integrity_arguments( text, args_at, args ) )
            {
                out += functional_integrity_expand_one( *macro, args, file_macros, header_macros, generators, depth );
                i = args_at;
                continue;
            }
            out += word;
            i = at;
            continue;
        }
        out += text[i++];
    }
    return out;
}

// Every top-level generator invocation in the code (directives blanked): its
// expansion, and the line it is invoked on.
static void functional_integrity_scan_generated( const std::string & rest, const std::string & file, const functional_integrity_macros_t & file_macros,
                                                 const functional_integrity_macros_t & header_macros, const std::set<std::string> & generators,
                                                 functional_integrity_definitions_t & definitions )
{
    for ( size_t i = 0; i < rest.size(); )
    {
        size_t at = i;
        std::string word;
        std::vector<std::string> args;
        if ( ( i == 0 || !functional_integrity_word_char( rest[i - 1] ) ) && functional_integrity_identifier( rest, at, word ) )
        {
            const functional_integrity_macro_t * macro = functional_integrity_generator( word, file_macros, header_macros, generators );
            size_t args_at = at;
            if ( macro != NULL && functional_integrity_arguments( rest, args_at, args ) )
            {
                const std::string expansion = functional_integrity_expand_one( *macro, args, file_macros, header_macros, generators, 0 );
                functional_integrity_scan_definitions( expansion, file, functional_integrity_line( rest, i ), definitions );
                i = args_at;
                continue;
            }
            i = at;
            continue;
        }
        i++;
    }
}

// Every `{ "test_...", function, ... }` row in comment-stripped code.
struct functional_integrity_row_t
{
    std::string test;
    std::string function;
    std::string where;
};

static void functional_integrity_scan_rows( const std::string & code, const std::string & file, std::vector<functional_integrity_row_t> & rows )
{
    for ( size_t start = 0; ( start = code.find( '{', start ) ) != std::string::npos; start++ )
    {
        size_t pos = start + 1;
        functional_integrity_row_t row;
        if ( !functional_integrity_take( code, pos, '"' ) || !functional_integrity_identifier( code, pos, row.test ) ||
             row.test.size() <= 5 || row.test.compare( 0, 5, "test_" ) != 0 || pos == code.size() || code[pos] != '"' )
        {
            continue;
        }
        pos++;
        if ( !functional_integrity_take( code, pos, ',' ) )
        {
            continue;
        }
        functional_integrity_skip_space( code, pos );
        if ( functional_integrity_identifier( code, pos, row.function ) && functional_integrity_take( code, pos, ',' ) )
        {
            row.where = file + ":" + std::to_string( functional_integrity_line( code, start ) );
            rows.push_back( row );
        }
    }
}

// The quoted CONTENT argument of every file(GENERATE ...) call in a CMake
// text, unescaped, with the line of its CONTENT keyword. Only those strings
// become generated sources, so only they are scanned: the rest of
// CMakeLists.txt is CMake, where "/*" is a glob, not a comment.
struct functional_integrity_generated_t
{
    std::string content;
    int line;
};

static std::vector<functional_integrity_generated_t> functional_integrity_generated_contents( const std::string & cmake )
{
    std::vector<functional_integrity_generated_t> contents;
    for ( size_t at = 0; ( at = cmake.find( "file(GENERATE", at ) ) != std::string::npos; at++ )
    {
        const size_t keyword = cmake.find( "CONTENT", at );
        if ( keyword == std::string::npos )
        {
            break;
        }
        size_t pos = keyword + 7;
        if ( !functional_integrity_take( cmake, pos, '"' ) )
        {
            continue;
        }
        functional_integrity_generated_t generated;
        generated.line = functional_integrity_line( cmake, keyword );
        while ( pos < cmake.size() && cmake[pos] != '"' )
        {
            if ( cmake[pos] == '\\' && pos + 1 < cmake.size() )
            {
                const char escaped = cmake[pos + 1];
                generated.content += escaped == 'n' ? '\n' : escaped == 't' ? '\t'
                                                                            : escaped;
                pos += 2;
                continue;
            }
            generated.content += cmake[pos++];
        }
        contents.push_back( generated );
        at = pos;
    }
    return contents;
}

// The rows whose function has a definition that does no work, as
// "case (row) has an empty body at file:line" lines.
static std::vector<std::string> functional_integrity_failures( const functional_integrity_definitions_t & definitions,
                                                               const std::vector<functional_integrity_row_t> & rows )
{
    std::vector<std::string> failures;
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        std::map<std::string, std::vector<std::string>>::const_iterator it = definitions.empty.find( rows[i].function );
        if ( it == definitions.empty.end() )
        {
            continue;
        }
        for ( size_t j = 0; j < it->second.size(); j++ )
        {
            failures.push_back( rows[i].test + " (" + rows[i].where + ") has an empty body at " + it->second[j] );
        }
    }
    return failures;
}

// The audit over in-memory sources: { file, text } pairs, plus a CMake text
// whose generated CONTENT strings are scanned as sources too.
static std::vector<std::string> functional_integrity_audit( const std::vector<std::pair<std::string, std::string>> & sources,
                                                            const std::string & cmake, std::vector<functional_integrity_row_t> * rows_out = NULL,
                                                            functional_integrity_definitions_t * definitions_out = NULL )
{
    functional_integrity_definitions_t definitions;
    std::vector<functional_integrity_row_t> rows;
    // First the directives: each file's macros, and the headers' macros,
    // which any file may invoke.
    std::vector<std::string> rests( sources.size() );
    std::vector<functional_integrity_macros_t> file_macros( sources.size() );
    functional_integrity_macros_t header_macros;
    std::vector<const functional_integrity_macro_t *> all_macros;
    std::vector<std::string> all_names;
    for ( size_t i = 0; i < sources.size(); i++ )
    {
        rests[i] = functional_integrity_split_directives( functional_integrity_without_comments( sources[i].second ), file_macros[i] );
        const std::string & file = sources[i].first;
        const bool header = ( file.size() > 2 && file.compare( file.size() - 2, 2, ".h" ) == 0 ) ||
                            ( file.size() > 4 && file.compare( file.size() - 4, 4, ".inc" ) == 0 );
        for ( functional_integrity_macros_t::const_iterator it = file_macros[i].begin(); it != file_macros[i].end(); ++it )
        {
            if ( header )
            {
                header_macros[it->first] = it->second;
            }
            all_macros.push_back( &it->second );
            all_names.push_back( it->first );
        }
    }
    // A generator writes definitions itself or invokes a generator that does.
    std::set<std::string> generators;
    for ( bool grew = true; grew; )
    {
        grew = false;
        for ( size_t m = 0; m < all_macros.size(); m++ )
        {
            if ( generators.count( all_names[m] ) != 0 )
            {
                continue;
            }
            bool generator = functional_integrity_writes_definitions( *all_macros[m] );
            for ( std::set<std::string>::const_iterator g = generators.begin(); !generator && g != generators.end(); ++g )
            {
                size_t at = 0;
                while ( !generator && ( at = all_macros[m]->body.find( *g, at ) ) != std::string::npos )
                {
                    const size_t end = at + g->size();
                    generator = ( at == 0 || !functional_integrity_word_char( all_macros[m]->body[at - 1] ) ) &&
                                ( end == all_macros[m]->body.size() || !functional_integrity_word_char( all_macros[m]->body[end] ) );
                    at = end;
                }
            }
            if ( generator )
            {
                generators.insert( all_names[m] );
                grew = true;
            }
        }
    }
    for ( size_t i = 0; i < sources.size(); i++ )
    {
        functional_integrity_scan_definitions( rests[i], sources[i].first, 0, definitions );
        functional_integrity_scan_generated( rests[i], sources[i].first, file_macros[i], header_macros, generators, definitions );
        functional_integrity_scan_rows( rests[i], sources[i].first, rows );
    }
    const std::vector<functional_integrity_generated_t> contents = functional_integrity_generated_contents( cmake );
    for ( size_t i = 0; i < contents.size(); i++ )
    {
        const std::string code = functional_integrity_without_comments( contents[i].content );
        functional_integrity_scan_definitions( code, "CMakeLists.txt", contents[i].line, definitions );
    }
    if ( rows_out != NULL )
    {
        *rows_out = rows;
    }
    if ( definitions_out != NULL )
    {
        *definitions_out = definitions;
    }
    return functional_integrity_failures( definitions, rows );
}

static std::vector<std::string> functional_integrity_one( const std::string & text, const std::string & row )
{
    std::vector<std::pair<std::string, std::string>> sources;
    sources.push_back( std::make_pair( std::string( "a.cpp" ), text ) );
    sources.push_back( std::make_pair( std::string( "registry.cpp" ), row ) );
    return functional_integrity_audit( sources, "" );
}

static std::vector<std::string> functional_integrity_manifest()
{
    std::vector<std::string> files;
#ifdef ROCKETNET_FUNCTIONAL_SOURCES
    const std::string globbed = ROCKETNET_FUNCTIONAL_SOURCES;
    size_t pos = 0;
    while ( pos <= globbed.size() )
    {
        const size_t sep = globbed.find( '|', pos );
        const std::string entry = globbed.substr( pos, sep == std::string::npos ? std::string::npos : sep - pos );
        if ( !entry.empty() )
        {
            files.push_back( entry );
        }
        if ( sep == std::string::npos )
        {
            break;
        }
        pos = sep + 1;
    }
#endif // #ifdef ROCKETNET_FUNCTIONAL_SOURCES
    return files;
}

static void test_functional_cases_have_no_empty_bodies()
{
    const std::string row = "{ \"test_windows_delivery\", windows_delivery, NULL },\n";
    rocketnet_unit_check( functional_integrity_one( "inline void windows_delivery() {}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void\twindows_delivery ( void ) { }", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "avoid windows_delivery() {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery(voidish) {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() {}", "{ \"test_\", windows_delivery, NULL }," ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery(void) {\n/* no checks */\n}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { check(delivered); }", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void unrelated_callback() {}", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() {}", "// " + row ).empty() );
    const std::string long_comment( 512 * 1024, 'x' );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() {/*" + long_comment + "*/}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "//" + long_comment + "\nvoid windows_delivery() {}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() {/*" + long_comment + "*/ check(delivered); }", row ).empty() );

    // The bodies that do no work: returns and casts-to-void, in any number.
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { return; }", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { return 0; }", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { (void) argc; }", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { ( void )argc; // nothing\n (void) argv; return; }", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { return check(delivered); }", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { return 01; }", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { return 0x1; }", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { (void) argc; check(delivered); }", row ).empty() );
    rocketnet_unit_check( functional_integrity_one( "void windows_delivery() { returned(); }", row ).empty() );

    // Under any #if branch: the preprocessor is not evaluated.
    rocketnet_unit_check( functional_integrity_one( "#if 0\nvoid windows_delivery() { check(delivered); }\n#else\nvoid windows_delivery() {}\n#endif\n", row ).size() == 1 );

    // A "//" inside a string literal is not a comment, so it hides nothing.
    rocketnet_unit_check( functional_integrity_one( "const char * url = \"http://host/\"; void windows_delivery() {}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "const char c = '\"'; void windows_delivery() {}", row ).size() == 1 );
    rocketnet_unit_check( functional_integrity_one( "const int n = 1'000; void windows_delivery() {}", row ).size() == 1 );

    // A case family a macro writes is read at its invocation, through a macro
    // that invokes the writer, with # and ## as the preprocessor does them.
    {
        const std::string family =
            "#define CELL( name, cell ) \\\n"
            "void rocketnet_test_##name##_##cell() \\\n"
            "{ \\\n"
            "    run( #cell ); \\\n"
            "}\n"
            "#define CELLS( name ) \\\n"
            "    CELL( name, portable ) \\\n"
            "    CELL( name, gso )\n"
            "#define EMPTY_CELL( name, cell ) void rocketnet_test_##name##_##cell() { (void) cell; }\n"
            "\n"
            "CELLS( init_term )\n"
            "EMPTY_CELL( init_term, xdp )\n";
        const std::string rows =
            "{ \"test_portable\", rocketnet_test_init_term_portable, NULL },\n"
            "{ \"test_gso\", rocketnet_test_init_term_gso, NULL },\n"
            "{ \"test_xdp\", rocketnet_test_init_term_xdp, NULL },\n";
        std::vector<std::pair<std::string, std::string>> sources;
        sources.push_back( std::make_pair( std::string( "cells.cpp" ), family ) );
        sources.push_back( std::make_pair( std::string( "registry.cpp" ), rows ) );
        std::vector<functional_integrity_row_t> scanned_rows;
        functional_integrity_definitions_t definitions;
        const std::vector<std::string> failures = functional_integrity_audit( sources, "", &scanned_rows, &definitions );
        rocketnet_unit_check( scanned_rows.size() == 3 );
        rocketnet_unit_check( definitions.defined.count( "rocketnet_test_init_term_portable" ) == 1 );
        rocketnet_unit_check( definitions.defined.count( "rocketnet_test_init_term_gso" ) == 1 );
        rocketnet_unit_check( failures.size() == 1 && failures[0] == "test_xdp (registry.cpp:3) has an empty body at cells.cpp:12" );
        // The macro text itself defines nothing until it is invoked.
        sources[0].second = family.substr( 0, family.find( "\nCELLS( init_term )" ) );
        rocketnet_unit_check( functional_integrity_audit( sources, "", NULL, &definitions ).empty() && definitions.defined.empty() );
    }

    // The row and the empty body in different files, the row in any table.
    {
        std::vector<std::pair<std::string, std::string>> sources;
        sources.push_back( std::make_pair( std::string( "planted.cpp" ), std::string( "\n\nstatic void planted_empty() { /* nothing */ }\n" ) ) );
        sources.push_back( std::make_pair( std::string( "sibling.cpp" ),
                                           std::string( "static const case_t sibling[] = {\n    { \"test_planted_empty\", planted_empty, NULL },\n};\n" ) ) );
        const std::vector<std::string> failures = functional_integrity_audit( sources, "" );
        rocketnet_unit_check( failures.size() == 1 );
        rocketnet_unit_check( failures.size() == 1 &&
                              failures[0] == "test_planted_empty (sibling.cpp:2) has an empty body at planted.cpp:3" );
    }

    // The CMake text around the CONTENT string is CMake, not C++: a glob such
    // as tests/functional/*.cpp before the generated body must not hide it.
    {
        std::vector<std::pair<std::string, std::string>> sources;
        sources.push_back( std::make_pair( std::string( "registry.cpp" ), row ) );
        const std::string planted_cmake =
            "file(GLOB planted \"${CMAKE_CURRENT_SOURCE_DIR}/tests/functional/*.cpp\")\n"
            "file(GENERATE OUTPUT \"${SHIM}\" CONTENT\n\"#pragma once\\n#include \\\"rocketnet.h\\\"\\nvoid windows_delivery() {}\\n\")\n";
        const std::vector<std::string> failures = functional_integrity_audit( sources, planted_cmake );
        rocketnet_unit_check( failures.size() == 1 && failures[0] == "test_windows_delivery (registry.cpp:1) has an empty body at CMakeLists.txt:2" );
        rocketnet_unit_check( functional_integrity_audit( sources, "file(GLOB planted \"tests/*.cpp\")\nvoid windows_delivery() {}\n" ).empty() );
    }

    // The repository. A missing or renamed input must not silently turn this
    // into an empty scan: the manifest is CMake's glob of tests/functional,
    // and it must hold the registry.
    const std::vector<std::string> files = functional_integrity_manifest();
    rocketnet_unit_check( !files.empty() );
    std::vector<std::pair<std::string, std::string>> sources;
    bool registry_listed = false;
    for ( size_t i = 0; i < files.size(); i++ )
    {
        sources.push_back( std::make_pair( files[i], functional_integrity_read( files[i] ) ) );
        if ( files[i] == "tests/functional/rocketnet_functional_tests.cpp" )
        {
            registry_listed = true;
            rocketnet_unit_check( sources.back().second.find( "rocketnet_functional_tests[]" ) != std::string::npos );
        }
    }
    rocketnet_unit_check( registry_listed );
    const std::string cmake = functional_integrity_read( "CMakeLists.txt" );
    rocketnet_unit_check( cmake.find( "ROCKETNET_FUNCTIONAL_PRODUCTION_SHIM_HEADER" ) != std::string::npos );
    // The scan must reach the production shim's generated text itself.
    const std::vector<functional_integrity_generated_t> contents = functional_integrity_generated_contents( cmake );
    bool shim_scanned = false;
    for ( size_t i = 0; i < contents.size(); i++ )
    {
        if ( contents[i].content.find( "void rocketnet_functional_production_server_config(" ) != std::string::npos )
        {
            shim_scanned = true;
        }
    }
    rocketnet_unit_check( shim_scanned );
    std::vector<functional_integrity_row_t> rows;
    functional_integrity_definitions_t definitions;
    const std::vector<std::string> failures = functional_integrity_audit( sources, cmake, &rows, &definitions );
    // Every registered function's definition is one the scan reads: a row whose
    // function it never found would be a case it could not judge.
    rocketnet_unit_check( !rows.empty() );
    int unread = 0;
    for ( size_t i = 0; i < rows.size(); i++ )
    {
        if ( definitions.defined.count( rows[i].function ) == 0 )
        {
            printf( "functional case %s (%s): no definition of %s found in the scanned sources\n", rows[i].test.c_str(), rows[i].where.c_str(),
                    rows[i].function.c_str() );
            unread++;
        }
    }
    rocketnet_unit_check( unread == 0 );
    for ( size_t i = 0; i < failures.size(); i++ )
    {
        printf( "functional case %s\n", failures[i].c_str() );
    }
    rocketnet_unit_check( failures.empty() );
}

REGISTER_UNIT_TEST( test_functional_cases_have_no_empty_bodies );
