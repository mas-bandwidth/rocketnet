/*
    RocketNet benchmark-only AF_XDP packet blaster.

    This translation unit is deliberately not installed and exports no SDK
    symbol. CMake compiles it as C++ because it composes RocketNet's private
    protocol machine with the existing C++ AF_XDP compatibility layer.
*/

#include "xdp_packet_blaster.h"

#include "rocketnet_address.h"
#include "rocketnet_allocator.h"
#include "rocketnet_base64.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_internal.h"
#include "rocketnet_message.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_wire.h"
#include "rocketnet_xdp_compat.h"
#include "rocketnet_xdp_frame.h"
#include "rocketnet_xdp_socket_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sodium.h>
#include <string>
#include <thread>
#include <vector>

#if defined( __linux__ )
#include <arpa/inet.h>
#include <sched.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#endif

#define RN_BLASTER_TOKEN_BYTES 768u
#define RN_BLASTER_MAX_FILE_BYTES ( 2u * 1024u * 1024u )
#define RN_BLASTER_UMEM_FRAMES 4096u
#define RN_BLASTER_TX_RING_SIZE 2048u
#define RN_BLASTER_RX_RING_SIZE 2048u
#define RN_BLASTER_FILL_RING_SIZE 4096u
#define RN_BLASTER_COMPLETION_RING_SIZE 2048u
#define RN_BLASTER_FRAME_BYTES 4096u
#define RN_BLASTER_RX_FRAMES ( RN_BLASTER_UMEM_FRAMES / 2u )
#define RN_BLASTER_PAYLOAD_MAGIC UINT32_C( 0x42504e52 ) /* RNPB */
#define RN_BLASTER_CONNECT_TIMEOUT_SECONDS 30u

static_assert( RN_BLASTER_FRAME_BYTES == ROCKETNET_XDP_FRAME_SIZE, "the benchmark preserves RocketNet's 4096-byte XDP frame" );
static_assert( RN_BLASTER_MAX_PAYLOAD_BYTES == ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES,
               "the largest payload is the largest message sent as one unreliable record, never promoted" );
static_assert( RN_BLASTER_MAX_PAYLOAD_BYTES + 4u <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES,
               "one record of the largest body fits one packet payload" );
static_assert( 37u + RN_BLASTER_MAX_PAYLOAD_BYTES <= ROCKETNET_SEND_BUDGET, "the largest packet stays inside the send budget" );
static_assert( RN_BLASTER_FILL_RING_SIZE >= 2u * RN_BLASTER_RX_FRAMES,
               "the fill ring retains a slot past the kernel's cached batch" );

struct rn_blaster_options_t
{
    struct rn_blaster_plan_t plan;
    std::string interface_name;
    std::string source_text;
    std::string server_text;
    std::string source_mac_text;
    std::string next_hop_mac_text;
    std::string token_file;
    std::string manifest_file;
    std::string server_transport_profile;
    std::string packet_filter_text;
    std::string owned_confirmation;
    std::string xdp_mode;
    std::string cpus_text;
    std::vector<uint32_t> cpus;
    bool execute;
    bool self_test;
};

struct rn_blaster_token_t
{
    std::array<uint8_t, RN_BLASTER_TOKEN_BYTES> bytes;
};

struct rn_blaster_string_wiper_t
{
    std::string * value;
    ~rn_blaster_string_wiper_t()
    {
        if ( value && !value->empty() )
        {
            sodium_memzero( &( *value )[0], value->size() );
        }
    }
};

static bool rn_blaster_parse_u32( const char * text_value, uint32_t low, uint32_t high, uint32_t * value )
{
    if ( !text_value || !*text_value || !value )
    {
        return false;
    }
    char * end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul( text_value, &end, 10 );
    if ( errno != 0 || !end || *end != '\0' || parsed < low || parsed > high )
    {
        return false;
    }
    *value = (uint32_t) parsed;
    return true;
}

static bool rn_blaster_parse_u64( const char * text_value, uint64_t * value )
{
    if ( !text_value || !*text_value || !value )
    {
        return false;
    }
    char * end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull( text_value, &end, 0 );
    if ( errno != 0 || !end || *end != '\0' )
    {
        return false;
    }
    *value = (uint64_t) parsed;
    return true;
}

static bool rn_blaster_parse_filter( const std::string & text_value, uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] )
{
    if ( text_value.size() != 8u )
    {
        return false;
    }
    for ( size_t i = 0; i < ROCKETNET_FILTER_VALUE_BYTES; ++i )
    {
        const char pair[3] = { text_value[2u * i], text_value[2u * i + 1u], '\0' };
        char * end = NULL;
        errno = 0;
        const unsigned long byte = strtoul( pair, &end, 16 );
        if ( errno != 0 || !end || *end != '\0' || byte > 255u )
        {
            return false;
        }
        filter[i] = (uint8_t) byte;
    }
    return true;
}

static bool rn_blaster_mac_unicast( const uint8_t mac[6] )
{
    bool nonzero = false;
    for ( int i = 0; i < 6; ++i )
    {
        nonzero = nonzero || mac[i] != 0;
    }
    return nonzero && ( mac[0] & 1u ) == 0;
}

static bool rn_blaster_read_file( const std::string & path, std::string * contents )
{
    if ( path.empty() || !contents )
    {
        return false;
    }
    FILE * file = fopen( path.c_str(), "rb" );
    if ( !file )
    {
        return false;
    }
    std::string data;
    rn_blaster_string_wiper_t data_wiper = { &data };
    (void) data_wiper;
    std::array<char, 4096> block;
    while ( true )
    {
        const size_t count = fread( block.data(), 1, block.size(), file );
        if ( count > 0 )
        {
            if ( data.size() > RN_BLASTER_MAX_FILE_BYTES - count )
            {
                fclose( file );
                return false;
            }
            data.append( block.data(), count );
        }
        if ( count < block.size() )
        {
            break;
        }
    }
    const int close_result = fclose( file );
    if ( close_result != 0 || data.empty() )
    {
        return false;
    }
    *contents = data;
    return true;
}

static bool rn_blaster_json_string( const std::string & object, const char * key, char * output, size_t output_bytes )
{
    if ( !key || !output || output_bytes == 0 )
    {
        return false;
    }
    const std::string needle = std::string( "\"" ) + key + "\"";
    size_t at = object.find( needle );
    if ( at == std::string::npos )
    {
        return false;
    }
    at = object.find( ':', at + needle.size() );
    if ( at == std::string::npos )
    {
        return false;
    }
    at = object.find( '"', at + 1u );
    if ( at == std::string::npos )
    {
        return false;
    }
    const size_t end = object.find( '"', at + 1u );
    if ( end == std::string::npos || end == at + 1u || end - at >= output_bytes )
    {
        return false;
    }
    for ( size_t i = at + 1u; i < end; ++i )
    {
        if ( object[i] == '\\' || (unsigned char) object[i] < 0x20u )
        {
            return false;
        }
    }
    memcpy( output, object.data() + at + 1u, end - at - 1u );
    output[end - at - 1u] = '\0';
    return true;
}

static bool rn_blaster_json_u32( const std::string & object, const char * key, uint32_t * output )
{
    const std::string needle = std::string( "\"" ) + key + "\"";
    size_t at = object.find( needle );
    if ( at == std::string::npos )
    {
        return false;
    }
    at = object.find( ':', at + needle.size() );
    if ( at == std::string::npos )
    {
        return false;
    }
    ++at;
    while ( at < object.size() && ( object[at] == ' ' || object[at] == '\t' || object[at] == '\n' || object[at] == '\r' ) )
    {
        ++at;
    }
    const size_t begin = at;
    while ( at < object.size() && object[at] >= '0' && object[at] <= '9' )
    {
        ++at;
    }
    if ( begin == at )
    {
        return false;
    }
    return rn_blaster_parse_u32( object.substr( begin, at - begin ).c_str(), 0, UINT32_MAX, output );
}

static bool rn_blaster_case_operation( const char * text_value, enum rn_blaster_case_operation_t * operation )
{
    struct entry_t
    {
        const char * name;
        enum rn_blaster_case_operation_t value;
    };
    static const entry_t entries[] = {
        { "truncate", RN_BLASTER_CASE_TRUNCATE },
        { "oversize", RN_BLASTER_CASE_OVERSIZE },
        { "wrong_epoch", RN_BLASTER_CASE_WRONG_EPOCH },
        { "wrong_filter", RN_BLASTER_CASE_WRONG_FILTER },
        { "reserved_type", RN_BLASTER_CASE_RESERVED_TYPE },
        { "replay", RN_BLASTER_CASE_REPLAY },
        { "bad_tag", RN_BLASTER_CASE_BAD_TAG },
    };
    for ( const entry_t & entry : entries )
    {
        if ( strcmp( text_value, entry.name ) == 0 )
        {
            *operation = entry.value;
            return true;
        }
    }
    return false;
}

struct rn_blaster_server_transport_profile_t
{
    const char * name;
    bool xdp_prefilter;
};

static const rn_blaster_server_transport_profile_t rn_blaster_server_transport_profiles[] = {
    { "xdp", true },
    { "gso", false },
    { "io_uring", false },
    { "msgx", false },
    { "iocp", false },
    { "rio", false },
};

static_assert( sizeof( rn_blaster_server_transport_profiles ) / sizeof( rn_blaster_server_transport_profiles[0] ) ==
                   RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES,
               "the manifest covers every physical server transport profile" );

static int rn_blaster_server_transport_profile( const std::string & name )
{
    for ( uint32_t i = 0; i < RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES; ++i )
    {
        if ( name == rn_blaster_server_transport_profiles[i].name )
        {
            return (int) i;
        }
    }
    return -1;
}

static bool rn_blaster_case_oracle_contract( const rn_blaster_case_t & item, uint32_t profile )
{
    const bool xdp = rn_blaster_server_transport_profiles[profile].xdp_prefilter;
    const char * expected_stage = NULL;
    const char * expected_counter = NULL;
    switch ( item.operation )
    {
        case RN_BLASTER_CASE_TRUNCATE:
            expected_stage = xdp ? "xdp-prefilter" : "server-ingress-size-filter";
            expected_counter = xdp ? "XDP_KERNEL_DROPPED_TOO_SMALL" : "PREFILTER_TOO_SHORT";
            break;
        case RN_BLASTER_CASE_OVERSIZE:
            expected_stage = xdp ? "xdp-prefilter" : "server-ingress-size-filter";
            expected_counter = xdp ? "XDP_KERNEL_DROPPED_TOO_LARGE" : "PREFILTER_TOO_LARGE";
            break;
        case RN_BLASTER_CASE_WRONG_EPOCH:
            expected_stage = xdp ? "xdp-prefilter" : "server-prefilter";
            expected_counter = xdp ? "XDP_KERNEL_DROPPED_WRONG_EPOCH" : "PREFILTER_WRONG_EPOCH";
            break;
        case RN_BLASTER_CASE_WRONG_FILTER:
            expected_stage = xdp ? "xdp-prefilter" : "server-prefilter";
            expected_counter = xdp ? "XDP_KERNEL_DROPPED_WRONG_FILTER" : "PREFILTER_WRONG_VALUE";
            break;
        case RN_BLASTER_CASE_RESERVED_TYPE:
            expected_stage = xdp ? "xdp-type-filter" : "server-type-filter";
            expected_counter = xdp ? "XDP_KERNEL_DROPPED_INVALID_TYPE" : "RESERVED_TYPE";
            break;
        case RN_BLASTER_CASE_REPLAY:
            expected_stage = "server-replay-window";
            expected_counter = "PAYLOAD_ALREADY_RECEIVED";
            break;
        case RN_BLASTER_CASE_BAD_TAG:
            expected_stage = "server-aead-open";
            expected_counter = "PAYLOAD_DID_NOT_OPEN";
            break;
    }
    return expected_stage && expected_counter &&
           strcmp( item.profile_oracles[profile].expected_stage, expected_stage ) == 0 &&
           strcmp( item.profile_oracles[profile].expected_counter, expected_counter ) == 0;
}

static bool rn_blaster_case_contract( const rn_blaster_case_t & item )
{
    const bool accepted = strcmp( item.sequence_state, "accepted" ) == 0;
    const bool fresh = strcmp( item.sequence_state, "fresh" ) == 0;
    const bool fresh_recovery = strcmp( item.expected_recovery, "fresh-authenticated-payload-accepted" ) == 0;
    switch ( item.operation )
    {
        case RN_BLASTER_CASE_TRUNCATE:
            return strcmp( item.name, "prefilter-too-short" ) == 0 &&
                   strcmp( item.source, "tests/fuzz/corpus/prefilter_too_short" ) == 0 &&
                   accepted && item.argument == 5u && item.expected_delta == 1u && fresh_recovery;
        case RN_BLASTER_CASE_OVERSIZE:
            return strcmp( item.name, "prefilter-too-large" ) == 0 &&
                   strcmp( item.source, "tests/fuzz/corpus/prefilter_too_large" ) == 0 &&
                   accepted && item.argument == ROCKETNET_RECEIVE_CEILING + 1u && item.expected_delta == 1u && fresh_recovery;
        case RN_BLASTER_CASE_WRONG_EPOCH:
            return strcmp( item.name, "wrong-filter-epoch" ) == 0 &&
                   strcmp( item.source, "sdk/rocketnet_wire.h:ROCKETNET_FILTER_EPOCH_UNKEYED" ) == 0 &&
                   accepted && item.argument == 1u && item.expected_delta == 1u && fresh_recovery;
        case RN_BLASTER_CASE_WRONG_FILTER:
            return strcmp( item.name, "wrong-filter-value" ) == 0 &&
                   strcmp( item.source, "tests/fuzz/corpus/prefilter_pcap_hostile_prefilter_1df577ed2292" ) == 0 &&
                   accepted && item.argument == 128u && item.expected_delta == 1u && fresh_recovery;
        case RN_BLASTER_CASE_RESERVED_TYPE:
            return strcmp( item.name, "reserved-packet-type" ) == 0 &&
                   strcmp( item.source, "sdk/rocketnet_wire.h:reserved-types-10-through-255" ) == 0 &&
                   accepted && item.argument == 10u && item.expected_delta == 1u && fresh_recovery;
        case RN_BLASTER_CASE_REPLAY:
            return strcmp( item.name, "session-replay" ) == 0 &&
                   strcmp( item.source, "tests/fuzz/corpus/prefilter_payload" ) == 0 &&
                   accepted && item.argument == 2u && item.expected_delta == 2u &&
                   strcmp( item.expected_recovery, "next-sequence-authenticated-payload-accepted" ) == 0;
        case RN_BLASTER_CASE_BAD_TAG:
            return strcmp( item.name, "bad-session-tag" ) == 0 &&
                   strcmp( item.source, "tests/fuzz/corpus/prefilter_payload_full" ) == 0 &&
                   fresh && item.argument == 1u && item.expected_delta == 1u && fresh_recovery;
    }
    return false;
}

