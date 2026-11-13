/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_TLA_TRACE_RECORD_H
#define ROCKETNET_TESTS_TLA_TRACE_RECORD_H

// The record writer of the TLA+ trace check (proofs/tla/README-TraceCheck.md),
// shared by every layer's walk: TLA+ values as text, one record per step,
// the walk's seeded generator, the walk's report and the writer of the trace
// module and its cfg. A layer's walk is a function
// int walk( uint64_t seed, int steps, const char * out ) in its own file
// tests/tla_trace_<layer>.cpp, registered with TLA_TRACE_LAYER below.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>

#include "portable_crt.h"

// a value the walk could not write faithfully: the writer then refuses to
// write the trace, so the walk fails red
inline int & tla_value_errors()
{
    static int errors = 0;
    return errors;
}

// TLC's integers are 32-bit: a value outside -2^31+1 .. 2^31-1 is an error,
// never a silent wrap. A 64-bit counter goes through tla_int_split.
inline std::string tla_int( int64_t value )
{
    if ( value < -2147483647LL || value > 2147483647LL )
    {
        fprintf( stderr, "tla_trace: %" PRId64 " is outside TLC's 32-bit integers: write it with tla_int_split\n", value );
        tla_value_errors()++;
    }
    char text[32];
    snprintf( text, sizeof( text ), "%" PRId64, value );
    return text;
}

// a 64-bit value as four 16-bit digits, most significant first: <<d3, d2, d1, d0>>;
// the spec compares it as a tuple, or rebuilds it where it does arithmetic
inline std::string tla_int_split( uint64_t value )
{
    char text[64];
    snprintf( text, sizeof( text ), "<<%u, %u, %u, %u>>", (unsigned) ( ( value >> 48 ) & 0xFFFF ), (unsigned) ( ( value >> 32 ) & 0xFFFF ),
              (unsigned) ( ( value >> 16 ) & 0xFFFF ), (unsigned) ( value & 0xFFFF ) );
    return text;
}

inline std::string tla_bool( bool value )
{
    return value ? "TRUE" : "FALSE";
}

// a string: quotes and backslashes escaped
inline std::string tla_string( const std::string & value )
{
    std::string text = "\"";
    for ( size_t i = 0; i < value.size(); i++ )
    {
        if ( value[i] == '"' || value[i] == '\\' )
        {
            text += '\\';
        }
        text += value[i];
    }
    text += "\"";
    return text;
}

