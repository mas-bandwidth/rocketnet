/*
    RocketNet Server XDP program

    Licensed under the GNU General Public License 3.0

    Drops everything that is not a well-formed RocketNet packet for this
    server before it costs a ring slot: UDP to the configured address and
    port, wire size in bounds, the filter epoch and value, and a
    server-inbound packet type at its exact wire size -- the same rule the userspace
    refusal ladder applies, shared via rocketnet_wire.h so the two can
    never drift apart. Survivors redirect into the AF_XDP socket for their
    queue; traffic for any other port passes untouched. Frames shorter than
    the IP header (runt IPv4 < 20 bytes or runt IPv6 < 40 bytes) drop
    before port inspection because they cannot name a port.

    Both address families take the same checks in the same order: IPv4, and
    IPv6 with UDP directly after the fixed header. RocketNet assumes a
    1500-byte MTU path and never sends a datagram the network must
    fragment, so an IP fragment for this server is hostile or misconfigured.
    The first fragment (IPv4 with more-fragments set and offset 0, IPv6 with
    a fragment header at offset 0 with more-fragments set, either carrying
    the UDP header) of a datagram to the configured address and port drops
    and is counted in DROPPED_FIRST_FRAGMENT: without it the kernel can
    never reassemble the datagram, and drops the rest at its reassembly
    timeout. Every other fragment (a non-first fragment to any port, a first
    fragment to another port or address, to an unconfigured server, or whose
    UDP header is not reached through the bounded extension-header walk)
    passes to the kernel stack and is
    counted in PASSED_FRAGMENT, so other services' fragmented traffic is
    untouched: a non-first fragment carries no UDP header, so nothing in it
    names a port. Both checks run before any size, epoch, filter or replay
    check, so a fragment never counts as any other drop.

    RocketNet never sends IPv4 options or IPv6 extension headers either, and
    in XDP mode no kernel socket listens on the server's port, so a datagram
    for this server behind either one has nowhere to go but the kernel's
    own error path. An IPv4 datagram with options (ihl 6 to 15) whose UDP
    header names the configured address and port drops and is counted in
    DROPPED_IP_OPTIONS_OR_EXTENSION; with options to any other port it passes untouched.
    An IPv6 frame whose next header is an extension header (hop-by-hop,
    routing, destination options, AH; a fragment header first is the
    fragment rule above) has its chain walked for at most
    SERVER_XDP_IPV6_EXTENSION_WALK headers: when the walk reaches UDP to the
    configured address and port the frame drops, counted in
    DROPPED_IP_OPTIONS_OR_EXTENSION. Every other extension-header frame (to another port,
    behind ESP, behind a non-first fragment header, a chain longer than the
    walk, or cut by the frame end) passes to the kernel stack counted in
    PASSED_IPV6_EXTENSION. Every other IPv6 next header, ICMPv6 and its
    neighbor discovery above all, passes untouched.

    USAGE:

        make
        sudo xdp-loader load eno1 server_xdp.o --pin-path /sys/fs/bpf/server_xdp   # manual bring-up only; the library pins per instance under /sys/fs/bpf/rocketnet_xdp_<token>_<pid>_<starttime>_<bootid>
        sudo xdp-loader status
        sudo cat /sys/kernel/debug/tracing/trace_pipe
        sudo xdp-loader unload -a eno1
*/

#if defined( __linux__ ) && defined( __BPF__ )

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