static bool rn_blaster_manifest_load( const std::string & path, struct rn_blaster_manifest_t * manifest, std::string * error )
{
    memset( manifest, 0, sizeof( *manifest ) );
    std::string json;
    if ( !rn_blaster_read_file( path, &json ) )
    {
        *error = "cannot read bounded manifest";
        return false;
    }
    uint32_t schema_version = 0;
    if ( !rn_blaster_json_u32( json, "schema_version", &schema_version ) || schema_version != 1u ||
         json.find( "\"cases\"" ) == std::string::npos )
    {
        *error = "manifest schema_version must be exactly 1 and cases must be present";
        return false;
    }
    size_t cursor = 0;
    uint32_t operation_bits = 0;
    while ( true )
    {
        const size_t name_at = json.find( "\"name\"", cursor );
        if ( name_at == std::string::npos )
        {
            break;
        }
        const size_t object_begin = json.rfind( '{', name_at );
        const size_t object_end = json.find( '}', name_at );
        if ( object_begin == std::string::npos || object_end == std::string::npos || manifest->count >= RN_BLASTER_MAX_MANIFEST_CASES )
        {
            *error = "manifest object or case bound invalid";
            return false;
        }
        const std::string object = json.substr( object_begin, object_end - object_begin + 1u );
        rn_blaster_case_t & item = manifest->cases[manifest->count];
        char operation[32];
        if ( !rn_blaster_json_string( object, "name", item.name, sizeof( item.name ) ) ||
             !rn_blaster_json_string( object, "source", item.source, sizeof( item.source ) ) ||
             !rn_blaster_json_string( object, "operation", operation, sizeof( operation ) ) ||
             !rn_blaster_json_string( object, "expected_recovery", item.expected_recovery, sizeof( item.expected_recovery ) ) ||
             !rn_blaster_json_string( object, "sequence_state", item.sequence_state, sizeof( item.sequence_state ) ) ||
             !rn_blaster_json_u32( object, "argument", &item.argument ) ||
             !rn_blaster_json_u32( object, "expected_delta", &item.expected_delta ) ||
             !rn_blaster_case_operation( operation, &item.operation ) )
        {
            *error = "manifest case has an absent, overlong, or unknown field";
            return false;
        }
        for ( uint32_t profile = 0; profile < RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES; ++profile )
        {
            const std::string stage_key = std::string( "expected_stage_" ) + rn_blaster_server_transport_profiles[profile].name;
            const std::string counter_key = std::string( "expected_counter_" ) + rn_blaster_server_transport_profiles[profile].name;
            if ( !rn_blaster_json_string( object, stage_key.c_str(), item.profile_oracles[profile].expected_stage,
                                          sizeof( item.profile_oracles[profile].expected_stage ) ) ||
                 !rn_blaster_json_string( object, counter_key.c_str(), item.profile_oracles[profile].expected_counter,
                                          sizeof( item.profile_oracles[profile].expected_counter ) ) )
            {
                *error = "manifest case has a missing or overlong server transport profile oracle";
                return false;
            }
        }
        if ( strcmp( item.sequence_state, "accepted" ) != 0 && strcmp( item.sequence_state, "fresh" ) != 0 )
        {
            *error = "manifest sequence_state must be accepted or fresh";
            return false;
        }
        bool oracle_contract = true;
        for ( uint32_t profile = 0; profile < RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES; ++profile )
        {
            oracle_contract = oracle_contract && rn_blaster_case_oracle_contract( item, profile );
        }
        if ( !rn_blaster_case_contract( item ) || !oracle_contract )
        {
            *error = "manifest operation has a noncanonical argument, sequence state, or oracle";
            return false;
        }
        const uint32_t operation_bit = UINT32_C( 1 ) << (uint32_t) item.operation;
        if ( ( operation_bits & operation_bit ) != 0 )
        {
            *error = "manifest operations must be unique";
            return false;
        }
        operation_bits |= operation_bit;
        if ( strncmp( item.source, "sdk/rocketnet_wire.h", 20 ) != 0 &&
             strncmp( item.source, "tests/pcapng/regress/", 21 ) != 0 &&
             strncmp( item.source, "tests/fuzz/corpus/", 12 ) != 0 )
        {
            *error = "manifest source is not an approved existing schema or corpus";
            return false;
        }
        for ( uint32_t i = 0; i < manifest->count; ++i )
        {
            if ( strcmp( manifest->cases[i].name, item.name ) == 0 )
            {
                *error = "manifest case names must be unique";
                return false;
            }
        }
        ++manifest->count;
        cursor = object_end + 1u;
    }
    if ( manifest->count != 7u || operation_bits != UINT32_C( 0x7f ) )
    {
        *error = "manifest must contain the exact seven canonical operations";
        return false;
    }
    return true;
}

static void rn_blaster_tokens_wipe( std::vector<rn_blaster_token_t> * tokens );

static bool rn_blaster_tokens_load( const std::string & path, uint32_t wanted, std::vector<rn_blaster_token_t> * tokens, std::string * error )
{
    rn_blaster_tokens_wipe( tokens );
    tokens->reserve( wanted );
    std::string data;
    if ( !rn_blaster_read_file( path, &data ) )
    {
        *error = "cannot read token file";
        rn_blaster_tokens_wipe( tokens );
        return false;
    }
    size_t cursor = 0;
    while ( cursor < data.size() && tokens->size() < wanted )
    {
        size_t end = data.find( '\n', cursor );
        if ( end == std::string::npos )
        {
            end = data.size();
        }
        if ( end > cursor && data[end - 1u] == '\r' )
        {
            --end;
        }
        std::string line = data.substr( cursor, end - cursor );
        rn_blaster_string_wiper_t line_wiper = { &line };
        (void) line_wiper;
        if ( !line.empty() )
        {
            if ( line.size() != ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1u )
            {
                *error = "each token line must be exactly the canonical base64 length";
                rn_blaster_tokens_wipe( tokens );
                return false;
            }
            rn_blaster_token_t token = {};
            if ( rocketnet_base64_decode_data( line.c_str(), token.bytes.data(), token.bytes.size() ) != (int) token.bytes.size() )
            {
                sodium_memzero( token.bytes.data(), token.bytes.size() );
                *error = "token file contains invalid base64";
                rn_blaster_tokens_wipe( tokens );
                return false;
            }
            for ( const rn_blaster_token_t & existing : *tokens )
            {
                if ( sodium_memcmp( existing.bytes.data(), token.bytes.data(), token.bytes.size() ) == 0 )
                {
                    sodium_memzero( token.bytes.data(), token.bytes.size() );
                    *error = "token file contains a duplicate token";
                    rn_blaster_tokens_wipe( tokens );
                    return false;
                }
            }
            tokens->push_back( token );
            sodium_memzero( token.bytes.data(), token.bytes.size() );
        }
        cursor = data.find( '\n', cursor );
        if ( cursor == std::string::npos )
        {
            break;
        }
        ++cursor;
    }
    if ( tokens->size() != wanted )
    {
        *error = "token file does not contain exactly the requested minimum distinct token count";
        rn_blaster_tokens_wipe( tokens );
        return false;
    }
    return true;
}

static void rn_blaster_tokens_wipe( std::vector<rn_blaster_token_t> * tokens )
{
    for ( rn_blaster_token_t & token : *tokens )
    {
        sodium_memzero( token.bytes.data(), token.bytes.size() );
    }
    tokens->clear();
}

static bool rn_blaster_tokens_name_server( const std::vector<rn_blaster_token_t> & tokens,
                                           const rocketnet_address_t & server )
{
    for ( const rn_blaster_token_t & bytes : tokens )
    {
        rocketnet_connect_token_t token = {};
        rocketnet_signed_connect_data_t signed_fields = {};
        rocketnet_address_t selected = {};
        rocketnet_read_connect_token( &token, bytes.bytes.data() );
        rocketnet_read_signed_connect_data( &signed_fields, token.signed_connect_data );
        const uint8_t flags = signed_fields.address_flags;
        const bool valid_flags = flags != 0 &&
                                 ( flags & ~( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) ) == 0;
        if ( valid_flags && ( flags & ROCKETNET_ADDRESS_FLAG_IPV6 ) != 0 )
        {
            selected.type = ROCKETNET_ADDRESS_IPV6;
            const uint8_t * cursor = signed_fields.ipv6_address;
            for ( int i = 0; i < 8; ++i )
            {
                selected.data.ipv6[i] = rocketnet_read_uint16( &cursor );
            }
            selected.port = rocketnet_read_uint16( &cursor );
        }
        else if ( valid_flags )
        {
            selected.type = ROCKETNET_ADDRESS_IPV4;
            const uint8_t * cursor = signed_fields.ipv4_address;
            rocketnet_read_bytes( &cursor, selected.data.ipv4, 4 );
            selected.port = rocketnet_read_uint16( &cursor );
        }
        const bool matches = valid_flags && selected.port != 0 && rocketnet_address_equal( &selected, &server );
        sodium_memzero( &selected, sizeof( selected ) );
        sodium_memzero( &signed_fields, sizeof( signed_fields ) );
        sodium_memzero( &token, sizeof( token ) );
        if ( !matches )
        {
            return false;
        }
    }
    return true;
}

static void rn_blaster_usage( const char * program )
{
    fprintf( stderr,
             "usage: %s [--dry-run|--execute] --interface IFACE --queue N --xdp-mode native|skb\n"
             "  --source IP:PORT --server PRIVATE_IP:PORT --source-mac XX:.. --next-hop-mac XX:..\n"
             "  --packet-filter 8hex --token-file FILE --manifest FILE --clients N --duration 1..600\n"
             "  --rate-pps N --payload-bytes 1..%u --source-port 1024.. --seed N\n"
             "    (the largest message sent as one unreliable record; the test server's echo\n"
             "     returns only unreliable records, so an echo cell measures the transport\n"
             "     echo of one unreliable record per message, never a numbered message)\n"
             "  --mode throughput|robustness --server-transport-profile xdp|gso|io_uring|msgx|iocp|rio\n"
             "  --direction c2s|s2c|both --bind auto|copy|zero-copy\n"
             "  [--threads 1..%u] [--cpus LIST, one CPU per thread, e.g. 1-6 or 0-7,32-39]\n"
             "  --owned-bench-confirm %s\n"
             "       %s --self-test\n",
             program, RN_BLASTER_MAX_PAYLOAD_BYTES, RN_BLASTER_MAX_THREADS, RN_BLASTER_OWNED_CONFIRM, program );
}

static bool rn_blaster_next( int argc, char ** argv, int * i, const char ** value )
{
    if ( *i + 1 >= argc )
    {
        return false;
    }
    *value = argv[++( *i )];
    return true;
}

static bool rn_blaster_parse_args( int argc, char ** argv, rn_blaster_options_t * options, std::string * error )
{
    memset( &options->plan, 0, sizeof( options->plan ) );
    options->plan.mode = RN_BLASTER_MODE_THROUGHPUT;
    options->plan.direction = RN_BLASTER_DIRECTION_CLIENT_TO_SERVER;
    options->plan.bind = RN_BLASTER_BIND_AUTO;
    options->plan.threads = 1;
    options->execute = false;
    options->self_test = false;
    for ( int i = 1; i < argc; ++i )
    {
        const char * arg = argv[i];
        const char * value = NULL;
        if ( strcmp( arg, "--self-test" ) == 0 )
        {
            options->self_test = true;
        }
        else if ( strcmp( arg, "--execute" ) == 0 )
        {
            options->execute = true;
        }
        else if ( strcmp( arg, "--dry-run" ) == 0 )
        {
            options->execute = false;
        }
        else if ( !rn_blaster_next( argc, argv, &i, &value ) )
        {
            *error = std::string( "missing value for " ) + arg;
            return false;
        }
        else if ( strcmp( arg, "--interface" ) == 0 )
        {
            options->interface_name = value;
        }
        else if ( strcmp( arg, "--source" ) == 0 )
        {
            options->source_text = value;
        }
        else if ( strcmp( arg, "--server" ) == 0 )
        {
            options->server_text = value;
        }
        else if ( strcmp( arg, "--source-mac" ) == 0 )
        {
            options->source_mac_text = value;
        }
        else if ( strcmp( arg, "--next-hop-mac" ) == 0 )
        {
            options->next_hop_mac_text = value;
        }
        else if ( strcmp( arg, "--token-file" ) == 0 )
        {
            options->token_file = value;
        }
        else if ( strcmp( arg, "--manifest" ) == 0 )
        {
            options->manifest_file = value;
        }
        else if ( strcmp( arg, "--server-transport-profile" ) == 0 )
        {
            options->server_transport_profile = value;
        }
        else if ( strcmp( arg, "--packet-filter" ) == 0 )
        {
            options->packet_filter_text = value;
        }
        else if ( strcmp( arg, "--owned-bench-confirm" ) == 0 )
        {
            options->owned_confirmation = value;
        }
        else if ( strcmp( arg, "--xdp-mode" ) == 0 )
        {
            options->xdp_mode = value;
        }
        else if ( strcmp( arg, "--duration" ) == 0 && rn_blaster_parse_u32( value, 1, RN_BLASTER_MAX_DURATION_SECONDS, &options->plan.duration_seconds ) )
        {
        }
        else if ( strcmp( arg, "--clients" ) == 0 && rn_blaster_parse_u32( value, 1, RN_BLASTER_MAX_CLIENTS, &options->plan.clients ) )
        {
        }
        else if ( strcmp( arg, "--rate-pps" ) == 0 && rn_blaster_parse_u32( value, 1, RN_BLASTER_MAX_RATE_PPS, &options->plan.rate_pps ) )
        {
        }
        else if ( strcmp( arg, "--payload-bytes" ) == 0 && rn_blaster_parse_u32( value, 1, RN_BLASTER_MAX_PAYLOAD_BYTES, &options->plan.payload_bytes ) )
        {
        }
        else if ( strcmp( arg, "--queue" ) == 0 && rn_blaster_parse_u32( value, 0, 65535, &options->plan.queue ) )
        {
        }
        else if ( strcmp( arg, "--threads" ) == 0 && rn_blaster_parse_u32( value, 1, RN_BLASTER_MAX_THREADS, &options->plan.threads ) )
        {
        }
        else if ( strcmp( arg, "--cpus" ) == 0 )
        {
            options->cpus_text = value;
        }
        else if ( strcmp( arg, "--source-port" ) == 0 )
        {
            uint32_t parsed = 0;
            if ( !rn_blaster_parse_u32( value, RN_BLASTER_MIN_SOURCE_PORT, UINT16_MAX, &parsed ) )
            {
                *error = "invalid source port";
                return false;
            }
            options->plan.source_port = (uint16_t) parsed;
        }
        else if ( strcmp( arg, "--seed" ) == 0 && rn_blaster_parse_u64( value, &options->plan.seed ) )
        {
        }
        else if ( strcmp( arg, "--mode" ) == 0 && strcmp( value, "throughput" ) == 0 )
        {
            options->plan.mode = RN_BLASTER_MODE_THROUGHPUT;
        }
        else if ( strcmp( arg, "--mode" ) == 0 && strcmp( value, "robustness" ) == 0 )
        {
            options->plan.mode = RN_BLASTER_MODE_ROBUSTNESS;
        }
        else if ( strcmp( arg, "--direction" ) == 0 && strcmp( value, "c2s" ) == 0 )
        {
            options->plan.direction = RN_BLASTER_DIRECTION_CLIENT_TO_SERVER;
        }
        else if ( strcmp( arg, "--direction" ) == 0 && strcmp( value, "s2c" ) == 0 )
        {
            options->plan.direction = RN_BLASTER_DIRECTION_SERVER_TO_CLIENT;
        }
        else if ( strcmp( arg, "--direction" ) == 0 && strcmp( value, "both" ) == 0 )
        {
            options->plan.direction = RN_BLASTER_DIRECTION_BIDIRECTIONAL;
        }
        else if ( strcmp( arg, "--bind" ) == 0 && strcmp( value, "auto" ) == 0 )
        {
            options->plan.bind = RN_BLASTER_BIND_AUTO;
        }
        else if ( strcmp( arg, "--bind" ) == 0 && strcmp( value, "copy" ) == 0 )
        {
            options->plan.bind = RN_BLASTER_BIND_COPY;
        }
        else if ( strcmp( arg, "--bind" ) == 0 && strcmp( value, "zero-copy" ) == 0 )
        {
            options->plan.bind = RN_BLASTER_BIND_ZERO_COPY;
        }
        else
        {
            *error = std::string( "unknown or invalid argument: " ) + arg;
            return false;
        }
    }
    if ( options->self_test )
    {
        return argc == 2;
    }
    /* Address parsing happens in main after every explicit text field exists.
       Validate all address-independent bounds here, then validate the real
       family together with the parsed source and server before dry-run or
       execution. */
    struct rn_blaster_plan_t bounded_plan = options->plan;
    bounded_plan.address_family = 4;
    if ( !rn_blaster_plan_valid( &bounded_plan ) )
    {
        *error = "plan values are absent or out of bounds";
        return false;
    }
    if ( !options->cpus_text.empty() )
    {
        uint32_t cpus[RN_BLASTER_MAX_THREADS];
        const uint32_t count = rn_blaster_parse_cpu_list( options->cpus_text.c_str(), cpus, RN_BLASTER_MAX_THREADS );
        if ( count == 0 || count != options->plan.threads )
        {
            *error = "--cpus must list exactly one distinct CPU per thread";
            return false;
        }
        options->cpus.assign( cpus, cpus + count );
    }
    if ( options->interface_name.empty() || options->source_text.empty() || options->server_text.empty() ||
         options->source_mac_text.empty() || options->next_hop_mac_text.empty() || options->token_file.empty() ||
         options->manifest_file.empty() || options->packet_filter_text.empty() ||
         ( options->xdp_mode != "native" && options->xdp_mode != "skb" ) )
    {
        *error = "all interface, address, MAC, token, manifest, filter, and xdp-mode inputs are explicit";
        return false;
    }
    if ( options->owned_confirmation != RN_BLASTER_OWNED_CONFIRM )
    {
        *error = "owned bench confirmation absent";
        return false;
    }
    if ( !options->server_transport_profile.empty() &&
         rn_blaster_server_transport_profile( options->server_transport_profile ) < 0 )
    {
        *error = "unsupported server transport profile (physical profiles: xdp,gso,io_uring,msgx,iocp,rio)";
        return false;
    }
    if ( options->plan.mode == RN_BLASTER_MODE_ROBUSTNESS && options->server_transport_profile.empty() )
    {
        *error = "robustness mode requires an explicit --server-transport-profile";
        return false;
    }
    return true;
}

static bool rn_blaster_address_private( const rocketnet_address_t & address )
{
    if ( address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        return rn_blaster_ipv4_private( address.data.ipv4 ) != 0;
    }
    if ( address.type == ROCKETNET_ADDRESS_IPV6 )
    {
        uint8_t bytes[16];
        for ( int i = 0; i < 8; ++i )
        {
            bytes[2 * i] = (uint8_t) ( address.data.ipv6[i] >> 8 );
            bytes[2 * i + 1] = (uint8_t) address.data.ipv6[i];
        }
        return rn_blaster_ipv6_ula( bytes ) != 0;
    }
    return false;
}

