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

    if ( (void*)eth + sizeof(struct ethhdr) <= data_end )
    {
        if ( eth->h_proto == __constant_htons(ETH_P_IP) ) // IPV4
        {
            struct iphdr * ip = data + sizeof(struct ethhdr);

            if ( (void*)ip + sizeof(struct iphdr) > data_end )
            {
                debug_printf( "smaller than ipv4 header" );
                return XDP_DROP;
            }

            if ( ip->ihl == 5 && ip->protocol == IPPROTO_UDP ) // UDP only
            {
                struct udphdr * udp = (void*) ip + sizeof(struct iphdr);

                if ( (void*)udp + sizeof(struct udphdr) <= data_end )
                {
                    int key = 0;
                    struct rocketnet_server_xdp_config_t * config = (struct rocketnet_server_xdp_config_t*) bpf_map_lookup_elem( &server_xdp_config_map, &key );
                    if ( config == NULL || config->public_port == 0 )
                    {
                        debug_printf( "server_xdp is not configured" );
                        return XDP_PASS;
                    }

                    if ( udp->dest == config->public_port && ( config->public_address == 0 || ip->daddr == config->public_address ) )
                    {
                        __u8 * packet_data = (unsigned char*) (void*)udp + sizeof(struct udphdr);

                        // wire size bounds: nothing a server accepts is smaller
                        // than the smallest inbound type or larger than the
                        // receive ceiling

                        if ( (void*)packet_data + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES > data_end )
                        {
                            debug_printf( "packet is too small" );
                            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
                            return XDP_DROP;
                        }

                        int packet_bytes = data_end - (void*)udp - sizeof(struct udphdr);

                        if ( packet_bytes > ROCKETNET_RECEIVE_CEILING )
                        {
                            debug_printf( "packet is too large" );
                            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE );
                            return XDP_DROP;
                        }

                        // the prefilter header: the epoch names which rule
                        // produced the filter value, so a generation this
                        // server does not hold dies on a comparison before
                        // the value is even examined

                        if ( packet_data[1] != config->filter_epoch )
                        {
                            debug_printf( "wrong filter epoch" );
                            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_EPOCH );
                            return XDP_DROP;
                        }

                        if ( packet_data[2] != config->filter_value[0] ||
                             packet_data[3] != config->filter_value[1] ||
                             packet_data[4] != config->filter_value[2] ||
                             packet_data[5] != config->filter_value[3] )
                        {
                            debug_printf( "wrong filter value" );
                            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_WRONG_FILTER );
                            return XDP_DROP;
                        }

                        const __u8 packet_type = packet_data[0];

                        if ( !ROCKETNET_WIRE_SERVER_PACKET_VALID( packet_type, packet_bytes ) )
                        {
                            debug_printf( "invalid packet type or size" );
                            server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_INVALID_TYPE );
                            return XDP_DROP;
                        }

                        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );

                        const __u32 queue_id = ctx->rx_queue_index;
                        bpf_redirect_map( &server_xdp_socket_map, queue_id, BPF_F_INGRESS );
                        return XDP_REDIRECT;
                    }
                }
            }
        }
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";

#endif // #if defined(__linux__) && defined(__BPF__)