struct
{
    __uint( type, BPF_MAP_TYPE_ARRAY );
    __u32 * key;
    struct rocketnet_server_xdp_config_t * value;
    __uint( max_entries, 1 );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_config_map SEC( ".maps" );

struct
{
    __uint( type, BPF_MAP_TYPE_XSKMAP );
    __u32 * key;
    __u32 * value;
    __uint( max_entries, ROCKETNET_XDP_MAX_QUEUES );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_socket_map SEC( ".maps" );

struct
{
    __uint( type, BPF_MAP_TYPE_PERCPU_ARRAY );
    __u32 * key;
    struct rocketnet_server_xdp_stats_t * value;
    __uint( max_entries, 1 );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_stats_map SEC( ".maps" );

// The kernel replay map: one entry per live source address and port, the
// highest sequence userspace has opened for it. Only userspace writes it,
// once per server update, and this program only looks entries up
// (proofs/tla/RNXdpReplay.tla, KernelNeverWrites). Pinned by name so the server
// socket opens this instance's own map and is its only writer.

struct
{
    __uint( type, BPF_MAP_TYPE_HASH );
    struct rocketnet_server_xdp_replay_key_t * key;
    struct rocketnet_server_xdp_replay_value_t * value;
    __uint( max_entries, ROCKETNET_XDP_REPLAY_MAX_ENTRIES );
    __uint( pinning, LIBBPF_PIN_BY_NAME );
} server_xdp_replay_map SEC( ".maps" );

static void server_xdp_count( __u32 counter )
{
    int key = 0;
    struct rocketnet_server_xdp_stats_t * stats = (struct rocketnet_server_xdp_stats_t *) bpf_map_lookup_elem( &server_xdp_stats_map, &key );
    if ( stats && counter < ROCKETNET_SERVER_XDP_NUM_COUNTERS )
    {
        stats->counters[counter]++;
    }
}

#define DEBUG 0

#if DEBUG
#define debug_printf bpf_printk
#else // #if DEBUG
#define debug_printf( ... ) do { } while (0)
#endif // #if DEBUG

// The IPv6 next header values that name an extension header (RFC 8200
// section 4): hop-by-hop options, routing, fragment, destination options,
// AH and ESP. These are the headers the walk below starts from. That walk
// steps over hop-by-hop, routing, destination options, AH, and a first
// fragment. ESP stops the walk with the header unopened. A non-first
// fragment is read, and the walk stops without stepping past it.

static __always_inline int server_xdp_is_ipv6_extension( __u8 next_header )
{
    return next_header == IPPROTO_HOPOPTS ||
           next_header == IPPROTO_ROUTING ||
           next_header == IPPROTO_FRAGMENT ||
           next_header == IPPROTO_DSTOPTS ||
           next_header == IPPROTO_AH ||
           next_header == IPPROTO_ESP;
}

// The IPv6 fragment header (RFC 8200 section 4.5): next header, reserved,
// the fragment offset in 8-byte units shifted left 3 with the more-fragments
// flag in bit 0 (two reserved bits between), and the identification. The
// kernel's struct frag_hdr is not in a uapi header, so it is defined here.

struct server_xdp_ipv6_fragment_header_t
{
    __u8 next_header;
    __u8 reserved;
    __be16 offset_flags;
    __be32 identification;
};

// The longest IPv6 extension header chain the program walks looking for
// UDP. Each step is bounded (at most 2048 bytes for hop-by-hop, routing and
// destination options, 1028 for AH, 8 for a fragment header), so the walk
// stays inside the verifier's packet range.

#define SERVER_XDP_IPV6_EXTENSION_WALK 4

// One step of the walk: the header at *cursor, whose type is *next_header.
// Returns 1 and advances both when the header was one the walk steps over;
// returns 0 when the walk stops (UDP reached, a header it does not open,
// a non-first fragment, or the frame end).

static __always_inline int server_xdp_ipv6_extension_step( __u8 ** cursor, __u8 * next_header, __u8 * data_end )
{
    __u8 * header = *cursor;

    if ( *next_header == IPPROTO_HOPOPTS || *next_header == IPPROTO_ROUTING || *next_header == IPPROTO_DSTOPTS )
    {
        // RFC 8200 section 4.3 to 4.6: next header, then the length in
        // 8-byte units not counting the first 8

        if ( header + 2 > data_end )
            return 0;

        const __u32 bytes = ( (__u32) header[1] + 1 ) * 8;

        *next_header = header[0];
        *cursor = header + bytes;
        return 1;
    }

    if ( *next_header == IPPROTO_AH )
    {
        // RFC 4302 section 2.2: next header, then the length in 4-byte units
        // minus 2

        if ( header + 2 > data_end )
            return 0;

        const __u32 bytes = ( (__u32) header[1] + 2 ) * 4;

        *next_header = header[0];
        *cursor = header + bytes;
        return 1;
    }

    if ( *next_header == IPPROTO_FRAGMENT )
    {
        // only a first fragment has the UDP header behind it

        if ( header + sizeof( struct server_xdp_ipv6_fragment_header_t ) > data_end )
            return 0;

        struct server_xdp_ipv6_fragment_header_t fragment;
        __builtin_memcpy( &fragment, header, sizeof( fragment ) );
        if ( ( fragment.offset_flags & __constant_htons( 0xfff8 ) ) != 0 )
            return 0;
        *next_header = fragment.next_header;
        *cursor = header + sizeof( struct server_xdp_ipv6_fragment_header_t );
        return 1;
    }

    return 0;
}

// The configuration, or NULL when there is none or it names no port: an
// unconfigured program passes every frame except an IPv4 or IPv6 frame too
// short to hold its IP header, which no host could deliver.

static __always_inline struct rocketnet_server_xdp_config_t * server_xdp_config( void )
{
    int key = 0;
    struct rocketnet_server_xdp_config_t * config = (struct rocketnet_server_xdp_config_t *) bpf_map_lookup_elem( &server_xdp_config_map, &key );
    if ( config == NULL || config->public_port == 0 )
    {
        debug_printf( "server_xdp is not configured" );
        return NULL;
    }

    return config;
}

static __always_inline int server_xdp_ipv4_address_match( const struct rocketnet_server_xdp_config_t * config, const struct iphdr * ip )
{
    return config->public_address == 0 || ip->daddr == config->public_address;
}

// the destination as four fixed-offset words; all zero accepts any

static __always_inline int server_xdp_ipv6_address_match( const struct rocketnet_server_xdp_config_t * config, const struct ipv6hdr * ip6 )
{
    const __u32 * want = (const __u32 *) config->public_address_ipv6;

    return ( want[0] | want[1] | want[2] | want[3] ) == 0 ||
           ( ip6->daddr.in6_u.u6_addr32[0] == want[0] &&
             ip6->daddr.in6_u.u6_addr32[1] == want[1] &&
             ip6->daddr.in6_u.u6_addr32[2] == want[2] &&
             ip6->daddr.in6_u.u6_addr32[3] == want[3] );
}

// A UDP header off the fast path (behind a first fragment's IP header, IPv4
// options or IPv6 extension headers): is it in the frame, and addressed to
// this server? This checks the UDP header fits, the program is configured,
// and the destination port and address are this server's.

static __always_inline int server_xdp_udp_is_ours( struct udphdr * udp, __u8 * data_end, const struct rocketnet_server_xdp_config_t * config, int address_match )
{
    if ( (__u8 *) udp + sizeof( struct udphdr ) > data_end )
        return 0;
    return config != NULL && udp->dest == config->public_port && address_match;
}

// Everything after the IP header is one function for both families, so the
// two cannot drift apart. The caller has parsed its IP header, checked that
// the UDP header fits, and compared the destination address against the
// configuration; this takes the UDP header, the frame end, the bytes from
// the UDP header to the end of the datagram as its IP header gives them
// (ip_udp_bytes), that address result, and the source address (4 bytes for
// family 4, 16 for family 6, network order) for the replay lookup.
//
// The UDP and IP length fields are byte-swapped scalars the verifier does not
// bound, so they are compared as numbers against the frame bytes after the
// UDP header (here, and in the IPv4 and IPv6 branches of server_xdp_filter)
// and never added to a packet pointer.

static __always_inline int server_xdp_udp( struct xdp_md * ctx, struct udphdr * udp, __u32 ip_udp_bytes, __u8 * data_end,
                                           const struct rocketnet_server_xdp_config_t * config, int address_match,
                                           __u8 family, const void * source_address )
{
    if ( udp->dest != config->public_port || !address_match )
        return XDP_PASS;

    __u8 * packet_data = (__u8 *) udp + sizeof( struct udphdr );

    // wire size bounds: nothing a server accepts is smaller
    // than the smallest inbound type or larger than the
    // receive ceiling

    if ( packet_data + ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES > data_end )
    {
        debug_printf( "packet is too small" );
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
        return XDP_DROP;
    }

    const __u32 frame_udp_bytes = (__u32) ( data_end - (__u8 *) udp );
    const __u32 udp_len = __builtin_bswap16( udp->len );
    if ( udp_len < sizeof( struct udphdr ) )
    {
        debug_printf( "packet udp length is below the UDP header" );
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
        return XDP_DROP;
    }

    if ( udp_len > ip_udp_bytes || udp_len > frame_udp_bytes )
    {
        debug_printf( "packet udp length exceeds the IP datagram or the frame" );
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_LARGE );
        return XDP_DROP;
    }