static const char * rn_blaster_mode_name( enum rn_blaster_mode_t mode )
{
    return mode == RN_BLASTER_MODE_ROBUSTNESS ? "robustness" : "throughput";
}

static const char * rn_blaster_direction_name( enum rn_blaster_direction_t direction )
{
    return direction == RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ? "c2s" : direction == RN_BLASTER_DIRECTION_SERVER_TO_CLIENT ? "s2c"
                                                                                                                           : "both";
}

static const char * rn_blaster_bind_name( enum rn_blaster_bind_t bind )
{
    return bind == RN_BLASTER_BIND_COPY ? "copy" : bind == RN_BLASTER_BIND_ZERO_COPY ? "zero-copy"
                                                                                     : "auto";
}

static void rn_blaster_print_plan( const rn_blaster_options_t & options, const rn_blaster_manifest_t & manifest,
                                   const rocketnet_address_t & source, const rocketnet_address_t & server )
{
    (void) source;
    (void) server;
    const uint64_t offers = rn_blaster_offered_packets( options.plan.duration_seconds, options.plan.rate_pps );
    uint32_t mutation_datagrams = 0;
    for ( uint32_t i = 0; i < manifest.count; ++i )
    {
        mutation_datagrams += manifest.cases[i].operation == RN_BLASTER_CASE_REPLAY ? manifest.cases[i].argument : 1u;
    }
    /* 33 bytes before a transport payload, then the message record's three
     * header bytes, body, and one trailing form byte. */
    const uint32_t nominal_wire = 37u + options.plan.payload_bytes;
    const uint32_t frame = rn_blaster_ethernet_frame_bytes( options.plan.address_family, nominal_wire );
    const uint32_t wire_slot = rn_blaster_physical_wire_bytes( frame );
    rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    const uint64_t arena_required = rn_arena_required_bytes( &message_config, (int) options.plan.clients );
    printf( "PLAN version=1 attach=false transmit=false\n" );
    printf( "PLAN interface=%s queue=%u xdp_mode=%s bind_request=%s frame_bytes=%u umem_frames=%u"
            " tx_ring=%u rx_ring=%u fill_ring=%u completion_ring=%u\n",
            options.interface_name.c_str(), options.plan.queue, options.xdp_mode.c_str(), rn_blaster_bind_name( options.plan.bind ),
            RN_BLASTER_FRAME_BYTES, RN_BLASTER_UMEM_FRAMES, RN_BLASTER_TX_RING_SIZE, RN_BLASTER_RX_RING_SIZE,
            RN_BLASTER_FILL_RING_SIZE, RN_BLASTER_COMPLETION_RING_SIZE );
    printf( "PLAN source=%s source_ports=%u-%u server=%s target_policy=private_owned_bench\n",
            options.source_text.c_str(), (unsigned) options.plan.source_port,
            (unsigned) ( options.plan.source_port + options.plan.clients - 1u ), options.server_text.c_str() );
    printf( "PLAN mode=%s direction=%s clients=%u duration_seconds=%u rate_pps=%u payload_bytes=%u seed=%" PRIu64 "\n",
            rn_blaster_mode_name( options.plan.mode ), rn_blaster_direction_name( options.plan.direction ), options.plan.clients,
            options.plan.duration_seconds, options.plan.rate_pps, options.plan.payload_bytes, options.plan.seed );
    if ( options.plan.mode == RN_BLASTER_MODE_THROUGHPUT )
    {
        printf( "PLAN throughput_offers_per_enabled_direction=%" PRIu64
                " nominal_rocketnet_bytes=%u nominal_frame_bytes=%u"
                " nominal_wire_slot_bytes=%u nominal_physical_wire_bytes_per_direction=%" PRIu64 "\n",
                offers, nominal_wire, frame, wire_slot, offers * wire_slot );
    }
    else
    {
        printf( "PLAN mutation_cases=%u mutation_datagrams=%u baseline_datagrams=1 recovery_datagrams=%u"
                " pacing=ack-driven rate_pps_ignored=true\n",
                manifest.count, mutation_datagrams, manifest.count );
        printf( "PLAN server_transport_profile=%s profile_source=operator-declared runtime_autodetect=false"
                " counter_observed=false\n",
                options.server_transport_profile.c_str() );
        const int profile = rn_blaster_server_transport_profile( options.server_transport_profile );
        rocketnet_assert( profile >= 0 );
        for ( uint32_t i = 0; i < manifest.count; ++i )
        {
            printf( "PLAN_CASE name=%s server_transport_profile=%s sequence_state=%s expected_stage=%s"
                    " expected_counter=%s expected_delta=%u expected_recovery=%s\n",
                    manifest.cases[i].name, options.server_transport_profile.c_str(), manifest.cases[i].sequence_state,
                    manifest.cases[i].profile_oracles[profile].expected_stage,
                    manifest.cases[i].profile_oracles[profile].expected_counter, manifest.cases[i].expected_delta,
                    manifest.cases[i].expected_recovery );
        }
    }
    printf( "PLAN manifest_cases=%u nominal_valid_rocketnet_bytes=%u nominal_valid_frame_bytes=%u"
            " nominal_valid_wire_slot_bytes=%u\n",
            manifest.count, nominal_wire, frame, wire_slot );
    printf( "PLAN threads=%u cpus=%s queues=0-%u reply_steering=%s\n", options.plan.threads,
            options.cpus_text.empty() ? "unpinned" : options.cpus_text.c_str(), options.plan.threads - 1u,
            options.plan.threads == 1 ? "one_queue" : "source_port_rss_hash" );
    printf( "PLAN protocol=rocketnet_client_protocol crypto=production_helpers message_config=default record=unreliable\n" );
    printf( "PLAN endpoint_arena_required_bytes=%" PRIu64 "\n", arena_required );
    printf( "PLAN downstream=controlled_server_workload_contract rss=%s\n",
            options.plan.threads == 1 ? "externally_provisioned_one_queue" : "nic_toeplitz_read_at_execute" );
    if ( options.plan.mode == RN_BLASTER_MODE_ROBUSTNESS )
    {
        printf( "PLAN robustness_family=ipv4 oversize_rocketnet_bytes=%u\n", ROCKETNET_RECEIVE_CEILING + 1u );
    }
    printf( "PLAN logging=no_tokens_no_keys_no_packet_bodies cleanup=xsk_umem_program_process_settings_postcheck\n" );
    printf( "PLAN next=%s\n", options.execute ? "attach_after_plan" : "dry_run_stop" );
    fflush( stdout );
}

static int rn_blaster_mutate_case( const rn_blaster_case_t & item, const uint8_t * valid, int valid_bytes,
                                   uint8_t * output, int capacity )
{
    if ( valid_bytes <= 0 || capacity < ROCKETNET_RECEIVE_CEILING + 1 )
    {
        return 0;
    }
    memcpy( output, valid, (size_t) valid_bytes );
    int bytes = valid_bytes;
    switch ( item.operation )
    {
        case RN_BLASTER_CASE_TRUNCATE:
            bytes = (int) std::min<uint32_t>( item.argument, (uint32_t) valid_bytes - 1u );
            break;
        case RN_BLASTER_CASE_OVERSIZE:
            bytes = (int) item.argument;
            memset( output + valid_bytes, 0xa5, (size_t) ( bytes - valid_bytes ) );
            break;
        case RN_BLASTER_CASE_WRONG_EPOCH:
            output[1] = (uint8_t) item.argument;
            break;
        case RN_BLASTER_CASE_WRONG_FILTER:
            output[2] ^= (uint8_t) item.argument;
            break;
        case RN_BLASTER_CASE_RESERVED_TYPE:
            output[0] = (uint8_t) item.argument;
            break;
        case RN_BLASTER_CASE_REPLAY:
            break;
        case RN_BLASTER_CASE_BAD_TAG:
            output[bytes - 1] ^= (uint8_t) item.argument;
            break;
    }
    return bytes;
}

/* The size of the next packet the client endpoint wants written, or 0. Every
 * packet the tool writes is its own flush (sdk/rocketnet_endpoint.h): the
 * write pass is opened first, as the SDK's send_packets does, because a pass
 * that an earlier request this update ended (nothing due then) answers 0
 * until it is opened again, and a record queued since would go unsent. */
static int rn_blaster_endpoint_request( rn_endpoint_t * endpoint, double now )
{
    rn_endpoint_begin_flush( endpoint );
    return rn_endpoint_next_packet_bytes( endpoint, now );
}