// a function over keys: k1 :> v1 @@ k2 :> v2 ..., and <<>> (the function on
// the empty set) when there are no keys
inline std::string tla_function( const std::vector<std::string> & keys, const std::vector<std::string> & values )
{
    // a key twice would vanish in @@ (the left one wins): an error, never a silent merge
    if ( keys.size() != values.size() )
    {
        fprintf( stderr, "tla_trace: tla_function with %d keys and %d values\n", (int) keys.size(), (int) values.size() );
        tla_value_errors()++;
        return "<<>>";
    }
    for ( size_t i = 0; i < keys.size(); i++ )
    {
        for ( size_t j = 0; j < i; j++ )
        {
            if ( keys[i] == keys[j] )
            {
                fprintf( stderr, "tla_trace: tla_function has the key %s twice\n", keys[i].c_str() );
                tla_value_errors()++;
            }
        }
    }
    if ( keys.empty() )
    {
        return "<<>>";
    }
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

// a sequence: <<v1, v2, ...>>
inline std::string tla_sequence( const std::vector<std::string> & values )
{
    std::string text = "<<";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        text += ( i > 0 ? ", " : "" ) + values[i];
    }
    return text + ">>";
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

// one step of a layer's trace: the operation the walk called (op, a
// lower-case identifier: letters, digits and '_'),
// the walk's arguments to it (args, a sequence: the model action's
// parameters in order, <<>> when it has none), the model action the
// operation's own result says it took (act), and every model variable after
// it (state). TraceStep allows only that action with those arguments, and
// only when the operation can be that action (the Trace module's OpActs),
// so a call that did one thing but landed in a state another action allows
// is rejected. act comes from the call's return value or out-parameters,
// never from comparing the state before and after: that would let the
// state pick the action, which is the hole this field closes. The first
// record's op and act are "init" and its args <<>>.
inline tla_record_t tla_step( const char * op, const char * act, const std::string & args, const tla_record_t & state )
{
    tla_record_t step;
    step.add( "op", tla_string( op ) );
    step.add( "act", tla_string( act ) );
    step.add( "args", args );
    step.add( "state", state.text() );
    return step;
}

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

// what the walk tells tools/tla_trace_check.py, besides the TRACE line:
//   ACTS <act>=<n> ...   how often each model action was taken
//   PLANT-FIRED <n>      how often the planted effect happened (the clean
//                        code's count must be 0)
//   PLANT-AT <i>,...     the 1-based records at which it happened, or -
// The checker requires TLC's rejection of a planted trace at the first
// record where it differs from the clean trace, and that record among these,
// so the plant is shown to be the cause.
struct tla_report_t
{
    std::map<std::string, int> acts;
    std::vector<int> plant_at;

    void act( const char * name )
    {
        acts[name]++;
    }

    void planted( int record )
    {
        plant_at.push_back( record );
    }

    void print() const
    {
        printf( "ACTS" );
        for ( std::map<std::string, int>::const_iterator i = acts.begin(); i != acts.end(); ++i )
        {
            printf( " %s=%d", i->first.c_str(), i->second );
        }
        printf( "\nPLANT-FIRED %d\nPLANT-AT ", (int) plant_at.size() );
        if ( plant_at.empty() )
        {
            printf( "-" );
        }
        for ( size_t i = 0; i < plant_at.size(); i++ )
        {
            printf( "%s%d", i > 0 ? "," : "", plant_at[i] );
        }
        printf( "\n" );
    }
};

// writes MODULE <stem>Seed<seed> (<stem>PlantSeed<seed> in the plant build:
// the writer names it, so a walk never tests ROCKETNET_TLA_PLANT) with the
// constants, definitions and trace, and its cfg; properties, when not NULL,
// names the temporal properties the cfg checks too. Refuses a name tlacheck
// would refuse (MC followed by letters and digits) and a trace with a value
// it could not write.
inline bool tla_write_trace( const char * dir, const char * stem, uint64_t seed, const char * extends, const std::string & definitions,
                             const std::string & constants, const std::vector<tla_record_t> & records, const char * properties = NULL )
{
    char seed_text[32];
    snprintf( seed_text, sizeof( seed_text ), "%" PRIu64, seed );
#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = std::string( stem ) + "PlantSeed" + seed_text;
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = std::string( stem ) + "Seed" + seed_text;
#endif // #if defined( ROCKETNET_TLA_PLANT )
    bool name_ok = name.size() > 2 && name.compare( 0, 2, "MC" ) == 0;
    for ( size_t i = 2; i < name.size(); i++ )
    {
        const char c = name[i];
        if ( !( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) ) )
        {
            name_ok = false;
        }
    }
    if ( !name_ok )
    {
        fprintf( stderr, "tla_trace: module name %s: tlacheck takes MC followed by letters and digits only\n", name.c_str() );
        return false;
    }
    if ( tla_value_errors() > 0 )
    {
        fprintf( stderr, "tla_trace: %d values could not be written: no trace\n", tla_value_errors() );
        return false;
    }

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
    fprintf( module, "\\* written by tests/tla_trace.cpp: %d records of the code's steps\n", (int) records.size() );
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
// --layer names, and --list prints them. A name is lower-case letters,
// digits and '-' (tlacheck's group names). The table is a fixed array: a
// registration past TLA_TRACE_MAX_LAYERS, a second registration of one name
// or a bad name is refused and recorded, and tla_trace then prints each
// refusal (REFUSED <name> <reason>) and refuses to run, so a layer is never
// silently left out or hidden behind another of its name. CMake refuses to
// configure with more tests/tla_trace_*.cpp files than this (one layer a
// file), so a full table is red where the file is added. About 132 layers
// are planned (every model bound to its code): 256 leaves room.

#define TLA_TRACE_MAX_LAYERS 256

typedef int ( *tla_trace_walk_t )( uint64_t seed, int steps, const char * out );

struct tla_trace_layer_t
{
    const char * name;
    tla_trace_walk_t walk;
};

struct tla_trace_refusal_t
{
    const char * name;
    const char * reason;
};

struct tla_trace_table_t
{
    tla_trace_layer_t layer[TLA_TRACE_MAX_LAYERS];
    int count;
    tla_trace_refusal_t refusal[TLA_TRACE_MAX_LAYERS];
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
        const char * reason = NULL;
        if ( name[0] == '\0' )
        {
            reason = "bad-name";
        }
        for ( const char * c = name; *c; c++ )
        {
            if ( !( ( *c >= 'a' && *c <= 'z' ) || ( *c >= '0' && *c <= '9' ) || *c == '-' ) )
            {
                reason = "bad-name";
            }
        }
        for ( int i = 0; i < table.count; i++ )
        {
            if ( std::string( table.layer[i].name ) == name )
            {
                reason = "registered-twice";
            }
        }
        if ( !reason && table.count >= TLA_TRACE_MAX_LAYERS )
        {
            reason = "table-full";
        }
        if ( reason )
        {
            if ( table.refused < TLA_TRACE_MAX_LAYERS )
            {
                table.refusal[table.refused].name = name;
                table.refusal[table.refused].reason = reason;
            }
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
