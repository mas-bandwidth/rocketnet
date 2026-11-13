/* RocketNet benchmark-only XDP packet blaster. Not part of the SDK API. */

#pragma once

#ifndef ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H
#define ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H

#include <stddef.h>
#include <stdint.h>

#define RN_BLASTER_MAX_DURATION_SECONDS 600u
#define RN_BLASTER_MAX_CLIENTS 1000u
#define RN_BLASTER_MAX_RATE_PPS 20000000u
/* The largest message the endpoint sends as one unreliable record:
 * ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES (1411). One byte more is promoted to
 * its reliable twin, a numbered message, and the test server's raw echo
 * refuses those (tests/echo_payload.h): it would hand the blaster its own
 * send numbers. The echo cells measure the transport echo of one unreliable
 * record. The RocketNet packet is at most 37 + 1411 = 1448 bytes of the
 * 1452-byte send budget. */
#define RN_BLASTER_MAX_PAYLOAD_BYTES 1411u
#define RN_BLASTER_MIN_SOURCE_PORT 1024u
#define RN_BLASTER_MAX_THREADS 32u
#define RN_BLASTER_MAX_CPU 1023u
#define RN_BLASTER_PORT_UNOWNED 0xffu
#define RN_BLASTER_MAX_RSS_KEY_BYTES 64u
#define RN_BLASTER_MAX_RSS_INDIRECTION 512u
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
    uint32_t threads;
    uint16_t source_port;
    uint64_t seed;
    int address_family;
};

/* Counters one sender thread owns; the run's totals are their sum. */
struct rn_blaster_results_t
{
    uint64_t offered;
    uint64_t submitted;
    uint64_t backpressure;
    uint64_t not_ready;
    /* offers the endpoint queued but wrote no packet for in their flush (the
     * update's packets_per_update spent): they leave with a later packet, or
     * never */
    uint64_t unsent;
    uint64_t received_frames;
    uint64_t received_protocol;
    uint64_t received_application;
    uint64_t receive_refused;
    uint64_t receive_wrong_thread;
    uint64_t descriptor_refused;
    uint64_t protocol_bytes;
    uint64_t ethernet_frame_bytes;
    uint64_t physical_wire_bytes;
    uint64_t application_queued_bytes;
    uint64_t application_received_bytes;
    uint64_t payload_errors;
    uint64_t ownership_errors;
    uint64_t robustness_cases;
    uint64_t recovery_offers;
    uint64_t transmit_batches;
    uint64_t transmit_wakeups;
};

static inline void rn_blaster_results_add( struct rn_blaster_results_t * total, const struct rn_blaster_results_t * part )
{
    total->offered += part->offered;
    total->submitted += part->submitted;
    total->backpressure += part->backpressure;
    total->not_ready += part->not_ready;
    total->unsent += part->unsent;
    total->received_frames += part->received_frames;
    total->received_protocol += part->received_protocol;
    total->received_application += part->received_application;
    total->receive_refused += part->receive_refused;
    total->receive_wrong_thread += part->receive_wrong_thread;
    total->descriptor_refused += part->descriptor_refused;
    total->protocol_bytes += part->protocol_bytes;
    total->ethernet_frame_bytes += part->ethernet_frame_bytes;
    total->physical_wire_bytes += part->physical_wire_bytes;
    total->application_queued_bytes += part->application_queued_bytes;
    total->application_received_bytes += part->application_received_bytes;
    total->payload_errors += part->payload_errors;
    total->ownership_errors += part->ownership_errors;
    total->robustness_cases += part->robustness_cases;
    total->recovery_offers += part->recovery_offers;
    total->transmit_batches += part->transmit_batches;
    total->transmit_wakeups += part->transmit_wakeups;
}

/* The counter that fails the run (docs/XDP-PACKET-BLASTER.md), or NULL when
 * none does: a delivered payload of the wrong size or tag, a UMEM frame that
 * could not go back to its ring, a receive descriptor outside the RX region,
 * or a reply RSS steered to a thread that does not own its client (the
 * Toeplitz port assignment was wrong). rx_refused is reported, not judged: it
 * also counts frames from other hosts on the shared queue. */
static inline const char * rn_blaster_counters_refusal( const struct rn_blaster_results_t * results,
                                                        const struct rn_blaster_results_t * disconnect )
{
    if ( results->payload_errors != 0 )
    {
        return "payload_errors";
    }
    if ( results->ownership_errors != 0 || disconnect->ownership_errors != 0 )
    {
        return "ownership_errors";
    }
    if ( results->descriptor_refused != 0 )
    {
        return "descriptor_refused";
    }
    if ( results->receive_wrong_thread != 0 )
    {
        return "rx_wrong_thread";
    }
    return NULL;
}