static int rn_blaster_self_test()
{
    int failures = 0;
#define RN_BLASTER_CHECK( expression ) do { if ( !( expression ) ) { fprintf( stderr, "self-test failed: %s:%d: %s\n", __FILE__, __LINE__, #expression ); ++failures; } } while ( 0 )
    rn_blaster_plan_t plan = {};
    plan.duration_seconds = 600;
    plan.clients = 1000;
    plan.rate_pps = 20000000;
    plan.payload_bytes = 1200;
    plan.source_port = 1024;
    plan.address_family = 4;
    plan.threads = 1;
    plan.direction = RN_BLASTER_DIRECTION_BIDIRECTIONAL;
    RN_BLASTER_CHECK( rn_blaster_plan_valid( &plan ) );
    plan.threads = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.threads = RN_BLASTER_MAX_THREADS + 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.threads = RN_BLASTER_MAX_THREADS;
    RN_BLASTER_CHECK( rn_blaster_plan_valid( &plan ) );
    plan.clients = RN_BLASTER_MAX_THREADS - 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.clients = 1000;
    plan.mode = RN_BLASTER_MODE_ROBUSTNESS;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.threads = 1;
    RN_BLASTER_CHECK( rn_blaster_plan_valid( &plan ) );
    plan.mode = RN_BLASTER_MODE_THROUGHPUT;
    plan.payload_bytes = RN_BLASTER_MAX_PAYLOAD_BYTES;
    RN_BLASTER_CHECK( rn_blaster_plan_valid( &plan ) );
    plan.payload_bytes = 1200;
    plan.duration_seconds = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.duration_seconds = 601;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.duration_seconds = 600;
    plan.clients = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.clients = RN_BLASTER_MAX_CLIENTS + 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.clients = RN_BLASTER_MAX_CLIENTS;
    plan.rate_pps = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.rate_pps = RN_BLASTER_MAX_RATE_PPS + 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.rate_pps = RN_BLASTER_MAX_RATE_PPS;
    plan.payload_bytes = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.payload_bytes = RN_BLASTER_MAX_PAYLOAD_BYTES + 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.payload_bytes = RN_BLASTER_MAX_PAYLOAD_BYTES;
    plan.queue = 1;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.queue = 0;
    plan.source_port = RN_BLASTER_MIN_SOURCE_PORT - 1u;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.source_port = UINT16_MAX;
    plan.clients = 2;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.source_port = RN_BLASTER_MIN_SOURCE_PORT;
    plan.clients = RN_BLASTER_MAX_CLIENTS;
    plan.address_family = 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.address_family = 4;
    plan.direction = (rn_blaster_direction_t) 0;
    RN_BLASTER_CHECK( !rn_blaster_plan_valid( &plan ) );
    plan.direction = RN_BLASTER_DIRECTION_BIDIRECTIONAL;
    RN_BLASTER_CHECK( rn_blaster_plan_valid( &plan ) );
    RN_BLASTER_CHECK( rn_blaster_offered_packets( 600, 20000000 ) == UINT64_C( 12000000000 ) );
    RN_BLASTER_CHECK( rn_blaster_ethernet_frame_bytes( 4, 100 ) == 142u );
    RN_BLASTER_CHECK( rn_blaster_ethernet_frame_bytes( 6, 100 ) == 162u );
    RN_BLASTER_CHECK( rn_blaster_physical_wire_bytes( 142 ) == 166u );
    RN_BLASTER_CHECK( rn_blaster_physical_wire_bytes( 20 ) == 84u );
    RN_BLASTER_CHECK( rn_blaster_schedule_nanoseconds( 5, 10 ) == UINT64_C( 500000000 ) );
    RN_BLASTER_CHECK( rn_blaster_schedule_nanoseconds( 0, RN_BLASTER_MAX_RATE_PPS ) == 0 );
    const uint64_t maximum_offers = rn_blaster_offered_packets( 600, RN_BLASTER_MAX_RATE_PPS );
    RN_BLASTER_CHECK( rn_blaster_schedule_nanoseconds( maximum_offers - 1u, RN_BLASTER_MAX_RATE_PPS ) <
                      UINT64_C( 600000000000 ) );
    RN_BLASTER_CHECK( rn_blaster_schedule_nanoseconds( maximum_offers, RN_BLASTER_MAX_RATE_PPS ) ==
                      UINT64_C( 600000000000 ) );
    RN_BLASTER_CHECK( rn_blaster_schedule_client( 7, 42, 1000 ) == rn_blaster_schedule_client( 7, 42, 1000 ) );
    RN_BLASTER_CHECK( rn_blaster_schedule_client( 7, 42, 1000 ) < 1000 );
    const uint8_t private_a[4] = { 10, 1, 2, 3 }, private_b[4] = { 172, 31, 2, 3 }, public_a[4] = { 8, 8, 8, 8 };
    RN_BLASTER_CHECK( rn_blaster_ipv4_private( private_a ) );
    RN_BLASTER_CHECK( rn_blaster_ipv4_private( private_b ) );
    RN_BLASTER_CHECK( !rn_blaster_ipv4_private( public_a ) );
    uint8_t ula[16] = {};
    ula[0] = 0xfd;
    RN_BLASTER_CHECK( rn_blaster_ipv6_ula( ula ) );
    ula[0] = 0x20;
    RN_BLASTER_CHECK( !rn_blaster_ipv6_ula( ula ) );
    uint8_t valid[100];
    uint8_t mutated[ROCKETNET_RECEIVE_CEILING + 1];
    for ( uint32_t i = 0; i < (uint32_t) sizeof( valid ); ++i )
    {
        valid[i] = (uint8_t) i;
    }
    rn_blaster_case_t mutation = {};
    mutation.operation = RN_BLASTER_CASE_TRUNCATE;
    mutation.argument = 5;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) == 5 );
    mutation.operation = RN_BLASTER_CASE_OVERSIZE;
    mutation.argument = ROCKETNET_RECEIVE_CEILING + 1u;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                      ROCKETNET_RECEIVE_CEILING + 1 );
    mutation.operation = RN_BLASTER_CASE_WRONG_EPOCH;
    mutation.argument = 1;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                          (int) sizeof( valid ) &&
                      mutated[1] == 1 );
    mutation.operation = RN_BLASTER_CASE_WRONG_FILTER;
    mutation.argument = 128;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                          (int) sizeof( valid ) &&
                      mutated[2] == (uint8_t) ( valid[2] ^ 128u ) );
    mutation.operation = RN_BLASTER_CASE_RESERVED_TYPE;
    mutation.argument = 10;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                          (int) sizeof( valid ) &&
                      mutated[0] == 10 );
    mutation.operation = RN_BLASTER_CASE_REPLAY;
    mutation.argument = 2;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                          (int) sizeof( valid ) &&
                      memcmp( valid, mutated, sizeof( valid ) ) == 0 );
    mutation.operation = RN_BLASTER_CASE_BAD_TAG;
    mutation.argument = 1;
    RN_BLASTER_CHECK( rn_blaster_mutate_case( mutation, valid, (int) sizeof( valid ), mutated, (int) sizeof( mutated ) ) ==
                          (int) sizeof( valid ) &&
                      mutated[sizeof( valid ) - 1u] == (uint8_t) ( valid[sizeof( valid ) - 1u] ^ 1u ) );
    uint8_t body[ROCKETNET_MESSAGE_CONFIG_BYTES];
    rocketnet_message_config_body_t config = { ROCKETNET_MESSAGE_CONFIG_VERSION, 0xffu, 32u, 1024u, 1048576u, 1048576u, 1152u };
    rocketnet_message_write_config_body( body, &config );
    rocketnet_message_record_t record = {};
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.control = 1;
    record.data = body;
    record.bytes = sizeof( body );
    uint8_t payload[128];
    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, sizeof( payload ) );
    RN_BLASTER_CHECK( rocketnet_message_writer_add( &writer, &record ) );
    RN_BLASTER_CHECK( rocketnet_message_writer_finish( &writer ) > (int) sizeof( body ) );

    /* The largest body is one record inside one packet payload. */
    {
        static uint8_t largest[RN_BLASTER_MAX_PAYLOAD_BYTES + 1u];
        uint8_t packet_payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rocketnet_message_record_t unreliable = {};
        unreliable.code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        unreliable.data = largest;
        unreliable.bytes = (int) RN_BLASTER_MAX_PAYLOAD_BYTES;
        rocketnet_message_writer_begin( &writer, packet_payload, sizeof( packet_payload ) );
        RN_BLASTER_CHECK( rocketnet_message_writer_add( &writer, &unreliable ) );
        RN_BLASTER_CHECK( rocketnet_message_writer_finish( &writer ) == (int) RN_BLASTER_MAX_PAYLOAD_BYTES + 4 );
    }

    /* Toeplitz against Microsoft's published RSS verification suite: the
     * IPv4 two-tuple and four-tuple hashes of its five vectors. */
    {
        static const uint8_t key[40] = { 0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3,
                                         0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3,
                                         0x80, 0x30, 0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa };
        struct vector_t
        {
            uint8_t source[4];
            uint16_t source_port;
            uint8_t destination[4];
            uint16_t destination_port;
            uint32_t addresses_hash;
            uint32_t ports_hash;
        };
        static const vector_t vectors[] = {
            { { 66, 9, 149, 187 }, 2794, { 161, 142, 100, 80 }, 1766, 0x323e8fc2u, 0x51ccc178u },
            { { 199, 92, 111, 2 }, 14230, { 65, 69, 140, 83 }, 4739, 0xd718262au, 0xc626b0eau },
            { { 24, 19, 198, 95 }, 12898, { 12, 22, 207, 184 }, 38024, 0xd2d0a5deu, 0x5c2b394au },
            { { 38, 27, 205, 30 }, 48228, { 209, 142, 163, 6 }, 2217, 0x82989176u, 0xafc7327fu },
            { { 153, 39, 163, 191 }, 44251, { 202, 188, 127, 2 }, 1303, 0x5d1809c5u, 0x10e828a2u },
        };
        for ( const vector_t & vector : vectors )
        {
            uint8_t input[36];
            const size_t input_bytes = rn_blaster_rss_input( 4, vector.source, vector.source_port, vector.destination,
                                                             vector.destination_port, input );
            RN_BLASTER_CHECK( input_bytes == 12u );
            RN_BLASTER_CHECK( rn_blaster_toeplitz( key, sizeof( key ), input, 8u ) == vector.addresses_hash );
            RN_BLASTER_CHECK( rn_blaster_toeplitz( key, sizeof( key ), input, input_bytes ) == vector.ports_hash );
        }
        uint8_t short_input[36] = {};
        RN_BLASTER_CHECK( rn_blaster_toeplitz( key, 15u, short_input, 12u ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_rss_input( 6, short_input, 1, short_input, 2, short_input ) == 36u );

        /* Port selection: every chosen port's reply hashes to its owner's
         * queue, shares are balanced, ports ascend within a thread, and a
         * table that names no owned queue exhausts the port space. */
        uint32_t indirection[128];
        for ( uint32_t i = 0; i < 128u; ++i )
        {
            indirection[i] = i % 6u;
        }
        const uint8_t server_ip[4] = { 10, 42, 0, 1 };
        const uint8_t client_ip[4] = { 10, 42, 0, 2 };
        static uint16_t ports[1000];
        static uint8_t owner[65536];
        RN_BLASTER_CHECK( rn_blaster_assign_ports( key, sizeof( key ), indirection, 128u, 4, server_ip, 40000, client_ip, 42000,
                                                   4u, 1000u, ports, owner ) );
        uint32_t owned[4] = { 0, 0, 0, 0 };
        uint32_t offset = 0;
        for ( uint32_t thread = 0; thread < 4u; ++thread )
        {
            const uint32_t share = rn_blaster_thread_share( 1000u, 4u, thread );
            for ( uint32_t i = 0; i < share; ++i )
            {
                const uint16_t port = ports[offset + i];
                RN_BLASTER_CHECK( port >= 42000u );
                RN_BLASTER_CHECK( owner[port] == thread );
                RN_BLASTER_CHECK( rn_blaster_rss_queue( key, sizeof( key ), indirection, 128u, 4, server_ip, 40000, client_ip, port ) == thread );
                RN_BLASTER_CHECK( i == 0 || port > ports[offset + i - 1u] );
            }
            offset += share;
        }
        for ( uint32_t port = 0; port <= UINT16_MAX; ++port )
        {
            if ( owner[port] != RN_BLASTER_PORT_UNOWNED )
            {
                RN_BLASTER_CHECK( owner[port] < 4u );
                ++owned[owner[port] < 4u ? owner[port] : 0u];
            }
        }
        RN_BLASTER_CHECK( owned[0] == 250u && owned[1] == 250u && owned[2] == 250u && owned[3] == 250u );
        RN_BLASTER_CHECK( owner[41999] == RN_BLASTER_PORT_UNOWNED );
        RN_BLASTER_CHECK( rn_blaster_assign_ports( NULL, 0, NULL, 0, 4, server_ip, 40000, client_ip, 42000, 1u, 7u, ports, owner ) );
        for ( uint32_t i = 0; i < 7u; ++i )
        {
            RN_BLASTER_CHECK( ports[i] == 42000u + i && owner[42000u + i] == 0u );
        }
        RN_BLASTER_CHECK( owner[42007] == RN_BLASTER_PORT_UNOWNED );
        for ( uint32_t i = 0; i < 128u; ++i )
        {
            indirection[i] = 5u;
        }
        RN_BLASTER_CHECK( !rn_blaster_assign_ports( key, sizeof( key ), indirection, 128u, 4, server_ip, 40000, client_ip, 42000,
                                                    4u, 8u, ports, owner ) );
        RN_BLASTER_CHECK( !rn_blaster_assign_ports( key, sizeof( key ), indirection, 128u, 4, server_ip, 40000, client_ip, 42000,
                                                    4u, 3u, ports, owner ) );
    }

    /* Shares, seeds, CPU lists and the summed counters. */
    RN_BLASTER_CHECK( rn_blaster_thread_share( 1000u, 6u, 0 ) == 167u && rn_blaster_thread_share( 1000u, 6u, 3 ) == 167u &&
                      rn_blaster_thread_share( 1000u, 6u, 4 ) == 166u && rn_blaster_thread_share( 1000u, 6u, 6 ) == 0u );
    {
        uint32_t sum = 0;
        for ( uint32_t thread = 0; thread < 7u; ++thread )
        {
            sum += rn_blaster_thread_share( 20000000u, 7u, thread );
        }
        RN_BLASTER_CHECK( sum == 20000000u );
    }
    RN_BLASTER_CHECK( rn_blaster_thread_share( 5u, 0u, 0u ) == 0u );
    RN_BLASTER_CHECK( rn_blaster_thread_seed( 0x1234u, 0 ) == 0x1234u );
    RN_BLASTER_CHECK( rn_blaster_thread_seed( 0x1234u, 1 ) != rn_blaster_thread_seed( 0x1234u, 2 ) );
    {
        uint32_t cpus[RN_BLASTER_MAX_THREADS];
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "1-6", cpus, RN_BLASTER_MAX_THREADS ) == 6u && cpus[0] == 1u && cpus[5] == 6u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "0-7,32-39", cpus, RN_BLASTER_MAX_THREADS ) == 16u && cpus[8] == 32u &&
                          cpus[15] == 39u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "5", cpus, RN_BLASTER_MAX_THREADS ) == 1u && cpus[0] == 5u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "1-3,2", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "3-1", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "1,", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "1-", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "a", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "1024", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
        RN_BLASTER_CHECK( rn_blaster_parse_cpu_list( "0-32", cpus, RN_BLASTER_MAX_THREADS ) == 0u );
    }
    {
        rn_blaster_results_t total = {};
        rn_blaster_results_t part = {};
        part.submitted = 3;
        part.receive_wrong_thread = 1;
        part.physical_wire_bytes = 501;
        part.transmit_wakeups = 2;
        rn_blaster_results_add( &total, &part );
        part.submitted = 4;
        rn_blaster_results_add( &total, &part );
        RN_BLASTER_CHECK( total.submitted == 7u && total.receive_wrong_thread == 2u && total.physical_wire_bytes == 1002u &&
                          total.transmit_wakeups == 4u && total.offered == 0u );
    }
    {
        // every counter the run is judged on fails it alone; the reported-only ones do not
        rn_blaster_results_t clean_results = {};
        rn_blaster_results_t clean_disconnect = {};
        clean_results.receive_refused = 5;
        clean_results.not_ready = 3;
        clean_results.unsent = 2;
        clean_results.backpressure = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &clean_results, &clean_disconnect ) == NULL );
        rn_blaster_results_t bad = clean_results;
        bad.receive_wrong_thread = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &bad, &clean_disconnect ) &&
                          strcmp( rn_blaster_counters_refusal( &bad, &clean_disconnect ), "rx_wrong_thread" ) == 0 );
        bad = clean_results;
        bad.descriptor_refused = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &bad, &clean_disconnect ) &&
                          strcmp( rn_blaster_counters_refusal( &bad, &clean_disconnect ), "descriptor_refused" ) == 0 );
        bad = clean_results;
        bad.payload_errors = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &bad, &clean_disconnect ) &&
                          strcmp( rn_blaster_counters_refusal( &bad, &clean_disconnect ), "payload_errors" ) == 0 );
        bad = clean_results;
        bad.ownership_errors = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &bad, &clean_disconnect ) &&
                          strcmp( rn_blaster_counters_refusal( &bad, &clean_disconnect ), "ownership_errors" ) == 0 );
        rn_blaster_results_t bad_disconnect = {};
        bad_disconnect.ownership_errors = 1;
        RN_BLASTER_CHECK( rn_blaster_counters_refusal( &clean_results, &bad_disconnect ) &&
                          strcmp( rn_blaster_counters_refusal( &clean_results, &bad_disconnect ), "ownership_errors" ) == 0 );
    }
    {
        // a record queued after this update's write pass ended (nothing was due
        // at the session's last request) still gets a packet: each request
        // opens the pass, as a flush does
        rocketnet_message_config_t message_config;
        rocketnet_message_default_config( &message_config );
        const uint64_t arena_required = rn_arena_required_bytes( &message_config, 1 );
        RN_BLASTER_CHECK( arena_required > 0 );
        std::vector<uint8_t> storage( (size_t) arena_required + ROCKETNET_ALLOCATION_ALIGNMENT );
        uint8_t * block = (uint8_t *) ( ( (uintptr_t) storage.data() + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) &
                                        ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
        rocketnet_counters_t message_counters = {};
        rn_arena_t * arena = arena_required > 0 ? rn_arena_create_in_block( block, arena_required, &message_config, 1, &message_counters ) : NULL;
        RN_BLASTER_CHECK( arena != NULL );
        if ( arena )
        {
            rn_endpoint_t endpoint;
            memset( &endpoint, 0, sizeof( endpoint ) );
            rn_endpoint_config_t endpoint_config = {};
            endpoint_config.config = &message_config;
            endpoint_config.send_kbps = 0;
            endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
            rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
            rn_endpoint_reset( &endpoint, 0.0 );
            (void) rn_endpoint_begin_update( &endpoint );
            RN_BLASTER_CHECK( rn_blaster_endpoint_request( &endpoint, 0.0 ) == 0 );
            uint8_t queued_body[64] = {};
            RN_BLASTER_CHECK( rn_endpoint_send( &endpoint, queued_body, (int) sizeof( queued_body ), ROCKETNET_RELIABILITY_UNRELIABLE,
                                                ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) != 0 );
            RN_BLASTER_CHECK( rn_blaster_endpoint_request( &endpoint, 0.0 ) > 0 );
            rn_arena_destroy( arena );
        }
    }
#undef RN_BLASTER_CHECK
    if ( failures == 0 )
    {
        printf( "xdp_packet_blaster self-test: PASS\n" );
    }
    return failures == 0 ? 0 : 1;
}

/* The Linux runtime is below. Dry-run and self-test never enter it. */
static int rn_blaster_execute( const rn_blaster_options_t & options, const rn_blaster_manifest_t & manifest,
                               const rocketnet_address_t & source, const rocketnet_address_t & server,
                               const uint8_t source_mac[6], const uint8_t next_hop_mac[6], const uint8_t packet_filter[4],
                               const std::vector<rn_blaster_token_t> & tokens );

int main( int argc, char ** argv )
{
    rn_blaster_options_t options = {};
    std::string error;
    if ( !rn_blaster_parse_args( argc, argv, &options, &error ) )
    {
        fprintf( stderr, "xdp_packet_blaster: %s\n", error.c_str() );
        rn_blaster_usage( argv[0] );
        return 2;
    }
    if ( options.self_test )
    {
        return rn_blaster_self_test();
    }

    rocketnet_address_t source = {}, server = {};
    uint8_t source_mac[6], next_hop_mac[6], packet_filter[4];
    if ( !rocketnet_address_parse( &source, options.source_text.c_str() ) ||
         !rocketnet_address_parse( &server, options.server_text.c_str() ) || source.type != server.type )
    {
        fprintf( stderr, "xdp_packet_blaster: source and server must be explicit addresses of one family\n" );
        return 2;
    }
    options.plan.address_family = source.type == ROCKETNET_ADDRESS_IPV6 ? 6 : 4;
    if ( !rn_blaster_plan_valid( &options.plan ) || !rn_blaster_address_private( server ) ||
         server.port == 0 || source.port != options.plan.source_port || !rn_blaster_address_private( source ) )
    {
        fprintf( stderr, "xdp_packet_blaster: source/server must be private or ULA, server port nonzero, and source port must match --source-port\n" );
        return 2;
    }
    if ( options.plan.mode == RN_BLASTER_MODE_ROBUSTNESS && source.type != ROCKETNET_ADDRESS_IPV4 )
    {
        fprintf( stderr, "xdp_packet_blaster: robustness mode is IPv4-only so the exact 1453-byte case stays within MTU 1500\n" );
        return 2;
    }
    if ( !rocketnet_xdp_parse_mac_address( options.source_mac_text.c_str(), source_mac ) ||
         !rocketnet_xdp_parse_mac_address( options.next_hop_mac_text.c_str(), next_hop_mac ) ||
         !rn_blaster_parse_filter( options.packet_filter_text, packet_filter ) )
    {
        fprintf( stderr, "xdp_packet_blaster: malformed MAC or packet filter\n" );
        return 2;
    }
    if ( !rn_blaster_mac_unicast( source_mac ) || !rn_blaster_mac_unicast( next_hop_mac ) ||
         memcmp( source_mac, next_hop_mac, 6 ) == 0 )
    {
        fprintf( stderr, "xdp_packet_blaster: source and next-hop MACs must be distinct nonzero unicast addresses\n" );
        return 2;
    }
    rn_blaster_manifest_t manifest;
    if ( !rn_blaster_manifest_load( options.manifest_file, &manifest, &error ) )
    {
        fprintf( stderr, "xdp_packet_blaster: %s\n", error.c_str() );
        return 2;
    }
    std::vector<rn_blaster_token_t> tokens;
    if ( !rn_blaster_tokens_load( options.token_file, options.plan.clients, &tokens, &error ) )
    {
        fprintf( stderr, "xdp_packet_blaster: %s (token contents are never logged)\n", error.c_str() );
        rn_blaster_tokens_wipe( &tokens );
        return 2;
    }
    if ( !rn_blaster_tokens_name_server( tokens, server ) )
    {
        fprintf( stderr, "xdp_packet_blaster: every token must select the exact explicit server address and port\n" );
        rn_blaster_tokens_wipe( &tokens );
        return 2;
    }
    rn_blaster_print_plan( options, manifest, source, server );
    if ( !options.execute )
    {
        rn_blaster_tokens_wipe( &tokens );
        return 0;
    }
#if defined( __linux__ )
    const int result = rn_blaster_execute( options, manifest, source, server, source_mac, next_hop_mac, packet_filter, tokens );
    rn_blaster_tokens_wipe( &tokens );
    return result;
#else
    rn_blaster_tokens_wipe( &tokens );
    fprintf( stderr, "xdp_packet_blaster: AF_XDP execution is Linux-only\n" );
    return 2;
#endif
}

#if defined( __linux__ )

/* ethtool's hfunc bit for Toeplitz (the kernel's ETH_RSS_HASH_TOP, BIT(0));
 * the uapi header names the field but not its values. */
#define RN_BLASTER_RSS_HASH_TOEPLITZ 1u
#define RN_BLASTER_TRANSMIT_BATCH 64u
#define RN_BLASTER_DRAIN_NANOSECONDS UINT64_C( 500000000 )

struct rn_blaster_xsk_t
{
    void * buffer;
    struct xsk_umem * umem;
    struct xsk_socket * socket;
    struct xsk_ring_prod fill;
    struct xsk_ring_cons completion;
    struct xsk_ring_cons receive;
    struct xsk_ring_prod transmit;
    std::vector<uint64_t> free_transmit;
    std::vector<uint64_t> pending_fill;
    uint32_t pending_transmit;
    uint32_t queue;
    int interface_index;
    bool owns_attach;
    bool zero_copy;
    bool cleanup_empty;
    bool rlimit_changed;
    bool rlimit_restored;
    struct rlimit old_memlock;
    rocketnet_xdp_attached_t before;
    rocketnet_xdp_attached_t active;
    rocketnet_xdp_attached_t after;
};

struct rn_blaster_session_t
{
    rocketnet_client_protocol_t protocol;
    rocketnet_counters_t counters;
    rn_endpoint_t endpoint;
    rocketnet_address_t source;
    bool endpoint_announced;
    uint64_t next_packet_id;
    uint64_t application_sequence;
    uint64_t watched_packet_id;
    bool watched_packet_acked;
    uint64_t payloads_received;
    uint64_t payload_errors;
};

/* What every sender thread reads. Setup writes the plain fields before any
 * thread starts; the atomics are the only state threads share afterwards, and
 * none of them is touched on the send path. */
