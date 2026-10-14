/*
    RocketNet Server XDP program

    Licensed under the GNU General Public License 3.0

    Drops everything that is not a well-formed RocketNet packet for this
    server before it costs a ring slot: UDP to the configured address and
    port, wire size in bounds, the filter epoch and value, and a
    server-inbound packet type at its exact wire size -- the same rule the userspace
    refusal ladder applies, shared via rocketnet_wire.h so the two can
    never drift apart. Survivors redirect into the AF_XDP socket for their
    queue; traffic for any other port passes untouched.

    USAGE:

        make
        sudo xdp-loader load eno1 server_xdp.o --pin-path /sys/fs/bpf/server_xdp   # manual bring-up only; the library pins per instance under /sys/fs/bpf/rocketnet_xdp_<token>_<pid>_<starttime>_<bootid>
        sudo xdp-loader status
        sudo cat /sys/kernel/debug/tracing/trace_pipe
        sudo xdp-loader unload -a eno1
*/

#if defined(__linux__) && defined(__BPF__)

#include <linux/in.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

#include "rocketnet_wire.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_parse.h"

struct {
    __uint( type, BPF_MAP_TYPE_ARRAY );
    __type( key, __u32 );
    __type( value, struct rocketnet_server_xdp_config_t );
    __uint( max_entries, 1 );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_config_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __type(key, __u32);
    __type(value, __u32);
    __uint(max_entries, ROCKETNET_XDP_MAX_QUEUES);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} server_xdp_socket_map SEC(".maps");

struct {
    __uint( type, BPF_MAP_TYPE_PERCPU_ARRAY );
    __type( key, __u32 );
    __type( value, struct rocketnet_server_xdp_stats_t );
    __uint( max_entries, 1 );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_stats_map SEC(".maps");

static void server_xdp_count( __u32 counter )
{
    int key = 0;
    struct rocketnet_server_xdp_stats_t * stats = (struct rocketnet_server_xdp_stats_t*) bpf_map_lookup_elem( &server_xdp_stats_map, &key );
    if ( stats && counter < ROCKETNET_SERVER_XDP_NUM_COUNTERS )
    {
        stats->counters[counter]++;
    }
}

#define DEBUG 0

#if DEBUG
#define debug_printf bpf_printk
#else // #if DEBUG
#define debug_printf(...) do { } while (0)
#endif // #if DEBUG

SEC("server_xdp") int server_xdp_filter( struct xdp_md *ctx )
{
    void * data = (void*) (long) ctx->data;
    void * data_end = (void*) (long) ctx->data_end;

    struct ethhdr * eth = data;
    if ( (void*)eth + sizeof(struct ethhdr) > data_end )
    {
        return XDP_PASS;
    }

    int key = 0;
    struct rocketnet_server_xdp_config_t * config = (struct rocketnet_server_xdp_config_t*) bpf_map_lookup_elem( &server_xdp_config_map, &key );
    if ( config == NULL || config->public_port == 0 )
    {
        debug_printf( "server_xdp is not configured" );
        return XDP_PASS;
    }

    struct rocketnet_xdp_parse_config_t parse_config;
    parse_config.public_port = config->public_port;
    parse_config.public_address = config->public_address;
    __builtin_memcpy( parse_config.filter_value, config->filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    parse_config.filter_epoch = config->filter_epoch;

    __u8 * udp_payload = NULL;
    int udp_payload_bytes = 0;

    if ( eth->h_proto == __constant_htons(ETH_P_IP) ) // IPV4
    {
        struct iphdr * ip = data + sizeof(struct ethhdr);
        if ( (void*)ip + sizeof(struct iphdr) > data_end )
        {
            debug_printf( "smaller than ipv4 header" );
            return XDP_DROP;
        }
        if ( ip->ihl != 5 || ip->protocol != IPPROTO_UDP )
        {
            return XDP_PASS;
        }
        struct udphdr * udp = (void*) ip + sizeof(struct iphdr);
        if ( (void*)udp + sizeof(struct udphdr) > data_end )
        {
            return XDP_DROP;
        }
        if ( udp->dest != config->public_port || ( config->public_address != 0 && ip->daddr != config->public_address ) )
        {
            return XDP_PASS;
        }
        udp_payload = (__u8*) (void*)udp + sizeof(struct udphdr);
        udp_payload_bytes = (int) ( (long) data_end - (long) udp_payload );
    }
    else if ( eth->h_proto == __constant_htons(ETH_P_IPV6) ) // IPV6
    {
        struct ipv6hdr * ip6 = data + sizeof(struct ethhdr);
        if ( (void*)ip6 + sizeof(struct ipv6hdr) > data_end )
        {
            debug_printf( "smaller than ipv6 header" );
            return XDP_DROP;
        }
        // No IPv6 public address is configured in this revision (the config
        // map holds an IPv4 address), so any destination address is
        // accepted for IPv6 and the destination port alone selects.
        __u8 * ptr = (__u8*) (void*)ip6 + sizeof(struct ipv6hdr);
        __u8 next = ip6->nexthdr;
        // Extension headers are walked with a small constant bound.
        // Accepted: Hop-by-Hop (0), Routing (43), Destination Options (60).
        // A Fragment header (44) is refused, matching the IPv4 ihl == 5
        // strictness. The walk is unrolled so the bound is constant.
        #pragma clang loop unroll(full)
        for ( int i = 0; i < ROCKETNET_XDP_MAX_EXTENSION_HEADERS; i++ )
        {
            if ( next == IPPROTO_UDP )
            {
                break;
            }
            if ( next == 44 )
            {
                return XDP_DROP;
            }
            if ( next != 0 && next != 43 && next != 60 )
            {
                return XDP_PASS;
            }
            if ( (void*)ptr + 8 > data_end )
            {
                return XDP_DROP;
            }
            const int ext_bytes = ( (int) ptr[1] + 1 ) * 8;
            if ( ext_bytes < 8 || (void*)ptr + ext_bytes > data_end )
            {
                return XDP_DROP;
            }
            next = ptr[0];
            ptr += ext_bytes;
        }
        if ( next != IPPROTO_UDP )
        {
            // Past the extension bound, or a fragment after the walk.
            if ( next == 44 || next == 0 || next == 43 || next == 60 )
            {
                return XDP_DROP;
            }
            return XDP_PASS;
        }
        struct udphdr * udp = (void*) ptr;
        if ( (void*)udp + sizeof(struct udphdr) > data_end )
        {
            return XDP_DROP;
        }
        if ( udp->dest != config->public_port )
        {
            return XDP_PASS;
        }
        udp_payload = (__u8*) (void*)udp + sizeof(struct udphdr);
        udp_payload_bytes = (int) ( (long) data_end - (long) udp_payload );
    }
    else
    {
        return XDP_PASS;
    }

    if ( (void*)udp_payload + 6 > data_end )
    {
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
        return XDP_DROP;
    }

    const int verdict = rocketnet_xdp_check_payload( udp_payload, udp_payload_bytes, &parse_config );

    if ( verdict == ROCKETNET_XDP_VERDICT_REDIRECT )
    {
        const __u32 queue_id = ctx->rx_queue_index;
        const int ret = bpf_redirect_map( &server_xdp_socket_map, queue_id, BPF_F_INGRESS );
        if ( ret == 0 )
        {
            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
            return XDP_REDIRECT;
        }
        return XDP_DROP;
    }

    server_xdp_count( (__u32) verdict );
    return XDP_DROP;
}

char _license[] SEC("license") = "GPL";

#endif // #if defined(__linux__) && defined(__BPF__)