/* Thread t's share of a total split over threads: the remainder goes to the
 * lowest-numbered threads, so the shares always sum to the total. */
static inline uint32_t rn_blaster_thread_share( uint32_t total, uint32_t threads, uint32_t thread )
{
    if ( threads == 0 || thread >= threads )
    {
        return 0;
    }
    return total / threads + ( thread < total % threads ? 1u : 0u );
}

/* The Toeplitz hash a NIC's RSS computes over input with key (Microsoft's
 * RSS specification): for every set input bit, XOR in the 32 key bits that
 * start at that bit's position. The key must be at least input_bytes + 4. */
static inline uint32_t rn_blaster_toeplitz( const uint8_t * key, size_t key_bytes, const uint8_t * input, size_t input_bytes )
{
    if ( key == NULL || input == NULL || key_bytes < input_bytes + 4u )
    {
        return 0;
    }
    uint32_t result = 0;
    uint32_t window = ( (uint32_t) key[0] << 24 ) | ( (uint32_t) key[1] << 16 ) | ( (uint32_t) key[2] << 8 ) | key[3];
    for ( size_t i = 0; i < input_bytes; ++i )
    {
        const uint8_t next = key[i + 4u];
        for ( int bit = 7; bit >= 0; --bit )
        {
            if ( ( input[i] >> bit ) & 1u )
            {
                result ^= window;
            }
            window = ( window << 1 ) | ( ( next >> bit ) & 1u );
        }
    }
    return result;
}

/* The RSS input of a UDP datagram as the receiving NIC sees it: source
 * address, destination address, source port, destination port, all in
 * network order. Returns the input length (12 for IPv4, 36 for IPv6). */
static inline size_t rn_blaster_rss_input( int address_family, const uint8_t * from_address, uint16_t from_port,
                                           const uint8_t * to_address, uint16_t to_port, uint8_t input[36] )
{
    const size_t address_bytes = address_family == 6 ? 16u : 4u;
    for ( size_t i = 0; i < address_bytes; ++i )
    {
        input[i] = from_address[i];
        input[address_bytes + i] = to_address[i];
    }
    input[2u * address_bytes] = (uint8_t) ( from_port >> 8 );
    input[2u * address_bytes + 1u] = (uint8_t) from_port;
    input[2u * address_bytes + 2u] = (uint8_t) ( to_port >> 8 );
    input[2u * address_bytes + 3u] = (uint8_t) to_port;
    return 2u * address_bytes + 4u;
}

/* The receive queue a NIC's RSS gives a reply from the server to one client
 * port: the hash indexes the indirection table modulo its size. */
static inline uint32_t rn_blaster_rss_queue( const uint8_t * key, size_t key_bytes, const uint32_t * indirection,
                                             uint32_t indirection_count, int address_family,
                                             const uint8_t * server_address, uint16_t server_port,
                                             const uint8_t * client_address, uint16_t client_port )
{
    if ( indirection == NULL || indirection_count == 0 )
    {
        return UINT32_MAX;
    }
    uint8_t input[36];
    const size_t input_bytes = rn_blaster_rss_input( address_family, server_address, server_port, client_address, client_port, input );
    return indirection[rn_blaster_toeplitz( key, key_bytes, input, input_bytes ) % indirection_count];
}

/* Choose every client's source port so that the server's replies to it land
 * on the receive queue of the thread that owns it. Thread t owns queue t and
 * rn_blaster_thread_share( clients, threads, t ) clients. Ports are taken in
 * ascending order from first_port; a port whose replies hash to a queue no
 * thread owns, or to a thread that already has its share, is skipped.
 * ports[] receives thread 0's ports, then thread 1's, and so on; owner[] (65536
 * entries) receives each chosen port's thread and RN_BLASTER_PORT_UNOWNED
 * elsewhere. Returns 0 when the port space runs out first. */
