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

    Both address families take the same checks in the same order: IPv4, and
    IPv6 with UDP directly after the fixed header. An IPv6 frame whose next
    header is an extension header (hop-by-hop, routing, fragment,
    destination options, AH, ESP) passes to the kernel stack and is counted,
    never parsed and never dropped; every other IPv6 next header, ICMPv6
    and its neighbor discovery above all, passes untouched.

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

// The kernel replay map: one entry per live source address and port, the
// highest sequence userspace has opened for it. Only userspace writes it,
// once per server update, and that writer and the pin are not built yet;
// this program only looks entries up (tla/RNXdpReplay.tla,
// KernelNeverWrites). Unpinned and unwritten, every lookup misses.

struct {
    __uint( type, BPF_MAP_TYPE_HASH );
    __type( key, struct rocketnet_server_xdp_replay_key_t );
    __type( value, struct rocketnet_server_xdp_replay_value_t );
    __uint( max_entries, ROCKETNET_XDP_REPLAY_MAX_ENTRIES );
} server_xdp_replay_map SEC(".maps");

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

// The IPv6 next header values that name an extension header (RFC 8200
// section 4): hop-by-hop options, routing, fragment, destination options,
// AH and ESP. The program does not walk the chain behind any of them.

static __always_inline int server_xdp_is_ipv6_extension( __u8 next_header )
{
    return next_header == IPPROTO_HOPOPTS  ||
           next_header == IPPROTO_ROUTING  ||
           next_header == IPPROTO_FRAGMENT ||
           next_header == IPPROTO_DSTOPTS  ||
           next_header == IPPROTO_AH       ||
           next_header == IPPROTO_ESP;
}

// Everything after the IP header is one function for both families, so the
// two cannot drift apart. The caller has parsed its IP header, checked that
// the UDP header fits, and compared the destination address against the
// configuration; this takes the UDP header, that address result, and the
// source address (4 bytes for family 4, 16 for family 6, network order) for
// the replay lookup.

static __always_inline int server_xdp_udp( struct xdp_md * ctx, struct udphdr * udp, void * data_end,
                                           const struct rocketnet_server_xdp_config_t * config, int address_match,
                                           __u8 family, const void * source_address )
{
    if ( udp->dest != config->public_port || !address_match )
    {
        return XDP_PASS;
    }

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

    // replay: a session packet from a source with a replay map entry is
    // dropped when its sequence is ROCKETNET_XDP_REPLAY_WINDOW or more behind
    // the entry's high-water (tla/RNXdpReplay.tla, KernelFilter). The
    // difference is taken modulo 2^32 and read as signed, so a sequence ahead
    // of the high-water passes across the wrap. The handshake types carry no
    // sequence here and are not checked. The map is only looked up, never
    // written: a forged sequence far ahead passes to userspace, fails the
    // AEAD open there, and moves nothing.

    if ( packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET ||
         packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET    ||
         packet_type == ROCKETNET_CONNECTION_DISCONNECT_PACKET ||
         packet_type == ROCKETNET_CONNECTION_ACK_PACKET        ||
         packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET )
    {
        struct rocketnet_server_xdp_replay_key_t key;
        __builtin_memset( &key, 0, sizeof(key) );
        key.family = family;
        key.port = udp->source;
        if ( family == 4 )
            __builtin_memcpy( key.address, source_address, 4 );
        else
            __builtin_memcpy( key.address, source_address, 16 );

        const struct rocketnet_server_xdp_replay_value_t * entry = (const struct rocketnet_server_xdp_replay_value_t*) bpf_map_lookup_elem( &server_xdp_replay_map, &key );
        if ( entry )
        {
            // the 32-bit sequence, little endian, right after the prefilter
            // header: inside the ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES bound
            // proven above

            const __u32 sequence = ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES] )             |
                                   ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES + 1] << 8 )    |
                                   ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES + 2] << 16 )   |
                                   ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES + 3] << 24 );

            if ( (int32_t) ( entry->high_water - sequence ) >= ROCKETNET_XDP_REPLAY_WINDOW )
            {
                debug_printf( "replayed: behind the high-water window" );
                server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_REPLAYED );
                return XDP_DROP;
            }
        }
    }

    // bpf_redirect_map returns XDP_REDIRECT when the queue's socket is in
    // the map. When it is not, it returns the low two bits of the flags as
    // the action: BPF_F_INGRESS is 1, which is XDP_DROP. It never returns 0.

    const __u32 queue_id = ctx->rx_queue_index;
    const long ret = bpf_redirect_map( &server_xdp_socket_map, queue_id, BPF_F_INGRESS );
    if ( ret == XDP_REDIRECT )
    {
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
        return XDP_REDIRECT;
    }
    return XDP_DROP;
}

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

                    const int address_match = config->public_address == 0 || ip->daddr == config->public_address;

                    return server_xdp_udp( ctx, udp, data_end, config, address_match, 4, &ip->saddr );
                }
            }
        }
        else if ( eth->h_proto == __constant_htons(ETH_P_IPV6) ) // IPV6
        {
            struct ipv6hdr * ip6 = data + sizeof(struct ethhdr);

            if ( (void*)ip6 + sizeof(struct ipv6hdr) > data_end )
            {
                debug_printf( "smaller than ipv6 header" );
                return XDP_DROP;
            }

            if ( ip6->nexthdr == IPPROTO_UDP ) // UDP directly after the fixed header only
            {
                struct udphdr * udp = (void*) ip6 + sizeof(struct ipv6hdr);

                if ( (void*)udp + sizeof(struct udphdr) <= data_end )
                {
                    int key = 0;
                    struct rocketnet_server_xdp_config_t * config = (struct rocketnet_server_xdp_config_t*) bpf_map_lookup_elem( &server_xdp_config_map, &key );
                    if ( config == NULL || config->public_port == 0 )
                    {
                        debug_printf( "server_xdp is not configured" );
                        return XDP_PASS;
                    }

                    // the destination as four fixed-offset words; all zero accepts any

                    const __u32 * want = (const __u32*) config->public_address_ipv6;

                    const int address_match = ( want[0] | want[1] | want[2] | want[3] ) == 0 ||
                                              ( ip6->daddr.in6_u.u6_addr32[0] == want[0] &&
                                                ip6->daddr.in6_u.u6_addr32[1] == want[1] &&
                                                ip6->daddr.in6_u.u6_addr32[2] == want[2] &&
                                                ip6->daddr.in6_u.u6_addr32[3] == want[3] );

                    return server_xdp_udp( ctx, udp, data_end, config, address_match, 6, &ip6->saddr );
                }
            }
            else if ( server_xdp_is_ipv6_extension( ip6->nexthdr ) )
            {
                // an extension header sits between this header and UDP, and
                // the program does not walk the chain: the kernel stack
                // gets the frame, whatever is behind the header

                debug_printf( "ipv6 extension header" );
                server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_PASSED_IPV6_EXTENSION );
                return XDP_PASS;
            }

            // every other next header (ICMPv6 above all, which carries
            // neighbor discovery) passes untouched
        }
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";

#endif // #if defined(__linux__) && defined(__BPF__)