    int packet_bytes = (int) ( udp_len - sizeof( struct udphdr ) );

    if ( packet_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES )
    {
        debug_printf( "packet is too small" );
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_TOO_SMALL );
        return XDP_DROP;
    }

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

    if ( packet_data[2] != config->packet_filter[0] ||
         packet_data[3] != config->packet_filter[1] ||
         packet_data[4] != config->packet_filter[2] ||
         packet_data[5] != config->packet_filter[3] )
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
    // the entry's high-water (proofs/tla/RNXdpReplay.tla, KernelFilter). The window
    // is the userspace replay window, so nothing dropped here would have
    // been accepted there. The
    // difference is taken modulo 2^32 and read as signed, so a sequence ahead
    // of the high-water passes across the wrap. The handshake types carry no
    // sequence here and are not checked. The map is only looked up, never
    // written: a forged sequence far ahead passes to userspace, fails the
    // AEAD open there, and moves nothing.

    if ( packet_type == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET ||
         packet_type == ROCKETNET_CONNECTION_PAYLOAD_PACKET ||
         packet_type == ROCKETNET_CONNECTION_DISCONNECT_PACKET ||
         packet_type == ROCKETNET_CONNECTION_ACK_PACKET ||
         packet_type == ROCKETNET_CONNECTION_FRAGMENT_PACKET )
    {
        struct rocketnet_server_xdp_replay_key_t key;
        __builtin_memset( &key, 0, sizeof( key ) );
        key.family = family;
        key.port = udp->source;

        if ( family == 4 )
        {
            __builtin_memcpy( key.address, source_address, 4 );
        }
        else
        {
            __builtin_memcpy( key.address, source_address, 16 );
        }

        const struct rocketnet_server_xdp_replay_value_t * entry = (const struct rocketnet_server_xdp_replay_value_t *) bpf_map_lookup_elem( &server_xdp_replay_map, &key );
        if ( entry )
        {
            // the 32-bit sequence, little endian, right after the prefilter
            // header: inside the ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES bound
            // proven above

            const __u32 sequence = ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES] ) |
                                   ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES + 1] << 8 ) |
                                   ( (__u32) packet_data[ROCKETNET_PREFILTER_BYTES + 2] << 16 ) |
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
    // A packet that passed every check but has no socket for its queue is
    // counted in DROPPED_NO_SOCKET.

    const __u32 queue_id = ctx->rx_queue_index;
    const long ret = bpf_redirect_map( &server_xdp_socket_map, queue_id, BPF_F_INGRESS );
    if ( ret == XDP_REDIRECT )
    {
        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_REDIRECTED );
        return XDP_REDIRECT;
    }

    debug_printf( "no socket in the map for this queue" );
    server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_NO_SOCKET );

    return XDP_DROP;
}