static inline int rn_blaster_assign_ports( const uint8_t * key, size_t key_bytes, const uint32_t * indirection,
                                           uint32_t indirection_count, int address_family,
                                           const uint8_t * server_address, uint16_t server_port,
                                           const uint8_t * client_address, uint16_t first_port,
                                           uint32_t threads, uint32_t clients, uint16_t * ports, uint8_t * owner )
{
    if ( threads == 0 || threads > RN_BLASTER_MAX_THREADS || clients < threads || ports == NULL || owner == NULL )
    {
        return 0;
    }
    uint32_t filled[RN_BLASTER_MAX_THREADS];
    uint32_t offset[RN_BLASTER_MAX_THREADS];
    uint32_t next_offset = 0;
    for ( uint32_t t = 0; t < threads; ++t )
    {
        filled[t] = 0;
        offset[t] = next_offset;
        next_offset += rn_blaster_thread_share( clients, threads, t );
    }
    for ( uint32_t port = 0; port <= UINT16_MAX; ++port )
    {
        owner[port] = RN_BLASTER_PORT_UNOWNED;
    }
    uint32_t assigned = 0;
    for ( uint32_t port = first_port; port <= UINT16_MAX && assigned < clients; ++port )
    {
        const uint32_t queue = threads == 1 ? 0u
                                            : rn_blaster_rss_queue( key, key_bytes, indirection, indirection_count, address_family,
                                                                    server_address, server_port, client_address, (uint16_t) port );
        if ( queue >= threads || filled[queue] == rn_blaster_thread_share( clients, threads, queue ) )
        {
            continue;
        }
        ports[offset[queue] + filled[queue]] = (uint16_t) port;
        owner[port] = (uint8_t) queue;
        ++filled[queue];
        ++assigned;
    }
    return assigned == clients;
}

/* Parse a CPU list such as "1-6" or "0-7,32-39" into cpus[], in order. Every
 * CPU at most once, at most capacity of them. Returns the count, 0 refused. */
static inline uint32_t rn_blaster_parse_cpu_list( const char * text, uint32_t * cpus, uint32_t capacity )
{
    if ( text == NULL || cpus == NULL || *text == '\0' )
    {
        return 0;
    }
    uint32_t count = 0;
    const char * cursor = text;
    while ( 1 )
    {
        uint32_t low = 0;
        if ( *cursor < '0' || *cursor > '9' )
        {
            return 0;
        }
        while ( *cursor >= '0' && *cursor <= '9' )
        {
            low = low * 10u + (uint32_t) ( *cursor - '0' );
            if ( low > RN_BLASTER_MAX_CPU )
            {
                return 0;
            }
            ++cursor;
        }
        uint32_t high = low;
        if ( *cursor == '-' )
        {
            ++cursor;
            if ( *cursor < '0' || *cursor > '9' )
            {
                return 0;
            }
            high = 0;
            while ( *cursor >= '0' && *cursor <= '9' )
            {
                high = high * 10u + (uint32_t) ( *cursor - '0' );
                if ( high > RN_BLASTER_MAX_CPU )
                {
                    return 0;
                }
                ++cursor;
            }
            if ( high < low )
            {
                return 0;
            }
        }
        for ( uint32_t cpu = low; cpu <= high; ++cpu )
        {
            if ( count == capacity )
            {
                return 0;
            }
            for ( uint32_t i = 0; i < count; ++i )
            {
                if ( cpus[i] == cpu )
                {
                    return 0;
                }
            }
            cpus[count++] = cpu;
        }
        if ( *cursor == '\0' )
        {
            return count;
        }
        if ( *cursor != ',' )
        {
            return 0;
        }
        ++cursor;
    }
}

static inline uint64_t rn_blaster_mix64( uint64_t value )
{
    value += UINT64_C( 0x9e3779b97f4a7c15 );
    value = ( value ^ ( value >> 30 ) ) * UINT64_C( 0xbf58476d1ce4e5b9 );
    value = ( value ^ ( value >> 27 ) ) * UINT64_C( 0x94d049bb133111eb );
    return value ^ ( value >> 31 );
}

/* Each thread's schedule is the plan's schedule with its own seed, so one
 * thread's choices never depend on another's progress. Thread 0 keeps the
 * plan seed: a one-thread run is today's run. */
static inline uint64_t rn_blaster_thread_seed( uint64_t seed, uint32_t thread )
{
    return thread == 0 ? seed : seed ^ rn_blaster_mix64( (uint64_t) thread );
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
         plan->threads == 0 || plan->threads > RN_BLASTER_MAX_THREADS || plan->clients < plan->threads ||
         ( plan->mode == RN_BLASTER_MODE_ROBUSTNESS && plan->threads != 1 ) ||
         plan->source_port < RN_BLASTER_MIN_SOURCE_PORT ||
         plan->source_port > UINT16_MAX - ( plan->clients - 1u ) ||
         ( plan->address_family != 4 && plan->address_family != 6 ) )
    {
        return 0;
    }
    return plan->direction == RN_BLASTER_DIRECTION_CLIENT_TO_SERVER ||
           plan->direction == RN_BLASTER_DIRECTION_SERVER_TO_CLIENT ||
           plan->direction == RN_BLASTER_DIRECTION_BIDIRECTIONAL;
}

#endif // ROCKETNET_TOOLS_XDP_PACKET_BLASTER_H