struct rn_blaster_shared_t
{
    const rn_blaster_options_t * options;
    const rn_blaster_manifest_t * manifest;
    rocketnet_address_t source;
    rocketnet_address_t server;
    uint8_t source_mac[6];
    uint8_t next_hop_mac[6];
    uint8_t packet_filter[4];
    const std::vector<rn_blaster_token_t> * tokens;
    std::vector<uint16_t> ports;
    std::vector<uint8_t> port_owner;
    uint64_t start_ns;
    std::atomic<uint32_t> create_turn;
    std::atomic<uint32_t> ready;
    std::atomic<uint64_t> workload_start_ns;
    std::atomic<bool> failed;
    std::atomic<bool> zero_copy;
};

struct rn_blaster_worker_t
{
    uint32_t index;
    uint32_t queue;
    int cpu;
    int running_cpu;
    uint32_t first_client;
    uint32_t clients;
    rn_blaster_xsk_t xsk;
    bool opened;
    std::string error;
    rn_blaster_results_t results;
    rn_blaster_results_t disconnect_results;
    bool sessions_ok;
    bool all_sessions_ready;
    bool schedule_complete;
    bool robustness_complete;
    uint64_t offer_ordinal;
    uint64_t total_offers;
    uint64_t workload_ns;
};

static bool rn_blaster_now_nanoseconds( uint64_t * nanoseconds )
{
    struct timespec time_value;
    if ( !nanoseconds || clock_gettime( CLOCK_MONOTONIC, &time_value ) != 0 )
    {
        return false;
    }
    *nanoseconds = (uint64_t) time_value.tv_sec * UINT64_C( 1000000000 ) + (uint64_t) time_value.tv_nsec;
    return true;
}

static bool rn_blaster_source_on_interface( const std::string & interface_name, const rocketnet_address_t & source )
{
    struct ifaddrs * addresses = NULL;
    if ( getifaddrs( &addresses ) != 0 )
    {
        return false;
    }
    bool found = false;
    for ( struct ifaddrs * item = addresses; item != NULL && !found; item = item->ifa_next )
    {
        if ( !item->ifa_addr || interface_name != item->ifa_name )
        {
            continue;
        }
        if ( source.type == ROCKETNET_ADDRESS_IPV4 && item->ifa_addr->sa_family == AF_INET )
        {
            const struct sockaddr_in * address = (const struct sockaddr_in *) item->ifa_addr;
            found = memcmp( &address->sin_addr, source.data.ipv4, 4 ) == 0;
        }
        else if ( source.type == ROCKETNET_ADDRESS_IPV6 && item->ifa_addr->sa_family == AF_INET6 )
        {
            const struct sockaddr_in6 * address = (const struct sockaddr_in6 *) item->ifa_addr;
            uint8_t bytes[16];
            for ( int i = 0; i < 8; ++i )
            {
                bytes[2 * i] = (uint8_t) ( source.data.ipv6[i] >> 8 );
                bytes[2 * i + 1] = (uint8_t) source.data.ipv6[i];
            }
            found = memcmp( &address->sin6_addr, bytes, sizeof( bytes ) ) == 0;
        }
    }
    freeifaddrs( addresses );
    return found;
}

static bool rn_blaster_mac_on_interface( const std::string & interface_name, const uint8_t source_mac[6] )
{
    const int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd < 0 )
    {
        return false;
    }
    struct ifreq request;
    memset( &request, 0, sizeof( request ) );
    strncpy( request.ifr_name, interface_name.c_str(), sizeof( request.ifr_name ) - 1u );
    const bool matches = ioctl( fd, SIOCGIFHWADDR, &request ) == 0 &&
                         memcmp( request.ifr_hwaddr.sa_data, source_mac, 6 ) == 0;
    close( fd );
    return matches;
}

static bool rn_blaster_ethtool( const std::string & interface_name, void * data )
{
    const int fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( fd < 0 )
    {
        return false;
    }
    struct ifreq request;
    memset( &request, 0, sizeof( request ) );
    strncpy( request.ifr_name, interface_name.c_str(), sizeof( request.ifr_name ) - 1u );
    request.ifr_data = (char *) data;
    const bool ok = ioctl( fd, SIOCETHTOOL, &request ) == 0;
    close( fd );
    return ok;
}

static bool rn_blaster_interface_channels( const std::string & interface_name, struct ethtool_channels * channels )
{
    memset( channels, 0, sizeof( *channels ) );
    channels->cmd = ETHTOOL_GCHANNELS;
    return rn_blaster_ethtool( interface_name, channels );
}

static bool rn_blaster_interface_one_queue( const std::string & interface_name )
{
    struct ethtool_channels channels;
    const bool queried = rn_blaster_interface_channels( interface_name, &channels );
    const bool one_combined = channels.combined_count == 1u && channels.rx_count == 0u && channels.tx_count == 0u;
    const bool split_pair = channels.combined_count == 0u && channels.rx_count == 1u && channels.tx_count == 1u;
    return queried && ( one_combined || split_pair );
}

/* A wide run needs a receive and a transmit queue per thread. */
static bool rn_blaster_interface_queues( const std::string & interface_name, uint32_t threads )
{
    struct ethtool_channels channels;
    if ( !rn_blaster_interface_channels( interface_name, &channels ) )
    {
        return false;
    }
    return channels.combined_count + channels.rx_count >= threads && channels.combined_count + channels.tx_count >= threads;
}

/* The NIC's RSS for UDP must hash both ports, or every reply from the one
 * server address lands on one queue whatever source port a client uses. */
static bool rn_blaster_rss_hashes_ports( const std::string & interface_name, int family )
{
    struct ethtool_rxnfc flow;
    memset( &flow, 0, sizeof( flow ) );
    flow.cmd = ETHTOOL_GRXFH;
    flow.flow_type = family == ROCKETNET_ADDRESS_IPV6 ? UDP_V6_FLOW : UDP_V4_FLOW;
    if ( !rn_blaster_ethtool( interface_name, &flow ) )
    {
        return false;
    }
    const uint64_t wanted = RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3;
    return ( flow.data & wanted ) == wanted;
}

/* The RSS key and indirection table, read from the NIC. Toeplitz only. */
static bool rn_blaster_rss_read( const std::string & interface_name, std::vector<uint8_t> * key, std::vector<uint32_t> * indirection,
                                 std::string * error )
{
    struct ethtool_rxfh sizes;
    memset( &sizes, 0, sizeof( sizes ) );
    sizes.cmd = ETHTOOL_GRSSH;
    if ( !rn_blaster_ethtool( interface_name, &sizes ) )
    {
        *error = "cannot read the RSS key and indirection table (ETHTOOL_GRSSH)";
        return false;
    }
    if ( sizes.key_size == 0 || sizes.key_size > RN_BLASTER_MAX_RSS_KEY_BYTES || sizes.indir_size == 0 ||
         sizes.indir_size > RN_BLASTER_MAX_RSS_INDIRECTION )
    {
        *error = "the RSS key or indirection table size is outside what the tool can model";
        return false;
    }
    const size_t bytes = sizeof( struct ethtool_rxfh ) + sizes.indir_size * sizeof( uint32_t ) + sizes.key_size;
    std::vector<uint64_t> storage( ( bytes + sizeof( uint64_t ) - 1u ) / sizeof( uint64_t ), 0 );
    struct ethtool_rxfh * table = (struct ethtool_rxfh *) storage.data();
    table->cmd = ETHTOOL_GRSSH;
    table->indir_size = sizes.indir_size;
    table->key_size = sizes.key_size;
    if ( !rn_blaster_ethtool( interface_name, table ) )
    {
        *error = "cannot read the RSS key and indirection table (ETHTOOL_GRSSH)";
        return false;
    }
    if ( table->hfunc != 0 && ( table->hfunc & RN_BLASTER_RSS_HASH_TOEPLITZ ) == 0 )
    {
        *error = "the NIC's RSS hash function is not Toeplitz";
        return false;
    }
    indirection->assign( table->rss_config, table->rss_config + table->indir_size );
    const uint8_t * key_bytes = (const uint8_t *) ( table->rss_config + table->indir_size );
    key->assign( key_bytes, key_bytes + table->key_size );
    return true;
}

static void rn_blaster_print_host( const std::string & interface_name )
{
    struct utsname system_name;
    memset( &system_name, 0, sizeof( system_name ) );
    if ( uname( &system_name ) != 0 )
    {
        strcpy( system_name.release, "unknown" );
    }
    char driver[sizeof( ( (struct ethtool_drvinfo *) 0 )->driver )] = "unknown";
    struct ethtool_drvinfo info;
    memset( &info, 0, sizeof( info ) );
    info.cmd = ETHTOOL_GDRVINFO;
    if ( rn_blaster_ethtool( interface_name, &info ) )
    {
        memcpy( driver, info.driver, sizeof( driver ) );
        driver[sizeof( driver ) - 1u] = '\0';
    }
    printf( "RUNTIME kernel=%s interface=%s driver=%s\n", system_name.release, interface_name.c_str(), driver );
}

static bool rn_blaster_xdp_query( int interface_index, rocketnet_xdp_attached_t * attached )
{
    memset( attached, 0, sizeof( *attached ) );
    return rocketnet_xdp_query_attached( interface_index, attached ) == 0;
}

static bool rn_blaster_xdp_empty( int interface_index )
{
    rocketnet_xdp_attached_t attached = {};
    return rn_blaster_xdp_query( interface_index, &attached ) && !rocketnet_xdp_attached_any( &attached );
}

static void rn_blaster_print_xdp_ids( const char * phase, const rocketnet_xdp_attached_t & attached )
{
    printf( "RUNTIME xdp_%s_ids=prog:%u,drv:%u,skb:%u,hw:%u\n", phase, attached.prog_id,
            attached.drv_prog_id, attached.skb_prog_id, attached.hw_prog_id );
}

/* Only the socket that made the attachment judges the interface clean and
 * restores RLIMIT_MEMLOCK; it is closed after every other socket. */
static void rn_blaster_xsk_close( rn_blaster_xsk_t * xsk )
{
    if ( xsk->socket )
    {
        xsk_socket__delete( xsk->socket );
        xsk->socket = NULL;
    }
    if ( xsk->umem )
    {
        xsk_umem__delete( xsk->umem );
        xsk->umem = NULL;
    }
    if ( xsk->buffer )
    {
        free( xsk->buffer );
        xsk->buffer = NULL;
    }
    xsk->cleanup_empty = !xsk->owns_attach || xsk->interface_index <= 0 ||
                         ( rn_blaster_xdp_query( xsk->interface_index, &xsk->after ) && !rocketnet_xdp_attached_any( &xsk->after ) );
    if ( xsk->rlimit_changed )
    {
        xsk->rlimit_restored = setrlimit( RLIMIT_MEMLOCK, &xsk->old_memlock ) == 0;
        xsk->rlimit_changed = false;
    }
}

static bool rn_blaster_xsk_attempt( rn_blaster_xsk_t * xsk, const rn_blaster_options_t & options, bool zero_copy, std::string * error )
{
    const size_t buffer_bytes = (size_t) RN_BLASTER_UMEM_FRAMES * RN_BLASTER_FRAME_BYTES;
    if ( posix_memalign( &xsk->buffer, (size_t) getpagesize(), buffer_bytes ) != 0 )
    {
        xsk->buffer = NULL;
        *error = "UMEM allocation failed";
        return false;
    }
    /* first touch: the calling thread is already on its CPU, so the pages land on its node */
    memset( xsk->buffer, 0, buffer_bytes );
    struct xsk_umem_config umem_config = {};
    umem_config.fill_size = RN_BLASTER_FILL_RING_SIZE;
    umem_config.comp_size = RN_BLASTER_COMPLETION_RING_SIZE;
    umem_config.frame_size = RN_BLASTER_FRAME_BYTES;
    if ( xsk_umem__create( &xsk->umem, xsk->buffer, buffer_bytes, &xsk->fill, &xsk->completion, &umem_config ) != 0 )
    {
        *error = "xsk_umem__create refused";
        return false;
    }
    struct xsk_socket_config socket_config = {};
    socket_config.rx_size = RN_BLASTER_RX_RING_SIZE;
    socket_config.tx_size = RN_BLASTER_TX_RING_SIZE;
    socket_config.libbpf_flags = 0;
    socket_config.xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST |
                              ( options.xdp_mode == "skb" ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE );
    socket_config.bind_flags = (uint16_t) ( XDP_USE_NEED_WAKEUP | ( zero_copy ? XDP_ZEROCOPY : XDP_COPY ) );
    const int create_result = xsk_socket__create( &xsk->socket, options.interface_name.c_str(), xsk->queue,
                                                  xsk->umem, &xsk->receive, &xsk->transmit, &socket_config );
    if ( create_result != 0 )
    {
        xsk->socket = NULL;
        *error = std::string( zero_copy ? "zero-copy" : "copy" ) + " AF_XDP bind refused: " + strerror( -create_result );
        return false;
    }
    if ( !rn_blaster_xdp_query( xsk->interface_index, &xsk->active ) || !rocketnet_xdp_attached_any( &xsk->active ) )
    {
        *error = "AF_XDP socket did not produce a queryable generic XDP attachment";
        return false;
    }
    struct xdp_options actual = {};
    socklen_t actual_bytes = sizeof( actual );
    if ( getsockopt( xsk_socket__fd( xsk->socket ), SOL_XDP, XDP_OPTIONS, &actual, &actual_bytes ) != 0 )
    {
        *error = "cannot read selected AF_XDP bind mode";
        return false;
    }
    xsk->zero_copy = ( actual.flags & XDP_OPTIONS_ZEROCOPY ) != 0;
    if ( zero_copy != xsk->zero_copy )
    {
        *error = "kernel selected a bind mode different from the requested attempt";
        return false;
    }
    uint32_t fill_index = 0;
    if ( xsk_ring_prod__reserve( &xsk->fill, RN_BLASTER_RX_FRAMES, &fill_index ) != (int) RN_BLASTER_RX_FRAMES )
    {
        *error = "cannot populate AF_XDP fill ring";
        return false;
    }
    for ( uint32_t i = 0; i < RN_BLASTER_RX_FRAMES; ++i )
    {
        *xsk_ring_prod__fill_addr( &xsk->fill, fill_index + i ) = (uint64_t) ( RN_BLASTER_RX_FRAMES + i ) * RN_BLASTER_FRAME_BYTES;
    }
    xsk_ring_prod__submit( &xsk->fill, RN_BLASTER_RX_FRAMES );
    xsk->free_transmit.reserve( RN_BLASTER_RX_FRAMES );
    xsk->pending_fill.reserve( RN_BLASTER_RX_FRAMES );
    for ( uint32_t i = 0; i < RN_BLASTER_RX_FRAMES; ++i )
    {
        xsk->free_transmit.push_back( (uint64_t) i * RN_BLASTER_FRAME_BYTES );
    }
    return true;
}

static void rn_blaster_xsk_reset( rn_blaster_xsk_t * xsk, const rn_blaster_options_t & options, uint32_t queue, bool owns_attach )
{
    *xsk = rn_blaster_xsk_t();
    xsk->rlimit_restored = true;
    xsk->queue = queue;
    xsk->owns_attach = owns_attach;
    xsk->interface_index = (int) if_nametoindex( options.interface_name.c_str() );
}

/* The first socket refuses an occupied interface, raises RLIMIT_MEMLOCK and
 * may fall back from zero-copy to copy; its attachment then carries every
 * later socket, which binds in the mode the first one got. */
static bool rn_blaster_xsk_open_first( rn_blaster_xsk_t * xsk, const rn_blaster_options_t & options, uint32_t queue, std::string * error )
{
    rn_blaster_xsk_reset( xsk, options, queue, true );
    if ( xsk->interface_index <= 0 )
    {
        *error = "interface does not exist";
        return false;
    }
    if ( !rn_blaster_xdp_query( xsk->interface_index, &xsk->before ) || rocketnet_xdp_attached_any( &xsk->before ) )
    {
        *error = "interface already has an XDP program; refusing to replace it";
        return false;
    }
    if ( getrlimit( RLIMIT_MEMLOCK, &xsk->old_memlock ) != 0 )
    {
        *error = "cannot read RLIMIT_MEMLOCK";
        return false;
    }
    struct rlimit raised = { RLIM_INFINITY, RLIM_INFINITY };
    if ( setrlimit( RLIMIT_MEMLOCK, &raised ) != 0 )
    {
        *error = "cannot raise RLIMIT_MEMLOCK";
        return false;
    }
    xsk->rlimit_changed = true;
    const bool first_zero_copy = options.plan.bind != RN_BLASTER_BIND_COPY;
    if ( rn_blaster_xsk_attempt( xsk, options, first_zero_copy, error ) )
    {
        rn_blaster_print_xdp_ids( "before", xsk->before );
        rn_blaster_print_xdp_ids( "active", xsk->active );
        return true;
    }
    const std::string first_error = *error;
    rn_blaster_xsk_close( xsk );
    if ( !xsk->rlimit_restored )
    {
        *error = first_error + "; failed attempt could not restore RLIMIT_MEMLOCK";
        return false;
    }
    if ( !xsk->cleanup_empty )
    {
        *error = first_error + "; failed attempt left an XDP attachment; refusing blind detach";
        return false;
    }
    rn_blaster_xsk_reset( xsk, options, queue, true );
    if ( options.plan.bind != RN_BLASTER_BIND_AUTO )
    {
        return false;
    }
    if ( !rn_blaster_xdp_empty( xsk->interface_index ) )
    {
        *error = "zero-copy fallback cleanup left an XDP attachment";
        return false;
    }
    if ( getrlimit( RLIMIT_MEMLOCK, &xsk->old_memlock ) != 0 || setrlimit( RLIMIT_MEMLOCK, &raised ) != 0 )
    {
        *error = "cannot prepare RLIMIT_MEMLOCK for copy fallback";
        return false;
    }
    xsk->rlimit_changed = true;
    if ( !rn_blaster_xsk_attempt( xsk, options, false, error ) )
    {
        *error = first_error + "; copy fallback: " + *error;
        return false;
    }
    rn_blaster_print_xdp_ids( "before", xsk->before );
    rn_blaster_print_xdp_ids( "active", xsk->active );
    printf( "RUNTIME fallback=zero-copy-to-copy reason=%s\n", first_error.c_str() );
    return true;
}