SEC( "server_xdp" )
int server_xdp_filter( struct xdp_md * ctx )
{
    __u8 * data = (__u8 *) (long) ctx->data;

    __u8 * data_end = (__u8 *) (long) ctx->data_end;

    struct ethhdr * eth = (struct ethhdr *) data;

    if ( (__u8 *) eth + sizeof( struct ethhdr ) <= data_end )
    {
        if ( eth->h_proto == __constant_htons( ETH_P_IP ) ) // IPV4
        {
            struct iphdr * ip = (struct iphdr *) ( data + sizeof( struct ethhdr ) );

            if ( (__u8 *) ip + sizeof( struct iphdr ) > data_end )
            {
                debug_printf( "smaller than ipv4 header" );
                return XDP_DROP;
            }

            // a fragment (more-fragments set or a nonzero offset): only the
            // first carries the UDP header, and none is a whole datagram. The
            // first fragment of a datagram to this server drops, so the
            // kernel can never reassemble it; every other fragment is the
            // kernel's to reassemble

            if ( ip->frag_off & __constant_htons( 0x3fff ) )
            {
                if ( ( ip->frag_off & __constant_htons( 0x3fff ) ) == __constant_htons( 0x2000 ) &&
                     ip->ihl >= 5 && ip->protocol == IPPROTO_UDP )
                {
                    struct udphdr * udp = (struct udphdr *) ( (__u8 *) ip + ip->ihl * 4 );
                    const struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                    if ( config != NULL && server_xdp_udp_is_ours( udp, data_end, config, server_xdp_ipv4_address_match( config, ip ) ) )
                    {
                        debug_printf( "ipv4 first fragment to this server" );
                        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT );
                        return XDP_DROP;
                    }
                }

                debug_printf( "ipv4 fragment" );
                server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT );
                return XDP_PASS;
            }

            if ( ip->ihl == 5 && ip->protocol == IPPROTO_UDP ) // UDP only
            {
                struct udphdr * udp = (struct udphdr *) ( (__u8 *) ip + sizeof( struct iphdr ) );

                if ( (__u8 *) udp + sizeof( struct udphdr ) <= data_end )
                {
                    struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                    if ( config == NULL )
                        return XDP_PASS;

                    // the IP total length bounds the datagram when it names
                    // at least a UDP header and fits in the frame; otherwise
                    // it is malformed; make server_xdp_udp count an otherwise
                    // valid UDP length as too large once the port and address
                    // match

                    const __u32 frame_udp_bytes = (__u32) ( data_end - (__u8 *) udp );
                    const __u32 ip_tot_len = __builtin_bswap16( ip->tot_len );
                    __u32 ip_udp_bytes = 0;
                    if ( ip_tot_len >= sizeof( struct iphdr ) + sizeof( struct udphdr ) && ip_tot_len - sizeof( struct iphdr ) <= frame_udp_bytes )
                    {
                        ip_udp_bytes = (__u32) ( ip_tot_len - sizeof( struct iphdr ) );
                    }

                    return server_xdp_udp( ctx, udp, ip_udp_bytes, data_end, config, server_xdp_ipv4_address_match( config, ip ), 4, &ip->saddr );
                }
            }
            else if ( ip->ihl > 5 && ip->protocol == IPPROTO_UDP )
            {
                // options between the IP header and UDP (ihl 6 to 15, at most
                // 40 bytes of them): a datagram to this server drops, any
                // other passes untouched

                struct udphdr * udp = (struct udphdr *) ( (__u8 *) ip + ip->ihl * 4 );
                const struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                if ( config != NULL && server_xdp_udp_is_ours( udp, data_end, config, server_xdp_ipv4_address_match( config, ip ) ) )
                {
                    debug_printf( "ipv4 options in front of a datagram to this server" );
                    server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION );
                    return XDP_DROP;
                }
            }
        }
        else if ( eth->h_proto == __constant_htons( ETH_P_IPV6 ) ) // IPV6
        {
            struct ipv6hdr * ip6 = (struct ipv6hdr *) ( data + sizeof( struct ethhdr ) );

            if ( (__u8 *) ip6 + sizeof( struct ipv6hdr ) > data_end )
            {
                debug_printf( "smaller than ipv6 header" );
                return XDP_DROP;
            }

            if ( ip6->nexthdr == IPPROTO_UDP ) // UDP directly after the fixed header only
            {
                struct udphdr * udp = (struct udphdr *) ( (__u8 *) ip6 + sizeof( struct ipv6hdr ) );

                if ( (__u8 *) udp + sizeof( struct udphdr ) <= data_end )
                {
                    struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                    if ( config == NULL )
                        return XDP_PASS;

                    // the IPv6 payload length bounds the datagram when it
                    // names at least a UDP header and fits in the frame;
                    // otherwise it is malformed; make server_xdp_udp count an
                    // otherwise valid UDP length as too large once the port
                    // and address match

                    const __u32 frame_udp_bytes = (__u32) ( data_end - (__u8 *) udp );
                    const __u32 ip6_payload_len = __builtin_bswap16( ip6->payload_len );
                    __u32 ip_udp_bytes = 0;
                    if ( ip6_payload_len >= sizeof( struct udphdr ) && ip6_payload_len <= frame_udp_bytes )
                    {
                        ip_udp_bytes = ip6_payload_len;
                    }

                    return server_xdp_udp( ctx, udp, ip_udp_bytes, data_end, config, server_xdp_ipv6_address_match( config, ip6 ), 6, &ip6->saddr );
                }
            }
            else if ( ip6->nexthdr == IPPROTO_FRAGMENT )
            {
                // a fragment, judged as the IPv4 fragments are: the first
                // fragment (offset 0, more-fragments set, UDP behind it,
                // possibly after a bounded extension-header chain) of a
                // datagram to this server drops; every other fragment is
                // the kernel's to reassemble

                // the header starts 54 bytes into the frame (the Ethernet
                // and fixed IPv6 headers), not a multiple of the struct's
                // 4-byte alignment, so it is copied through a byte pointer
                // into a local after the bounds check rather than read
                // through a struct pointer

                __u8 * fragment = (__u8 *) ip6 + sizeof( struct ipv6hdr );

                if ( fragment + sizeof( struct server_xdp_ipv6_fragment_header_t ) <= data_end )
                {
                    struct server_xdp_ipv6_fragment_header_t header;
                    __builtin_memcpy( &header, fragment, sizeof( header ) );

                    if ( ( header.offset_flags & __constant_htons( 0xfff9 ) ) == __constant_htons( 0x0001 ) )
                    {
                        __u8 * cursor = fragment + sizeof( struct server_xdp_ipv6_fragment_header_t );
                        __u8 next_header = header.next_header;
                        int walking = 1;

                        for ( int i = 0; i < SERVER_XDP_IPV6_EXTENSION_WALK; i++ )
                        {
                            // A second fragment header is malformed and is
                            // not part of the post-fragment policy walk.

                            if ( walking && next_header != IPPROTO_FRAGMENT )
                            {
                                walking = server_xdp_ipv6_extension_step( &cursor, &next_header, data_end );
                            }
                            else
                            {
                                walking = 0;
                            }
                        }

                        if ( next_header == IPPROTO_UDP )
                        {
                            struct udphdr * udp = (struct udphdr *) cursor;
                            const struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                            if ( config != NULL && server_xdp_udp_is_ours( udp, data_end, config, server_xdp_ipv6_address_match( config, ip6 ) ) )
                            {
                                debug_printf( "ipv6 first fragment to this server" );
                                server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_FIRST_FRAGMENT );
                                return XDP_DROP;
                            }
                        }
                    }
                }

                debug_printf( "ipv6 fragment" );
                server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_PASSED_FRAGMENT );
                return XDP_PASS;
            }
            else if ( server_xdp_is_ipv6_extension( ip6->nexthdr ) )
            {
                // an extension header sits between this header and UDP: walk
                // the chain a bounded number of headers, and when it reaches
                // UDP to this server, drop the frame. Anything else goes to
                // the kernel stack

                __u8 * cursor = (__u8 *) ip6 + sizeof( struct ipv6hdr );
                __u8 next_header = ip6->nexthdr;
                int walking = 1;

                for ( int i = 0; i < SERVER_XDP_IPV6_EXTENSION_WALK; i++ )
                {
                    if ( walking )
                    {
                        walking = server_xdp_ipv6_extension_step( &cursor, &next_header, data_end );
                    }
                }

                if ( next_header == IPPROTO_UDP )
                {
                    struct udphdr * udp = (struct udphdr *) cursor;
                    const struct rocketnet_server_xdp_config_t * config = server_xdp_config();
                    if ( config != NULL && server_xdp_udp_is_ours( udp, data_end, config, server_xdp_ipv6_address_match( config, ip6 ) ) )
                    {
                        debug_printf( "ipv6 extension header in front of a datagram to this server" );
                        server_xdp_count( ROCKETNET_SERVER_XDP_COUNTER_DROPPED_IP_OPTIONS_OR_EXTENSION );
                        return XDP_DROP;
                    }
                }

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

char _license[] SEC( "license" ) = "GPL";

#endif // #if defined(__linux__) && defined(__BPF__)
