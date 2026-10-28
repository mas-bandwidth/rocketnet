/* RocketNet benchmark-only XDP packet blaster. Not part of the SDK API. */

#pragma once

#ifndef ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H
#define ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H

#include <stddef.h>
#include <stdint.h>

#define RN_BLASTER_MAX_DURATION_SECONDS 600u
#define RN_BLASTER_MAX_CLIENTS 1000u
#define RN_BLASTER_MAX_RATE_PPS 20000000u
#define RN_BLASTER_MAX_PAYLOAD_BYTES 1200u
#define RN_BLASTER_MIN_SOURCE_PORT 1024u
#define RN_BLASTER_MAX_MANIFEST_CASES 64u
#define RN_BLASTER_MAX_CASE_NAME 64u
#define RN_BLASTER_MAX_CASE_TEXT 96u
#define RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES 6u
#define RN_BLASTER_OWNED_CONFIRM "I-OWN-THIS-BENCH"

enum rn_blaster_mode_t
{
    RN_BLASTER_MODE_THROUGHPUT = 0,
    RN_BLASTER_MODE_ROBUSTNESS = 1
};

enum rn_blaster_direction_t
{
    RN_BLASTER_DIRECTION_CLIENT_TO_SERVER = 1,
    RN_BLASTER_DIRECTION_SERVER_TO_CLIENT = 2,
    RN_BLASTER_DIRECTION_BIDIRECTIONAL = 3
};

enum rn_blaster_bind_t
{
    RN_BLASTER_BIND_AUTO = 0,
    RN_BLASTER_BIND_COPY = 1,
    RN_BLASTER_BIND_ZERO_COPY = 2
};

enum rn_blaster_case_operation_t
{
    RN_BLASTER_CASE_TRUNCATE = 0,
    RN_BLASTER_CASE_OVERSIZE = 1,
    RN_BLASTER_CASE_WRONG_EPOCH = 2,
    RN_BLASTER_CASE_WRONG_FILTER = 3,
    RN_BLASTER_CASE_RESERVED_TYPE = 4,
    RN_BLASTER_CASE_REPLAY = 5,
    RN_BLASTER_CASE_BAD_TAG = 6
};

struct rn_blaster_case_t
{
    char name[RN_BLASTER_MAX_CASE_NAME];
    char source[RN_BLASTER_MAX_CASE_TEXT];
    struct
    {
        char expected_stage[RN_BLASTER_MAX_CASE_TEXT];
        char expected_counter[RN_BLASTER_MAX_CASE_TEXT];
    } profile_oracles[RN_BLASTER_NUM_SERVER_TRANSPORT_PROFILES];
    char expected_recovery[RN_BLASTER_MAX_CASE_TEXT];
    char sequence_state[RN_BLASTER_MAX_CASE_TEXT];
    enum rn_blaster_case_operation_t operation;
    uint32_t argument;
    uint32_t expected_delta;
};

struct rn_blaster_manifest_t
{
    struct rn_blaster_case_t cases[RN_BLASTER_MAX_MANIFEST_CASES];
    uint32_t count;
};

struct rn_blaster_plan_t
{
    enum rn_blaster_mode_t mode;
    enum rn_blaster_direction_t direction;
    enum rn_blaster_bind_t bind;
    uint32_t duration_seconds;
    uint32_t clients;
    uint32_t rate_pps;
    uint32_t payload_bytes;
    uint32_t queue;
    uint16_t source_port;
    uint64_t seed;
    int address_family;
};

static inline uint64_t rn_blaster_mix64( uint64_t value )
{
    value += UINT64_C( 0x9e3779b97f4a7c15 );
    value = ( value ^ ( value >> 30 ) ) * UINT64_C( 0xbf58476d1ce4e5b9 );
    value = ( value ^ ( value >> 27 ) ) * UINT64_C( 0x94d049bb133111eb );
    return value ^ ( value >> 31 );
}

/* One stable client choice per offered datagram. */
static inline uint32_t rn_blaster_schedule_client( uint64_t seed, uint64_t ordinal, uint32_t clients )
{
    return clients == 0 ? 0 : (uint32_t) ( rn_blaster_mix64( seed ^ ordinal ) % clients );
}

/* Integer nanosecond deadlines avoid accumulated floating point drift. */
static inline uint64_t rn_blaster_schedule_nanoseconds( uint64_t ordinal, uint32_t rate_pps )
{
    return rate_pps == 0 ? UINT64_MAX : ( ordinal * UINT64_C( 1000000000 ) ) / rate_pps;
}

static inline uint64_t rn_blaster_offered_packets( uint32_t duration_seconds, uint32_t rate_pps )
{
    return (uint64_t) duration_seconds * (uint64_t) rate_pps;
}

static inline uint32_t rn_blaster_ethernet_frame_bytes( int address_family, uint32_t rocketnet_wire_bytes )
{
    const uint32_t ip_bytes = address_family == 6 ? 40u : 20u;
    return 14u + ip_bytes + 8u + rocketnet_wire_bytes;
}

/* Physical medium occupancy: preamble/SFD + padded frame/FCS + IFG. */
static inline uint32_t rn_blaster_physical_wire_bytes( uint32_t ethernet_frame_bytes )
{
    const uint32_t frame_with_fcs = ethernet_frame_bytes + 4u;
    return 8u + ( frame_with_fcs < 64u ? 64u : frame_with_fcs ) + 12u;
}

static inline int rn_blaster_ipv4_private( const uint8_t address[4] )
{
    return address != NULL &&
           ( address[0] == 10u ||
             ( address[0] == 172u && address[1] >= 16u && address[1] <= 31u ) ||
             ( address[0] == 192u && address[1] == 168u ) );
}

static inline int rn_blaster_ipv6_ula( const uint8_t address[16] )
{
    return address != NULL && ( address[0] & 0xfeu ) == 0xfcu;
}

static inline int rn_blaster_plan_valid( const struct rn_blaster_plan_t * plan )
{
    if ( plan == NULL || plan->duration_seconds == 0 || plan->duration_seconds > RN_BLASTER_MAX_DURATION_SECONDS ||
         plan->clients == 0 || plan->clients > RN_BLASTER_MAX_CLIENTS ||
         plan->rate_pps == 0 || plan->rate_pps > RN_BLASTER_MAX_RATE_PPS ||
         plan->payload_bytes == 0 || plan->payload_bytes > RN_BLASTER_MAX_PAYLOAD_BYTES ||
         plan->queue != 0 ||
         plan->source_port < RN_BLASTER_MIN_SOURCE_PORT ||
         plan->source_port > UINT16_MAX - ( plan->clients - 1u ) ||
         ( plan->address_family != 4 && plan->address_family != 6 ) )
        return 0;
    return plan->direction == RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ||
           plan->direction == RN_BLASTER_DIRECTION_SERVER_TO_CLIENT ||
           plan->direction == RN_BLASTER_DIRECTION_BIDIRECTIONAL;
}

#endif // ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H