static bool rn_blaster_xsk_open_more( rn_blaster_xsk_t * xsk, const rn_blaster_options_t & options, uint32_t queue,
                                      bool zero_copy, std::string * error )
{
    rn_blaster_xsk_reset( xsk, options, queue, false );
    if ( xsk->interface_index <= 0 )
    {
        *error = "interface does not exist";
        return false;
    }
    return rn_blaster_xsk_attempt( xsk, options, zero_copy, error );
}

static void rn_blaster_complete( rn_blaster_xsk_t * xsk, rn_blaster_results_t * results )
{
    uint32_t index = 0;
    const uint32_t count = xsk_ring_cons__peek( &xsk->completion, RN_BLASTER_COMPLETION_RING_SIZE, &index );
    for ( uint32_t i = 0; i < count; ++i )
    {
        const uint64_t address = *xsk_ring_cons__comp_addr( &xsk->completion, index + i );
        if ( address < (uint64_t) RN_BLASTER_RX_FRAMES * RN_BLASTER_FRAME_BYTES &&
             address % RN_BLASTER_FRAME_BYTES == 0 )
        {
            xsk->free_transmit.push_back( address );
        }
        else
        {
            ++results->ownership_errors;
        }
    }
    if ( count > 0 )
    {
        xsk_ring_cons__release( &xsk->completion, count );
    }
}

/* Publish every reserved descriptor at once, with at most one wakeup. */
static void rn_blaster_flush( rn_blaster_xsk_t * xsk, rn_blaster_results_t * results )
{
    if ( xsk->pending_transmit == 0 )
    {
        return;
    }
    xsk_ring_prod__submit( &xsk->transmit, xsk->pending_transmit );
    xsk->pending_transmit = 0;
    ++results->transmit_batches;
    if ( xsk_ring_prod__needs_wakeup( &xsk->transmit ) )
    {
        ++results->transmit_wakeups;
        (void) sendto( xsk_socket__fd( xsk->socket ), NULL, 0, MSG_DONTWAIT, NULL, 0 );
    }
}

static bool rn_blaster_take_frame( rn_blaster_xsk_t * xsk, rn_blaster_results_t * results, uint64_t * address )
{
    if ( xsk->free_transmit.empty() )
    {
        rn_blaster_complete( xsk, results );
    }
    if ( xsk->free_transmit.empty() )
    {
        return false;
    }
    *address = xsk->free_transmit.back();
    xsk->free_transmit.pop_back();
    return true;
}

static bool rn_blaster_queue_frame( rn_blaster_xsk_t * xsk, uint64_t address, int frame_bytes, int packet_bytes,
                                    rn_blaster_results_t * results )
{
    uint32_t index = 0;
    if ( xsk_ring_prod__reserve( &xsk->transmit, 1, &index ) != 1 )
    {
        xsk->free_transmit.push_back( address );
        ++results->backpressure;
        return false;
    }
    struct xdp_desc * descriptor = xsk_ring_prod__tx_desc( &xsk->transmit, index );
    descriptor->addr = address;
    descriptor->len = (uint32_t) frame_bytes;
    ++xsk->pending_transmit;
    if ( xsk->pending_transmit == RN_BLASTER_TRANSMIT_BATCH )
    {
        rn_blaster_flush( xsk, results );
    }
    ++results->submitted;
    results->protocol_bytes += (uint64_t) packet_bytes;
    results->ethernet_frame_bytes += (uint64_t) frame_bytes;
    results->physical_wire_bytes += (uint64_t) rn_blaster_physical_wire_bytes( (uint32_t) frame_bytes );
    return true;
}

static bool rn_blaster_send( rn_blaster_xsk_t * xsk, const uint8_t * packet, int packet_bytes,
                             const rocketnet_address_t & source, const rocketnet_address_t & server,
                             const uint8_t source_mac[6], const uint8_t next_hop_mac[6], rn_blaster_results_t * results )
{
    uint64_t address = 0;
    if ( !rn_blaster_take_frame( xsk, results, &address ) )
    {
        ++results->backpressure;
        return false;
    }
    uint8_t * frame = (uint8_t *) xsk->buffer + address;
    uint64_t refused = 0;
    const int copied = rocketnet_xdp_send_copy( frame, RN_BLASTER_FRAME_BYTES, source.type, packet, (size_t) packet_bytes, &refused );
    const int frame_bytes = copied > 0 ? rocketnet_xdp_frame_build( frame, RN_BLASTER_FRAME_BYTES, source_mac, next_hop_mac,
                                                                    &source, &server, copied )
                                       : 0;
    if ( frame_bytes <= 0 )
    {
        xsk->free_transmit.push_back( address );
        ++results->backpressure;
        return false;
    }
    return rn_blaster_queue_frame( xsk, address, frame_bytes, packet_bytes, results );
}

/* The one intentional packet beyond RocketNet's receive ceiling. IPv4 still
 * fits a 1500-byte IP packet; production traffic continues through
 * rocketnet_xdp_send_copy and cannot enter this path. */
static bool rn_blaster_send_robustness( rn_blaster_xsk_t * xsk, const uint8_t * packet, int packet_bytes,
                                        const rocketnet_address_t & source, const rocketnet_address_t & server,
                                        const uint8_t source_mac[6], const uint8_t next_hop_mac[6],
                                        rn_blaster_results_t * results )
{
    if ( packet_bytes <= ROCKETNET_RECEIVE_CEILING )
    {
        return rn_blaster_send( xsk, packet, packet_bytes, source, server, source_mac, next_hop_mac, results );
    }
    if ( packet_bytes != ROCKETNET_RECEIVE_CEILING + 1 || source.type != ROCKETNET_ADDRESS_IPV4 )
    {
        return false;
    }
    uint64_t address = 0;
    if ( !rn_blaster_take_frame( xsk, results, &address ) )
    {
        ++results->backpressure;
        return false;
    }
    uint8_t * frame = (uint8_t *) xsk->buffer + address;
    const int header_bytes = rocketnet_xdp_frame_header_bytes( source.type );
    if ( header_bytes <= 0 || (size_t) header_bytes + (size_t) packet_bytes > RN_BLASTER_FRAME_BYTES )
    {
        xsk->free_transmit.push_back( address );
        return false;
    }
    memcpy( frame + header_bytes, packet, (size_t) packet_bytes );
    int frame_bytes = rocketnet_xdp_frame_build( frame, RN_BLASTER_FRAME_BYTES, source_mac, next_hop_mac,
                                                 &source, &server, ROCKETNET_RECEIVE_CEILING );
    if ( frame_bytes <= 0 )
    {
        xsk->free_transmit.push_back( address );
        return false;
    }
    rocketnet_xdp_frame_write_be16( frame + 16,
                                    (uint16_t) ( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES + packet_bytes ) );
    frame[24] = 0;
    frame[25] = 0;
    rocketnet_xdp_frame_write_be16( frame + 24,
                                    rocketnet_xdp_frame_checksum( frame + 14, ROCKETNET_IPV4_HEADER_BYTES ) );
    rocketnet_xdp_frame_write_be16( frame + header_bytes - 4,
                                    (uint16_t) ( ROCKETNET_UDP_HEADER_BYTES + packet_bytes ) );
    ++frame_bytes;
    return rn_blaster_queue_frame( xsk, address, frame_bytes, packet_bytes, results );
}

static uint16_t rn_blaster_frame_destination_port( const uint8_t * frame, int frame_bytes, int family )
{
    const int header = family == ROCKETNET_ADDRESS_IPV6 ? 14 + 40 + 8 : 14 + 20 + 8;
    if ( frame_bytes < header )
    {
        return 0;
    }
    return (uint16_t) ( ( (uint16_t) frame[header - 6] << 8 ) | frame[header - 5] );
}

static void rn_blaster_fill_returned( rn_blaster_xsk_t * xsk )
{
    if ( xsk->pending_fill.empty() )
    {
        return;
    }
    uint32_t index = 0;
    const uint32_t wanted = (uint32_t) xsk->pending_fill.size();
    const uint32_t reserved = xsk_ring_prod__reserve( &xsk->fill, wanted, &index );
    if ( reserved == 0 )
    {
        return;
    }
    for ( uint32_t i = 0; i < reserved; ++i )
    {
        *xsk_ring_prod__fill_addr( &xsk->fill, index + i ) = xsk->pending_fill.back();
        xsk->pending_fill.pop_back();
    }
    if ( reserved > 0 )
    {
        xsk_ring_prod__submit( &xsk->fill, reserved );
    }
}

/* Receive for one thread. port_owner names the thread that owns each client
 * port and session_of_port that thread's session index; a reply for another
 * thread's client means RSS steered it here: counted, never processed. */
static void rn_blaster_receive( rn_blaster_xsk_t * xsk, std::vector<rn_blaster_session_t> * sessions,
                                const rocketnet_address_t & server, const std::vector<uint8_t> & port_owner,
                                const std::vector<uint16_t> & session_of_port, uint32_t thread,
                                uint32_t expected_payload_bytes, double now, rn_blaster_results_t * results )
{
    uint32_t index = 0;
    const uint32_t count = xsk_ring_cons__peek( &xsk->receive, 256, &index );
    if ( count == 0 )
    {
        rn_blaster_fill_returned( xsk );
        if ( xsk_ring_prod__needs_wakeup( &xsk->fill ) )
        {
            (void) recvfrom( xsk_socket__fd( xsk->socket ), NULL, 0, MSG_DONTWAIT, NULL, NULL );
        }
        return;
    }
    for ( uint32_t i = 0; i < count; ++i )
    {
        const struct xdp_desc * descriptor = xsk_ring_cons__rx_desc( &xsk->receive, index + i );
        const uint64_t receive_begin = (uint64_t) RN_BLASTER_RX_FRAMES * RN_BLASTER_FRAME_BYTES;
        const uint64_t receive_end = (uint64_t) RN_BLASTER_UMEM_FRAMES * RN_BLASTER_FRAME_BYTES;
        uint64_t frame_address = 0;
        if ( rocketnet_xdp_fill_return( descriptor->addr, receive_begin, receive_end, &frame_address ) &&
             xsk->pending_fill.size() < RN_BLASTER_RX_FRAMES )
        {
            xsk->pending_fill.push_back( frame_address );
        }
        else
        {
            ++results->ownership_errors;
        }
        rocketnet_xdp_receive_drops_t descriptor_drops = {};
        const uint8_t * frame = rocketnet_xdp_receive_descriptor( xsk->buffer, receive_begin, receive_end,
                                                                  descriptor->addr, descriptor->len,
                                                                  &descriptor_drops );
        if ( !frame )
        {
            results->descriptor_refused += descriptor_drops.descriptor;
            continue;
        }
        rocketnet_address_t from = {};
        const uint8_t * packet = NULL;
        int packet_bytes = 0;
        ++results->received_frames;
        if ( !rocketnet_xdp_frame_parse( frame, descriptor->len, &from, &packet, &packet_bytes ) ||
             !rocketnet_address_equal( &from, &server ) )
        {
            ++results->receive_refused;
            continue;
        }
        const uint16_t destination_port = rn_blaster_frame_destination_port( frame, (int) descriptor->len, from.type );
        const uint8_t owner = port_owner[destination_port];
        if ( owner == RN_BLASTER_PORT_UNOWNED )
        {
            ++results->receive_refused;
            continue;
        }
        if ( owner != thread )
        {
            ++results->receive_wrong_thread;
            continue;
        }
        rn_blaster_session_t & session = ( *sessions )[session_of_port[destination_port]];
        uint8_t payload[ROCKETNET_RECEIVE_CEILING];
        int payload_bytes = 0;
        if ( rocketnet_client_protocol_process_packet( &session.protocol, packet, packet_bytes, now,
                                                       payload, sizeof( payload ), &payload_bytes ) )
        {
            ++results->received_protocol;
            for ( int ack = 0; ack < session.protocol.num_acked; ++ack )
            {
                rn_endpoint_on_acked( &session.endpoint, session.protocol.acked_packet_ids[ack], now );
                if ( session.protocol.acked_packet_ids[ack] == session.watched_packet_id )
                {
                    session.watched_packet_acked = true;
                }
            }
            session.protocol.num_acked = 0;
            if ( payload_bytes > 0 )
            {
                rn_delivery_t deliveries[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];
                int delivery_count = 0;
                if ( rn_endpoint_on_payload( &session.endpoint, payload, payload_bytes,
                                             deliveries, (int) ( sizeof( deliveries ) / sizeof( deliveries[0] ) ),
                                             &delivery_count, now ) )
                {
                    rocketnet_client_protocol_accept_payload( &session.protocol, session.protocol.received_session_sequence );
                    for ( int d = 0; d < delivery_count; ++d )
                    {
                        ++session.payloads_received;
                        ++results->received_application;
                        results->application_received_bytes += (uint64_t) deliveries[d].bytes;
                        if ( deliveries[d].bytes != (int) expected_payload_bytes )
                        {
                            ++session.payload_errors;
                            ++results->payload_errors;
                        }
                        if ( deliveries[d].bytes >= 4 )
                        {
                            const uint8_t * cursor = deliveries[d].data;
                            if ( rocketnet_read_uint32( &cursor ) != RN_BLASTER_PAYLOAD_MAGIC )
                            {
                                ++session.payload_errors;
                                ++results->payload_errors;
                            }
                        }
                    }
                }
                else
                {
                    ++session.payload_errors;
                    ++results->payload_errors;
                    ++results->receive_refused;
                }
            }
        }
        else
        {
            ++results->receive_refused;
        }
    }
    xsk_ring_cons__release( &xsk->receive, count );
    rn_blaster_fill_returned( xsk );
}

static void rn_blaster_application_message( uint8_t * body, uint32_t bytes,
                                            uint32_t client, uint64_t sequence, uint64_t seed )
{
    uint32_t state = (uint32_t) rn_blaster_mix64( seed ^ sequence ^ client );
    for ( uint32_t i = 0; i < bytes; ++i )
    {
        state = state * UINT32_C( 1664525 ) + UINT32_C( 1013904223 );
        body[i] = (uint8_t) ( state >> 24 );
    }
    if ( bytes >= 4 )
    {
        uint8_t * cursor = body;
        rocketnet_write_uint32( &cursor, RN_BLASTER_PAYLOAD_MAGIC );
    }
    if ( bytes >= 8 )
    {
        uint8_t * cursor = body + 4;
        rocketnet_write_uint32( &cursor, client );
    }
}

static bool rn_blaster_endpoint_prepare( rn_blaster_session_t * session, double now,
                                         std::array<uint8_t, ROCKETNET_SEND_BUDGET> * packet,
                                         int * packet_bytes, uint64_t * packet_id )
{
    const int requested = rn_blaster_endpoint_request( &session->endpoint, now );
    if ( requested == 0 )
    {
        return false;
    }
    *packet_id = session->next_packet_id++;
    uint64_t sequence = 0;
    if ( !rocketnet_client_protocol_start_payload( &session->protocol, &sequence ) )
    {
        rn_endpoint_packet_failed( &session->endpoint, *packet_id, now );
        return false;
    }
    uint8_t * payload = packet->data() + ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
    const int payload_bytes = rn_endpoint_write_packet( &session->endpoint, payload, requested, *packet_id, now );
    if ( payload_bytes <= 0 )
    {
        rn_endpoint_packet_failed( &session->endpoint, *packet_id, now );
        return false;
    }
    *packet_bytes = rocketnet_client_protocol_seal_payload_frame( &session->protocol, sequence, *packet_id,
                                                                  packet->data(), payload_bytes, now );
    if ( *packet_bytes <= 0 )
    {
        rn_endpoint_packet_failed( &session->endpoint, *packet_id, now );
        return false;
    }
    return true;
}

