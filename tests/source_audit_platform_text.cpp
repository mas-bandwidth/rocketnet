/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The platform and guide text audits: the spin lock's acquire loop pauses
// each spin and yields after a bounded number, its MSVC unlock is a plain
// exchange, the relaxed 64-bit load is not a locked read-modify-write on
// 64-bit MSVC, and the shipped guides state the config_bytes field-boundary
// rule. Pause, yield and instruction choice are timing and code-generation
// behaviour a portable unit case cannot observe, so the shape is read from
// the source. They read repository files through ROCKETNET_SOURCE_DIR, so they
// are source-policy checks and run in the source-audit binary, not the unit
// suite (which stays inside its working directory).

#include <stdio.h>
#include <string.h>
#include <string>
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "source_audit_text.h"

#define check rocketnet_unit_check

static bool platform_text_read( const char * relative_path, std::string & contents )
{
    const std::string path = std::string( ROCKETNET_SOURCE_DIR ) + "/" + relative_path;
    FILE * file = rocketnet_test_fopen( path.c_str(), "rb" );
    if ( !file )
    {
        return false;
    }

    contents.clear();
    char buffer[4096];
    size_t got;
    while ( ( got = fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
    {
        contents.append( buffer, got );
    }
    fclose( file );
    return !contents.empty();
}

// The acquire loop of the spin lock that guards the sent-history ring: a CPU
// pause on each spin and a thread yield after ROCKETNET_PLATFORM_SPIN_YIELD_AFTER
// of them, so a holder that was preempted gets the CPU back. The MSVC unlock
// releases the word; it is not another compare-exchange. (The acquire and
// release themselves are the unit case test_platform_spin_lock_acquire_release.)

static void test_platform_spin_lock_backoff_source()
{
    std::string header;
    check( platform_text_read( "sdk/rocketnet_platform.h", header ) );
    // the calls commented out passed when comments were read (VERIFY-3 W16)
    header = source_audit_code_only( header );

    const std::string lock_name = "inline void rocketnet_platform_spin_lock_u64";
    const std::string unlock_name = "inline void rocketnet_platform_spin_unlock_u64";
    const size_t lock_at = header.find( lock_name );
    const size_t unlock_at = header.find( unlock_name, lock_at );
    check( lock_at != std::string::npos );
    check( unlock_at != std::string::npos );

    const std::string acquire = header.substr( lock_at, unlock_at - lock_at );
    check( acquire.find( "ROCKETNET_PLATFORM_SPIN_YIELD_AFTER" ) != std::string::npos );
    check( acquire.find( "_mm_pause" ) != std::string::npos );
    check( acquire.find( "__builtin_ia32_pause" ) != std::string::npos );
    check( acquire.find( "sched_yield" ) != std::string::npos );
    check( acquire.find( "SwitchToThread" ) != std::string::npos );

    const std::string release = header.substr( unlock_at );
    check( release.find( "_InterlockedExchange64" ) != std::string::npos );
    check( release.find( "_InterlockedCompareExchange64" ) == std::string::npos );
}

REGISTER_UNIT_TEST( test_platform_spin_lock_backoff_source );

// A relaxed 64-bit load must not be a locked read-modify-write. On 64-bit MSVC
// the word is readable with the volatile load intrinsic, so the load path and
// the receive-reset read take the guarded branch instead of the compare-exchange
// fallback the 32-bit targets still need.

static void platform_text_check_relaxed_u64_load( const std::string & source, const char * start_name, const char * end_name )
{
    // code only: a name in a comment is not the operation the branch takes
    const std::string text = source_audit_code_only( source );
    const size_t start = text.find( start_name );
    const size_t end = text.find( end_name, start );
    check( start != std::string::npos );
    check( end != std::string::npos && end > start );
    if ( start == std::string::npos || end == std::string::npos || end <= start )
    {
        return;
    }

    // the 64-bit MSVC branch is the #if of the guard and ends at its #else;
    // the fallback runs from the #else to the #endif, with no directive
    // between, and nothing outside them loads the word
    const std::string body = text.substr( start, end - start );
    const std::string guard = "#if defined( _M_X64 ) || defined( _M_ARM64 )";
    const size_t if_at = body.find( guard );
    check( if_at != std::string::npos );
    if ( if_at == std::string::npos )
    {
        return;
    }
    const size_t taken_at = if_at + guard.size();
    const size_t else_at = body.find( "#else", taken_at );
    const size_t endif_at = body.find( "#endif", taken_at );
    check( else_at != std::string::npos && endif_at != std::string::npos && else_at < endif_at );
    if ( else_at == std::string::npos || endif_at == std::string::npos || else_at >= endif_at )
    {
        return;
    }
    check( body.find( "#", taken_at ) == else_at );
    check( body.find( "#", else_at + 1 ) == endif_at );

    // the taken branch returns the volatile load and holds no compare-exchange
    // (either spelling: InterlockedCompareExchange64 or the underscore intrinsic)
    const std::string taken = body.substr( taken_at, else_at - taken_at );
    const size_t returned = taken.find( "return" );
    const size_t plain = taken.find( "__iso_volatile_load64" );
    check( returned != std::string::npos && plain != std::string::npos && returned < plain );
    check( returned == std::string::npos || taken.find( ";", returned ) > plain );
    check( taken.find( "InterlockedCompareExchange64" ) == std::string::npos );

    const std::string fallback = body.substr( else_at, endif_at - else_at );
    check( fallback.find( "InterlockedCompareExchange64" ) != std::string::npos );

    const std::string outside = body.substr( 0, if_at ) + body.substr( endif_at );
    check( outside.find( "InterlockedCompareExchange64" ) == std::string::npos );
    check( outside.find( "__iso_volatile_load64" ) == std::string::npos );
}

static void test_platform_relaxed_u64_load_is_not_locked_on_64_bit_msvc()
{
    std::string header;
    check( platform_text_read( "sdk/rocketnet_platform.h", header ) );
    platform_text_check_relaxed_u64_load( header,
                                          "inline uint64_t rocketnet_platform_atomic_load_u64",
                                          "inline void rocketnet_platform_atomic_store_u64" );

    std::string source;
    check( platform_text_read( "sdk/rocketnet_platform.cpp", source ) );
    platform_text_check_relaxed_u64_load( source,
                                          "uint64_t rocketnet_platform_receive_resets()",
                                          "int rocketnet_platform_socket_receive_buffer_bytes" );
}

REGISTER_UNIT_TEST( test_platform_relaxed_u64_load_is_not_locked_on_64_bit_msvc );

// The shipped guides must state the rule the code enforces: a smaller
// config_bytes is accepted only when it ends where one of the struct's fields
// ends, and rocketnet_connect_token_config_t follows the public config rules.

static std::string platform_text_doc_section( const std::string & text, const char * heading, const char * next )
{
    const size_t start = text.find( heading );
    if ( start == std::string::npos )
    {
        return std::string();
    }

    const size_t end = text.find( next, start );
    if ( end == std::string::npos )
    {
        return std::string();
    }

    return text.substr( start, end - start );
}

static void test_docs_abi_evolution_records_the_field_boundary()
{
    std::string abi;
    check( platform_text_read( "docs/ABI-EVOLUTION.md", abi ) );
    std::string usage;
    check( platform_text_read( "docs/USAGE.md", usage ) );

    const std::string rule = platform_text_doc_section( abi, "## 1. The Rule", "## 2." );
    const std::string implementation = platform_text_doc_section( abi, "## 3. The Implementation", "## 4." );
    const std::string table = platform_text_doc_section( abi, "## 4. The Other Public Structs", "## 5." );

    check( rule.find( "rocketnet_connect_token_config_t" ) != std::string::npos );
    check( rule.find( "ends inside a field" ) != std::string::npos );
    check( implementation.find( "ends inside a field" ) != std::string::npos );
    check( table.find( "| `rocketnet_connect_token_config_t` | in | yes, `config_bytes` |" ) != std::string::npos );
    check( abi.find( "accept every size of at least" ) == std::string::npos );
    check( usage.find( "ends inside a field" ) != std::string::npos );
}

REGISTER_UNIT_TEST( test_docs_abi_evolution_records_the_field_boundary );
