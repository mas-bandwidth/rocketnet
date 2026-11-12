/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_TLA_TRACE_RECORD_H
#define ROCKETNET_TESTS_TLA_TRACE_RECORD_H

// The record writer of the TLA+ trace check (proofs/tla/README-TraceCheck.md),
// shared by every layer's walk: TLA+ values as text, one record of a
// model's variables, the walk's seeded generator, and the writer of the
// trace module and its cfg. A layer's walk is a function
// int walk( uint64_t seed, int steps, const char * out ) in its own file
// tests/tla_trace_<layer>.cpp, registered with TLA_TRACE_LAYER below.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "portable_crt.h"

inline std::string tla_int( int64_t value )
{
    char text[32];
    snprintf( text, sizeof( text ), "%" PRId64, value );
    return text;
}

inline std::string tla_bool( bool value )
{
    return value ? "TRUE" : "FALSE";
}

// a function over 0..n-1 (or base..base+n-1): k1 :> v1 @@ k2 :> v2 ...
inline std::string tla_function( const std::vector<std::string> & keys, const std::vector<std::string> & values )
{
    std::string text = "(";
    for ( size_t i = 0; i < keys.size(); i++ )
    {
        if ( i > 0 )
        {
            text += " @@ ";
        }
        text += keys[i] + " :> " + values[i];
    }
    text += ")";
    return text;
}

struct tla_record_t
{
    std::vector<std::string> fields;
    std::vector<std::string> values;

    void add( const char * field, const std::string & value )
    {
        fields.push_back( field );
        values.push_back( value );
    }

    std::string text() const
    {
        std::string out = "[";
        for ( size_t i = 0; i < fields.size(); i++ )
        {
            if ( i > 0 )
            {
                out += ", ";
            }
            out += fields[i] + " |-> " + values[i];
        }
        out += "]";
        return out;
    }
};

// the deterministic generator of the walk: splitmix64
struct tla_random_t
{
    uint64_t state;

    uint64_t next()
    {
        uint64_t z = ( state += 0x9E3779B97F4A7C15ULL );
        z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ULL;
        z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBULL;
        return z ^ ( z >> 31 );
    }

    int below( int n )
    {
        return (int) ( next() % (uint64_t) n );
    }
};

// writes MODULE <name> with the constants, definitions and trace, and its cfg;
// properties, when not NULL, names the temporal properties the cfg checks too
inline bool tla_write_trace( const char * dir, const std::string & name, const char * extends, const std::string & definitions,
                             const std::string & constants, const std::vector<tla_record_t> & records, const char * properties = NULL )
{
    const std::string module_path = std::string( dir ) + "/" + name + ".tla";
    const std::string cfg_path = std::string( dir ) + "/" + name + ".cfg";

    FILE * module = rocketnet_test_fopen( module_path.c_str(), "w" );
    if ( !module )
    {
        fprintf( stderr, "tla_trace: could not write %s\n", module_path.c_str() );
        return false;
    }

    std::string header = "---- MODULE " + name + " ----";
    fprintf( module, "%s\n", header.c_str() );
    fprintf( module, "\\* written by tests/tla_trace.cpp: %d records of the code's state, one per step\n", (int) records.size() );
    fprintf( module, "EXTENDS %s, TLC\n\n", extends );
    fprintf( module, "%s\n", definitions.c_str() );
    fprintf( module, "Log == <<\n" );
    for ( size_t i = 0; i < records.size(); i++ )
    {
        fprintf( module, "    %s%s\n", records[i].text().c_str(), i + 1 < records.size() ? "," : "" );
    }
    fprintf( module, ">>\n\n====\n" );
    fclose( module );

    FILE * cfg = rocketnet_test_fopen( cfg_path.c_str(), "w" );
    if ( !cfg )
    {
        fprintf( stderr, "tla_trace: could not write %s\n", cfg_path.c_str() );
        return false;
    }

    fprintf( cfg, "\\* written by tests/tla_trace.cpp: the trace must be a behaviour of the spec\n" );
    fprintf( cfg, "SPECIFICATION TraceSpec\n\nCONSTANTS\n%s    TraceLog <- Log\n\nINVARIANT TraceConsistent\n", constants.c_str() );
    if ( properties )
    {
        fprintf( cfg, "\nPROPERTIES %s\n", properties );
    }
    fclose( cfg );

    printf( "TRACE %s records=%d\n", module_path.c_str(), (int) records.size() );
    return true;
}

// the layer table: each layer's file registers its walk at namespace scope
// with TLA_TRACE_LAYER( name, walk ); tests/tla_trace.cpp runs the one
// --layer names, and --list prints them. Every tests/tla_trace_*.cpp is
// compiled into tla_trace and tla_trace_plant (CMakeLists.txt). The table is
// a fixed array: registration past TLA_TRACE_MAX_LAYERS is refused and
// tla_trace then refuses to run, so a layer is never silently left out.

#define TLA_TRACE_MAX_LAYERS 64

typedef int ( *tla_trace_walk_t )( uint64_t seed, int steps, const char * out );

struct tla_trace_layer_t
{
    const char * name;
    tla_trace_walk_t walk;
};

struct tla_trace_table_t
{
    tla_trace_layer_t layer[TLA_TRACE_MAX_LAYERS];
    int count;
    int refused;
};

inline tla_trace_table_t & tla_trace_layers()
{
    static tla_trace_table_t table = {};
    return table;
}

struct tla_trace_registrar_t
{
    tla_trace_registrar_t( const char * name, tla_trace_walk_t walk )
    {
        tla_trace_table_t & table = tla_trace_layers();
        if ( table.count >= TLA_TRACE_MAX_LAYERS )
        {
            table.refused++;
            return;
        }
        table.layer[table.count].name = name;
        table.layer[table.count].walk = walk;
        table.count++;
    }
};

#define TLA_TRACE_LAYER( name, walk ) static tla_trace_registrar_t tla_trace_registrar_##walk( name, walk )

#endif // ROCKETNET_TESTS_TLA_TRACE_RECORD_H