static bool rn_blaster_endpoint_packet( rn_blaster_xsk_t * xsk, rn_blaster_session_t * session, double now,
                                        const rocketnet_address_t & server, const uint8_t source_mac[6],
                                        const uint8_t next_hop_mac[6], rn_blaster_results_t * results,
                                        std::array<uint8_t, ROCKETNET_SEND_BUDGET> * last_packet, int * last_packet_bytes,
                                        uint64_t * sent_packet_id )
{
    uint64_t packet_id = 0;
    if ( !rn_blaster_endpoint_prepare( session, now, last_packet, last_packet_bytes, &packet_id ) )
    {
        return false;
    }
    if ( !rn_blaster_send( xsk, last_packet->data(), *last_packet_bytes, session->source, server,
                           source_mac, next_hop_mac, results ) )
    {
        rn_endpoint_packet_failed( &session->endpoint, packet_id, now );
        return false;
    }
    rn_endpoint_packet_sent( &session->endpoint, packet_id, *last_packet_bytes, now );
    if ( sent_packet_id )
    {
        *sent_packet_id = packet_id;
    }
    return true;
}

/* A disconnect burst waits a bounded time for completions instead of being
 * dropped when the transmit ring is full at the end of a run. */
static void rn_blaster_send_drained( rn_blaster_xsk_t * xsk, const uint8_t * packet, int packet_bytes,
                                     const rocketnet_address_t & source, const rocketnet_address_t & server,
                                     const uint8_t source_mac[6], const uint8_t next_hop_mac[6], uint64_t deadline_ns,
                                     rn_blaster_results_t * results )
{
    while ( true )
    {
        rn_blaster_results_t attempt = {};
        if ( rn_blaster_send( xsk, packet, packet_bytes, source, server, source_mac, next_hop_mac, &attempt ) )
        {
            rn_blaster_results_add( results, &attempt );
            return;
        }
        uint64_t now_ns = 0;
        if ( !rn_blaster_now_nanoseconds( &now_ns ) || now_ns >= deadline_ns )
        {
            rn_blaster_results_add( results, &attempt );
            return;
        }
        rn_blaster_flush( xsk, results );
        rn_blaster_complete( xsk, results );
        sched_yield();
    }
}

static bool rn_blaster_pin( int cpu )
{
    cpu_set_t set;
    CPU_ZERO( &set );
    CPU_SET( (size_t) cpu, &set );
    return sched_setaffinity( 0, sizeof( set ), &set ) == 0;
}

/* One sender thread: its own queue, socket, UMEM, arena and clients. */
static void rn_blaster_worker_run( rn_blaster_shared_t * shared, rn_blaster_worker_t * worker )
{
    const rn_blaster_options_t & options = *shared->options;
    const rn_blaster_manifest_t & manifest = *shared->manifest;
    const rocketnet_address_t & server = shared->server;
    const uint32_t threads = options.plan.threads;
    worker->sessions_ok = true;
    if ( worker->cpu >= 0 && !rn_blaster_pin( worker->cpu ) )
    {
        worker->error = "cannot pin the sender thread to its CPU";
        worker->sessions_ok = false;
        shared->failed.store( true );
    }
    worker->running_cpu = sched_getcpu();

    while ( shared->create_turn.load() != worker->index && !shared->failed.load() )
    {
        sched_yield();
    }
    if ( !shared->failed.load() )
    {
        if ( worker->index == 0 )
        {
            worker->opened = rn_blaster_xsk_open_first( &worker->xsk, options, worker->queue, &worker->error );
            shared->zero_copy.store( worker->opened && worker->xsk.zero_copy );
        }
        else
        {
            worker->opened = rn_blaster_xsk_open_more( &worker->xsk, options, worker->queue, shared->zero_copy.load(), &worker->error );
        }
        if ( !worker->opened )
        {
            worker->sessions_ok = false;
            shared->failed.store( true );
        }
    }
    shared->create_turn.store( worker->index + 1u );

    rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    const uint64_t arena_required = worker->sessions_ok ? rn_arena_required_bytes( &message_config, (int) worker->clients ) : 0;
    if ( worker->sessions_ok && arena_required == 0 )
    {
        worker->error = "endpoint arena memory plan is invalid";
        worker->sessions_ok = false;
        shared->failed.store( true );
    }
    rocketnet_counters_t message_counters = {};
    std::vector<uint8_t> message_arena_storage( worker->sessions_ok ? (size_t) arena_required + ROCKETNET_ALLOCATION_ALIGNMENT : 0u );
    uint8_t * message_arena_block = (uint8_t *) ( ( (uintptr_t) message_arena_storage.data() + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) &
                                                  ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
    rn_arena_t * message_arena = worker->sessions_ok ? rn_arena_create_in_block( message_arena_block, arena_required, &message_config,
                                                                                 (int) worker->clients, &message_counters )
                                                     : NULL;
    if ( worker->sessions_ok && !message_arena )
    {
        worker->error = "endpoint arena creation failed";
        worker->sessions_ok = false;
        shared->failed.store( true );
    }
    std::vector<rn_blaster_session_t> sessions( worker->clients );
    std::vector<uint16_t> session_of_port( 65536u, 0 );
    uint32_t initialized_sessions = 0;
    const time_t wall_time = time( NULL );
    const uint64_t unix_time = wall_time >= 0 ? (uint64_t) wall_time : 0;
    if ( wall_time < 0 )
    {
        worker->sessions_ok = false;
    }
    for ( uint32_t i = 0; i < worker->clients && worker->sessions_ok; ++i )
    {
        const uint32_t client = worker->first_client + i;
        rn_blaster_session_t & session = sessions[i];
        memset( &session, 0, sizeof( session ) );
        rocketnet_client_protocol_init( &session.protocol, &session.counters );
        ++initialized_sessions;
        memcpy( session.protocol.packet_filter, shared->packet_filter, ROCKETNET_FILTER_VALUE_BYTES );
        session.protocol.defer_payload_acks = 1;
        session.source = shared->source;
        session.source.port = shared->ports[client];
        session_of_port[session.source.port] = (uint16_t) i;
        session.next_packet_id = 1;
        rn_endpoint_config_t endpoint_config = {};
        endpoint_config.config = &message_config;
        endpoint_config.send_kbps = 0;
        endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        rn_endpoint_init( &session.endpoint, rn_arena_region( message_arena, (int) i ), &endpoint_config );
        rn_endpoint_reset( &session.endpoint, 0.0 );
        if ( !rocketnet_client_protocol_connect( &session.protocol, ( *shared->tokens )[client].bytes.data(), 0.0, unix_time ) ||
             !rocketnet_address_equal( &session.protocol.server_address, &server ) )
        {
            worker->sessions_ok = false;
            break;
        }
    }
    if ( !worker->sessions_ok )
    {
        shared->failed.store( true );
    }

    rn_blaster_results_t & results = worker->results;
    const uint64_t start_ns = shared->start_ns;
    const uint64_t connect_deadline_ns = start_ns + RN_BLASTER_CONNECT_TIMEOUT_SECONDS * UINT64_C( 1000000000 );
    const uint32_t rate_pps = rn_blaster_thread_share( options.plan.rate_pps, threads, worker->index );
    const uint64_t seed = rn_blaster_thread_seed( options.plan.seed, worker->index );
    worker->total_offers = rn_blaster_offered_packets( options.plan.duration_seconds, rate_pps );
    uint64_t workload_start_ns = 0;
    uint64_t workload_end_ns = 0;
    uint64_t last_now_ns = 0;
    uint32_t robustness_index = 0;
    std::array<uint8_t, ROCKETNET_SEND_BUDGET> scratch_packet = {};
    int scratch_packet_bytes = 0;
    std::array<uint8_t, ROCKETNET_SEND_BUDGET> case_valid = {};
    int case_valid_bytes = 0;
    uint64_t robustness_wait_ack = 0;
    bool robustness_baseline_sent = false;
    bool robustness_waiting_recovery = false;
    bool counted_ready = false;
    uint64_t last_handshake_step_ns = 0;
    rn_blaster_xsk_t * xsk = &worker->xsk;
    while ( worker->sessions_ok && !shared->failed.load( std::memory_order_relaxed ) )
    {
        uint64_t now_ns = 0;
        if ( !rn_blaster_now_nanoseconds( &now_ns ) )
        {
            worker->sessions_ok = false;
            break;
        }
        last_now_ns = now_ns;
        if ( workload_start_ns == 0 )
        {
            const uint64_t shared_start = shared->workload_start_ns.load( std::memory_order_acquire );
            if ( shared_start != 0 )
            {
                workload_start_ns = shared_start;
                workload_end_ns = workload_start_ns + (uint64_t) options.plan.duration_seconds * UINT64_C( 1000000000 );
            }
        }
        if ( workload_start_ns != 0 && now_ns >= workload_end_ns )
        {
            break;
        }
        if ( workload_start_ns == 0 && now_ns >= connect_deadline_ns )
        {
            worker->sessions_ok = false;
            break;
        }
        const double now = (double) ( now_ns - start_ns ) / 1000000000.0;
        for ( rn_blaster_session_t & session : sessions )
        {
            (void) rn_endpoint_begin_update( &session.endpoint );
        }
        rn_blaster_receive( xsk, &sessions, server, shared->port_owner, session_of_port, worker->index,
                            options.plan.payload_bytes, now, &results );
        if ( now_ns - last_handshake_step_ns >= UINT64_C( 1000000 ) )
        {
            last_handshake_step_ns = now_ns;
            for ( uint32_t i = 0; i < worker->clients; ++i )
            {
                rn_blaster_session_t & session = sessions[i];
                uint8_t packet[ROCKETNET_SEND_BUDGET];
                int packet_bytes = 0;
                rocketnet_client_protocol_update( &session.protocol, now, packet, &packet_bytes );
                if ( packet_bytes > 0 )
                {
                    (void) rn_blaster_send( xsk, packet, packet_bytes, session.source, server, shared->source_mac,
                                            shared->next_hop_mac, &results );
                }
                const bool connected = session.protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
                if ( connected && !session.endpoint_announced )
                {
                    rn_endpoint_announce_config( &session.endpoint, now );
                    session.endpoint_announced = true;
                }
                rn_endpoint_tick( &session.endpoint, now );
                rn_endpoint_config_tick( &session.endpoint, connected ? 1 : 0, now );
                if ( rn_endpoint_error( &session.endpoint ) != ROCKETNET_DISCONNECT_REASON_NONE )
                {
                    if ( worker->error.empty() )
                    {
                        worker->error = "message endpoint failed with disconnect reason " +
                                        std::to_string( (int) rn_endpoint_error( &session.endpoint ) );
                    }
                    worker->sessions_ok = false;
                }
                if ( connected )
                {
                    (void) rn_blaster_endpoint_packet( xsk, &session, now, server, shared->source_mac, shared->next_hop_mac,
                                                       &results, &scratch_packet, &scratch_packet_bytes, NULL );
                }
            }
            rn_blaster_flush( xsk, &results );
        }
        bool ready = worker->sessions_ok;
        for ( const rn_blaster_session_t & session : sessions )
        {
            ready = ready && session.protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                    session.endpoint.ordered.config_received;
        }
        worker->all_sessions_ready = ready;
        if ( ready && !counted_ready )
        {
            counted_ready = true;
            if ( shared->ready.fetch_add( 1u ) + 1u == threads )
            {
                shared->workload_start_ns.store( now_ns, std::memory_order_release );
                printf( "RUNTIME workload=started connected_clients=%u threads=%u\n", options.plan.clients, threads );
                fflush( stdout );
            }
        }
        if ( workload_start_ns != 0 && now_ns >= workload_start_ns && options.plan.mode == RN_BLASTER_MODE_THROUGHPUT )
        {
            const uint64_t elapsed_ns = now_ns - workload_start_ns;
            if ( ( options.plan.direction & RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ) == 0 )
            {
                const uint64_t due = std::min( worker->total_offers,
                                               UINT64_C( 1 ) + elapsed_ns * rate_pps / UINT64_C( 1000000000 ) );
                results.offered += due - worker->offer_ordinal;
                worker->offer_ordinal = due;
            }
            uint32_t burst = 0;
            while ( worker->offer_ordinal < worker->total_offers && burst < RN_BLASTER_TX_RING_SIZE &&
                    rn_blaster_schedule_nanoseconds( worker->offer_ordinal, rate_pps ) <= elapsed_ns )
            {
                const uint32_t local = rn_blaster_schedule_client( seed, worker->offer_ordinal, worker->clients );
                const uint32_t client = worker->first_client + local;
                rn_blaster_session_t & session = sessions[local];
                ++results.offered;
                if ( ( options.plan.direction & RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ) != 0 )
                {
                    uint8_t payload[RN_BLASTER_MAX_PAYLOAD_BYTES];
                    rn_blaster_application_message( payload, options.plan.payload_bytes, client,
                                                    ++session.application_sequence, options.plan.seed );
                    if ( rn_endpoint_send( &session.endpoint, payload, (int) options.plan.payload_bytes,
                                           ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) != 0 )
                    {
                        results.application_queued_bytes += options.plan.payload_bytes;
                        const uint64_t backpressure_before = results.backpressure;
                        if ( !rn_blaster_endpoint_packet( xsk, &session, now, server, shared->source_mac, shared->next_hop_mac,
                                                          &results, &scratch_packet, &scratch_packet_bytes, NULL ) &&
                             results.backpressure == backpressure_before )
                        {
                            // queued, but no packet was written for it: counted, never silent
                            ++results.unsent;
                        }
                    }
                    else
                    {
                        ++results.not_ready;
                    }
                }
                ++worker->offer_ordinal;
                ++burst;
            }
        }
        else if ( workload_start_ns != 0 && now_ns >= workload_start_ns && robustness_index < manifest.count )
        {
            rn_blaster_session_t & session = sessions[0];
            if ( robustness_wait_ack != 0 && session.watched_packet_acked )
            {
                robustness_wait_ack = 0;
                if ( robustness_waiting_recovery )
                {
                    robustness_waiting_recovery = false;
                    ++robustness_index;
                }
            }
            if ( robustness_wait_ack == 0 && !robustness_baseline_sent )
            {
                uint8_t baseline[RN_BLASTER_MAX_PAYLOAD_BYTES];
                rn_blaster_application_message( baseline, options.plan.payload_bytes, 0,
                                                ++session.application_sequence, options.plan.seed );
                if ( rn_endpoint_send( &session.endpoint, baseline, (int) options.plan.payload_bytes,
                                       ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) != 0 &&
                     rn_blaster_endpoint_packet( xsk, &session, now, server, shared->source_mac, shared->next_hop_mac,
                                                 &results, &case_valid, &case_valid_bytes, &robustness_wait_ack ) )
                {
                    robustness_baseline_sent = true;
                    session.watched_packet_id = robustness_wait_ack;
                    session.watched_packet_acked = false;
                    results.application_queued_bytes += options.plan.payload_bytes;
                }
            }
            else if ( robustness_baseline_sent && robustness_wait_ack == 0 && robustness_index < manifest.count )
            {
                const rn_blaster_case_t & item = manifest.cases[robustness_index];
                std::array<uint8_t, ROCKETNET_SEND_BUDGET> fresh_valid = {};
                int fresh_valid_bytes = 0;
                const uint8_t * mutation_source = case_valid.data();
                int mutation_source_bytes = case_valid_bytes;
                uint64_t unsent_packet_id = 0;
                if ( strcmp( item.sequence_state, "fresh" ) == 0 )
                {
                    uint8_t probe[RN_BLASTER_MAX_PAYLOAD_BYTES];
                    rn_blaster_application_message( probe, options.plan.payload_bytes, 0,
                                                    ++session.application_sequence, options.plan.seed );
                    if ( rn_endpoint_send( &session.endpoint, probe, (int) options.plan.payload_bytes,
                                           ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) == 0 ||
                         !rn_blaster_endpoint_prepare( &session, now, &fresh_valid, &fresh_valid_bytes,
                                                       &unsent_packet_id ) )
                    {
                        worker->sessions_ok = false;
                        continue;
                    }
                    rn_endpoint_packet_failed( &session.endpoint, unsent_packet_id, now );
                    mutation_source = fresh_valid.data();
                    mutation_source_bytes = fresh_valid_bytes;
                }
                uint8_t mutated[ROCKETNET_RECEIVE_CEILING + 1];
                const int mutated_bytes = rn_blaster_mutate_case( item, mutation_source,
                                                                  mutation_source_bytes, mutated, sizeof( mutated ) );
                ++results.offered;
                bool mutation_sent = mutated_bytes > 0;
                const uint32_t mutation_copies = item.operation == RN_BLASTER_CASE_REPLAY ? item.argument : 1u;
                for ( uint32_t copy = 0; copy < mutation_copies && mutation_sent; ++copy )
                {
                    mutation_sent = rn_blaster_send_robustness( xsk, mutated, mutated_bytes, session.source, server,
                                                                shared->source_mac, shared->next_hop_mac, &results );
                }
                uint8_t recovery_payload[RN_BLASTER_MAX_PAYLOAD_BYTES];
                rn_blaster_application_message( recovery_payload, options.plan.payload_bytes, 0,
                                                ++session.application_sequence, options.plan.seed );
                const bool recovery_queued = rn_endpoint_send( &session.endpoint, recovery_payload,
                                                               (int) options.plan.payload_bytes, ROCKETNET_RELIABILITY_UNRELIABLE,
                                                               ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) != 0;
                const bool recovery_sent = recovery_queued &&
                                           rn_blaster_endpoint_packet( xsk, &session, now, server, shared->source_mac,
                                                                       shared->next_hop_mac, &results, &case_valid,
                                                                       &case_valid_bytes, &robustness_wait_ack );
                if ( !mutation_sent || !recovery_sent )
                {
                    worker->sessions_ok = false;
                }
                else
                {
                    session.watched_packet_id = robustness_wait_ack;
                    session.watched_packet_acked = false;
                    results.application_queued_bytes += options.plan.payload_bytes;
                    ++results.robustness_cases;
                    ++results.recovery_offers;
                    robustness_waiting_recovery = true;
                }
            }
        }
        rn_blaster_flush( xsk, &results );
        if ( options.plan.mode == RN_BLASTER_MODE_ROBUSTNESS && robustness_index == manifest.count &&
             robustness_wait_ack == 0 )
        {
            break;
        }
    }
    if ( !worker->sessions_ok )
    {
        shared->failed.store( true );
    }
    rn_blaster_flush( xsk, &results );
    worker->workload_ns = workload_start_ns != 0 && last_now_ns > workload_start_ns ? last_now_ns - workload_start_ns : 0;
    worker->schedule_complete = options.plan.mode != RN_BLASTER_MODE_THROUGHPUT || worker->offer_ordinal == worker->total_offers;
    worker->robustness_complete = options.plan.mode != RN_BLASTER_MODE_ROBUSTNESS ||
                                  ( robustness_index == manifest.count && robustness_wait_ack == 0 &&
                                    results.robustness_cases == manifest.count );

    uint64_t drain_start_ns = 0;
    const bool clock_ok = rn_blaster_now_nanoseconds( &drain_start_ns );
    const uint64_t drain_deadline_ns = clock_ok ? drain_start_ns + RN_BLASTER_DRAIN_NANOSECONDS : 0;
    for ( uint32_t i = 0; i < initialized_sessions; ++i )
    {
        rn_blaster_session_t & session = sessions[i];
        uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
        int burst_count = 0;
        int burst_bytes = 0;
        rocketnet_client_protocol_disconnect( &session.protocol, burst, &burst_count, &burst_bytes );
        for ( int packet = 0; packet < burst_count && worker->opened; ++packet )
        {
            rn_blaster_send_drained( xsk, burst + packet * ROCKETNET_SEND_BUDGET, burst_bytes, session.source, server,
                                     shared->source_mac, shared->next_hop_mac, drain_deadline_ns, &worker->disconnect_results );
        }
        sodium_memzero( burst, sizeof( burst ) );
    }
    if ( worker->opened )
    {
        rn_blaster_flush( xsk, &worker->disconnect_results );
    }
    sodium_memzero( scratch_packet.data(), scratch_packet.size() );
    sodium_memzero( case_valid.data(), case_valid.size() );
    if ( !worker->error.empty() )
    {
        // name the message-layer refusal behind a failed endpoint
        for ( int counter = 0; counter < ROCKETNET_NUM_COUNTERS; ++counter )
        {
            const char * name = rocketnet_counter_name( counter );
            if ( message_counters.values[counter] != 0 && name && strncmp( name, "MESSAGE_", 8 ) == 0 )
            {
                fprintf( stderr, "xdp_packet_blaster: thread %u: counter %s %" PRIu64 "\n", worker->index, name,
                         message_counters.values[counter] );
            }
        }
    }
    if ( message_arena )
    {
        rn_arena_destroy( message_arena );
    }
}

static int rn_blaster_execute( const rn_blaster_options_t & options, const rn_blaster_manifest_t & manifest,
                               const rocketnet_address_t & source, const rocketnet_address_t & server,
                               const uint8_t source_mac[6], const uint8_t next_hop_mac[6], const uint8_t packet_filter[4],
                               const std::vector<rn_blaster_token_t> & tokens )
{
    const uint32_t threads = options.plan.threads;
    if ( geteuid() != 0 )
    {
        fprintf( stderr, "xdp_packet_blaster: --execute requires root on the reserved owned bench\n" );
        return 2;
    }
    if ( !rn_blaster_source_on_interface( options.interface_name, source ) )
    {
        fprintf( stderr, "xdp_packet_blaster: source address is not assigned to the named interface\n" );
        return 2;
    }
    if ( !rn_blaster_mac_on_interface( options.interface_name, source_mac ) )
    {
        fprintf( stderr, "xdp_packet_blaster: source MAC does not match the named interface\n" );
        return 2;
    }
    if ( threads == 1 && ( options.plan.queue != 0 || !rn_blaster_interface_one_queue( options.interface_name ) ) )
    {
        fprintf( stderr, "xdp_packet_blaster: qualification requires externally provisioned one-queue RSS and queue 0\n" );
        return 2;
    }
    rn_blaster_shared_t shared;
    shared.options = &options;
    shared.manifest = &manifest;
    shared.source = source;
    shared.server = server;
    memcpy( shared.source_mac, source_mac, 6 );
    memcpy( shared.next_hop_mac, next_hop_mac, 6 );
    memcpy( shared.packet_filter, packet_filter, 4 );
    shared.tokens = &tokens;
    shared.ports.assign( options.plan.clients, 0 );
    shared.port_owner.assign( 65536u, RN_BLASTER_PORT_UNOWNED );
    shared.start_ns = 0;
    shared.create_turn.store( 0 );
    shared.ready.store( 0 );
    shared.workload_start_ns.store( 0 );
    shared.failed.store( false );
    shared.zero_copy.store( false );
    std::vector<uint8_t> rss_key;
    std::vector<uint32_t> rss_indirection;
    if ( threads > 1 )
    {
        std::string rss_error;
        if ( !rn_blaster_interface_queues( options.interface_name, threads ) )
        {
            fprintf( stderr, "xdp_packet_blaster: --threads %u needs at least %u receive and transmit queues on the interface\n",
                     threads, threads );
            return 2;
        }
        if ( !rn_blaster_rss_hashes_ports( options.interface_name, source.type ) )
        {
            fprintf( stderr, "xdp_packet_blaster: --threads above 1 needs the interface's UDP RSS hash to include both ports"
                             " (ethtool -N IFACE rx-flow-hash udp4 sdfn)\n" );
            return 2;
        }
        if ( !rn_blaster_rss_read( options.interface_name, &rss_key, &rss_indirection, &rss_error ) )
        {
            fprintf( stderr, "xdp_packet_blaster: %s\n", rss_error.c_str() );
            return 2;
        }
    }
    uint8_t server_bytes[16] = {};
    uint8_t source_bytes[16] = {};
    const int family = source.type == ROCKETNET_ADDRESS_IPV6 ? 6 : 4;
    for ( int i = 0; i < ( family == 6 ? 8 : 0 ); ++i )
    {
        server_bytes[2 * i] = (uint8_t) ( server.data.ipv6[i] >> 8 );
        server_bytes[2 * i + 1] = (uint8_t) server.data.ipv6[i];
        source_bytes[2 * i] = (uint8_t) ( source.data.ipv6[i] >> 8 );
        source_bytes[2 * i + 1] = (uint8_t) source.data.ipv6[i];
    }
    if ( family == 4 )
    {
        memcpy( server_bytes, server.data.ipv4, 4 );
        memcpy( source_bytes, source.data.ipv4, 4 );
    }
    if ( !rn_blaster_assign_ports( rss_key.data(), rss_key.size(), rss_indirection.data(), (uint32_t) rss_indirection.size(),
                                   family, server_bytes, server.port, source_bytes, options.plan.source_port, threads,
                                   options.plan.clients, shared.ports.data(), shared.port_owner.data() ) )
    {
        fprintf( stderr, "xdp_packet_blaster: the source port space ran out before every thread had clients whose replies hash to its queue\n" );
        return 2;
    }
    rn_blaster_print_host( options.interface_name );
    if ( threads > 1 )
    {
        printf( "RUNTIME rss=toeplitz key_bytes=%u indirection_entries=%u udp_hash=sdfn\n", (unsigned) rss_key.size(),
                (unsigned) rss_indirection.size() );
    }
    std::vector<rn_blaster_worker_t> workers( threads );
    uint32_t next_client = 0;
    for ( uint32_t t = 0; t < threads; ++t )
    {
        rn_blaster_worker_t & worker = workers[t];
        worker = rn_blaster_worker_t();
        worker.index = t;
        worker.queue = threads == 1 ? options.plan.queue : t;
        worker.cpu = options.cpus.empty() ? -1 : (int) options.cpus[t];
        worker.running_cpu = -1;
        worker.first_client = next_client;
        worker.clients = rn_blaster_thread_share( options.plan.clients, threads, t );
        next_client += worker.clients;
        printf( "RUNTIME thread=%u queue=%u cpu=%d clients=%u ports=%u..%u\n", t, worker.queue, worker.cpu, worker.clients,
                (unsigned) shared.ports[worker.first_client], (unsigned) shared.ports[worker.first_client + worker.clients - 1u] );
    }
    fflush( stdout );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "xdp_packet_blaster: rocketnet_init failed\n" );
        return 1;
    }
    if ( !rn_blaster_now_nanoseconds( &shared.start_ns ) )
    {
        (void) rocketnet_term();
        return 1;
    }
    if ( threads == 1 && options.cpus.empty() )
    {
        rn_blaster_worker_run( &shared, &workers[0] );
    }
    else
    {
        std::vector<std::thread> running;
        running.reserve( threads );
        for ( uint32_t t = 0; t < threads; ++t )
        {
            running.emplace_back( rn_blaster_worker_run, &shared, &workers[t] );
        }
        for ( std::thread & thread : running )
        {
            thread.join();
        }
    }
    for ( uint32_t t = 0; t < threads; ++t )
    {
        if ( !workers[t].error.empty() )
        {
            fprintf( stderr, "xdp_packet_blaster: thread %u: %s\n", t, workers[t].error.c_str() );
        }
    }
    if ( workers[0].opened )
    {
        printf( "RUNTIME attached=true selected_bind=%s queue=%u\n", workers[0].xsk.zero_copy ? "zero-copy" : "copy",
                workers[0].queue );
    }

    rn_blaster_results_t results = {};
    rn_blaster_results_t disconnect_results = {};
    bool sessions_ok = true;
    bool all_sessions_ready = true;
    bool schedule_complete = true;
    bool robustness_complete = true;
    uint64_t workload_ns = 0;
    for ( uint32_t t = 0; t < threads; ++t )
    {
        const rn_blaster_worker_t & worker = workers[t];
        rn_blaster_results_add( &results, &worker.results );
        rn_blaster_results_add( &disconnect_results, &worker.disconnect_results );
        sessions_ok = sessions_ok && worker.sessions_ok && worker.opened;
        all_sessions_ready = all_sessions_ready && worker.all_sessions_ready;
        schedule_complete = schedule_complete && worker.schedule_complete;
        robustness_complete = robustness_complete && worker.robustness_complete;
        workload_ns = std::max( workload_ns, worker.workload_ns );
        if ( threads > 1 )
        {
            printf( "RESULT_THREAD thread=%u queue=%u cpu=%d running_cpu=%d clients=%u offered=%" PRIu64 " submitted=%" PRIu64
                    " backpressure=%" PRIu64 " not_ready=%" PRIu64 " unsent=%" PRIu64 " rx_frames=%" PRIu64 " rx_wrong_thread=%" PRIu64
                    " rx_refused=%" PRIu64 " physical_wire_bytes=%" PRIu64 " tx_batches=%" PRIu64 " tx_wakeups=%" PRIu64
                    " workload_ns=%" PRIu64 "\n",
                    t, worker.queue, worker.cpu, worker.running_cpu, worker.clients, worker.results.offered, worker.results.submitted,
                    worker.results.backpressure, worker.results.not_ready, worker.results.unsent, worker.results.received_frames,
                    worker.results.receive_wrong_thread, worker.results.receive_refused, worker.results.physical_wire_bytes,
                    worker.results.transmit_batches, worker.results.transmit_wakeups, worker.workload_ns );
        }
    }
    printf( "RESULT work_units_offered=%" PRIu64 " submitted=%" PRIu64 " backpressure=%" PRIu64 " not_ready=%" PRIu64 " unsent=%" PRIu64
            " rx_frames=%" PRIu64 " rx_protocol=%" PRIu64 " rx_application=%" PRIu64
            " rx_refused=%" PRIu64 " descriptor_refused=%" PRIu64
            " protocol_bytes=%" PRIu64 " ethernet_frame_bytes=%" PRIu64 " physical_wire_bytes=%" PRIu64
            " application_queued_bytes=%" PRIu64 " application_received_bytes=%" PRIu64
            " payload_errors=%" PRIu64 " ownership_errors=%" PRIu64
            " robustness_cases=%" PRIu64 " recovery_offers=%" PRIu64
            " threads=%u rx_wrong_thread=%" PRIu64 " tx_batches=%" PRIu64 " tx_wakeups=%" PRIu64
            " schedule_complete=%d workload_ns=%" PRIu64 "\n",
            results.offered, results.submitted, results.backpressure, results.not_ready, results.unsent, results.received_frames,
            results.received_protocol, results.received_application, results.receive_refused,
            results.descriptor_refused, results.protocol_bytes,
            results.ethernet_frame_bytes, results.physical_wire_bytes,
            results.application_queued_bytes, results.application_received_bytes,
            results.payload_errors, results.ownership_errors,
            results.robustness_cases, results.recovery_offers,
            threads, results.receive_wrong_thread, results.transmit_batches, results.transmit_wakeups,
            schedule_complete ? 1 : 0, workload_ns );
    if ( workload_ns > 0 )
    {
        const double seconds = (double) workload_ns / 1000000000.0;
        printf( "RESULT_RATE seconds=%.3f submitted_pps=%.0f physical_wire_gbps=%.3f ethernet_frame_gbps=%.3f\n", seconds,
                (double) results.submitted / seconds, (double) results.physical_wire_bytes * 8.0 / seconds / 1e9,
                (double) results.ethernet_frame_bytes * 8.0 / seconds / 1e9 );
    }
    for ( uint32_t i = 0; i < manifest.count && options.plan.mode == RN_BLASTER_MODE_ROBUSTNESS; ++i )
    {
        const int profile = rn_blaster_server_transport_profile( options.server_transport_profile );
        rocketnet_assert( profile >= 0 );
        printf( "CASE name=%s server_transport_profile=%s sequence_state=%s expected_stage=%s expected_counter=%s expected_delta=%u"
                " expected_recovery=%s\n",
                manifest.cases[i].name, options.server_transport_profile.c_str(), manifest.cases[i].sequence_state,
                manifest.cases[i].profile_oracles[profile].expected_stage,
                manifest.cases[i].profile_oracles[profile].expected_counter, manifest.cases[i].expected_delta,
                manifest.cases[i].expected_recovery );
    }

    /* every other socket first: the attaching socket judges the interface clean */
    for ( uint32_t t = threads; t-- > 1; )
    {
        rn_blaster_xsk_close( &workers[t].xsk );
    }
    rn_blaster_xsk_close( &workers[0].xsk );
    rn_blaster_print_xdp_ids( "after", workers[0].xsk.after );
    const bool clean = workers[0].xsk.cleanup_empty;
    const bool rlimit_restored = workers[0].xsk.rlimit_restored;
    const int term = rocketnet_term();
    printf( "CLEANUP disconnect_packets=%" PRIu64 " disconnect_backpressure=%" PRIu64
            " disconnect_ownership_errors=%" PRIu64
            " secrets=protocol-reset xdp_program=%s queue=closed umem=freed rlimit=%s"
            " process=%s settings=unchanged\n",
            disconnect_results.submitted, disconnect_results.backpressure, disconnect_results.ownership_errors,
            clean ? "absent" : "present", rlimit_restored ? "restored" : "restore-failed", threads == 1 ? "single" : "threads" );
    const bool downstream_observed = ( options.plan.direction & RN_BLASTER_DIRECTION_SERVER_TO_CLIENT ) == 0 ||
                                     results.received_application > 0;
    const bool upstream_observed = ( options.plan.direction & RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ) == 0 ||
                                   results.application_queued_bytes > 0;
    const char * counters_refusal = rn_blaster_counters_refusal( &results, &disconnect_results );
    if ( counters_refusal )
    {
        fprintf( stderr, "xdp_packet_blaster: the run fails on its counters: %s is not 0\n", counters_refusal );
    }
    if ( !sessions_ok || !all_sessions_ready || !schedule_complete || !upstream_observed ||
         !robustness_complete || !downstream_observed || counters_refusal || !clean || !rlimit_restored ||
         term != ROCKETNET_STATUS_OK )
    {
        return 1;
    }
    return 0;
}

#else

static int rn_blaster_execute( const rn_blaster_options_t &, const rn_blaster_manifest_t &,
                               const rocketnet_address_t &, const rocketnet_address_t &,
                               const uint8_t[6], const uint8_t[6], const uint8_t[4],
                               const std::vector<rn_blaster_token_t> & )
{
    return 2;
}

#endif
