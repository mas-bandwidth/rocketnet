/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The packet side of the XDP kernel tier: crafts the negative corpus, judges
// frames against the standard, sends a corpus onto a veth, captures traffic,
// and compares what arrived on the AF_XDP socket with what the manifest says.
//
//     xdp_vm_replay craft OUTDIR                        write the crafted pcaps, print their manifest rows
//     xdp_vm_replay classify FILE.pcap                  print manifest rows for a recorded capture
//     xdp_vm_replay send IFNAME MANIFEST PCAPDIR        send every manifest frame through AF_PACKET
//     xdp_vm_replay capture IFNAME OUT.pcap PORT MAX STOPFILE
//                                                       record inbound UDP to PORT until STOPFILE or MAX frames
//     xdp_vm_replay compare MANIFEST PCAPDIR RECEIVED.pcap COUNTERS
//                                                       the redirect frames arrived byte-identical, in order,
//                                                       and the counters equal the manifest totals
//
// The judge in classify() is written from STANDARD.md (The Prefilter Header,
// Structural checks, the Packet sizes table) and from the filter's documented
// pass branches; it deliberately does not use ROCKETNET_WIRE_SERVER_PACKET_VALID,
// so the manifest is an outside judge of the program, not a copy of it.
//
// send and capture open AF_PACKET sockets and run only inside the guest that
// run.sh boots; craft, classify and compare touch no socket.

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#define FRAME_CAPACITY 2048
#define MAX_FRAMES 4096
#define MAX_ROWS 4096

// --------------------------------------------------------------------------
// the classic pcap file: a 24-byte header, then 16-byte record headers

struct pcap_t
{
    int count;
    int size[MAX_FRAMES];
    uint8_t * data[MAX_FRAMES];
};

static uint32_t read32( const uint8_t * p, int swap )
{
    uint32_t v = (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
    if ( swap )
        v = ( v >> 24 ) | ( ( v >> 8 ) & 0xFF00 ) | ( ( v << 8 ) & 0xFF0000 ) | ( v << 24 );
    return v;
}

static int pcap_read( const char * path, struct pcap_t * pcap )
{
    memset( pcap, 0, sizeof(*pcap) );
    FILE * file = fopen( path, "rb" );
    if ( !file )
    {
        fprintf( stderr, "could not open %s: %s\n", path, strerror( errno ) );
        return 0;
    }
    uint8_t header[24];
    if ( fread( header, 1, 24, file ) != 24 )
    {
        fprintf( stderr, "%s: short pcap header\n", path );
        fclose( file );
        return 0;
    }
    const uint32_t magic = read32( header, 0 );
    int swap;
    if ( magic == 0xa1b2c3d4 || magic == 0xa1b23c4d )
        swap = 0;
    else if ( magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1 )
        swap = 1;
    else
    {
        fprintf( stderr, "%s: not a classic pcap file\n", path );
        fclose( file );
        return 0;
    }
    if ( read32( header + 20, swap ) != 1 )
    {
        fprintf( stderr, "%s: link type is not ethernet\n", path );
        fclose( file );
        return 0;
    }
    uint8_t record[16];
    while ( fread( record, 1, 16, file ) == 16 )
    {
        const uint32_t length = read32( record + 8, swap );
        if ( length > FRAME_CAPACITY || pcap->count == MAX_FRAMES )
        {
            fprintf( stderr, "%s: frame %d is too large or the file has too many frames\n", path, pcap->count );
            fclose( file );
            return 0;
        }
        uint8_t * data = (uint8_t*) malloc( length ? length : 1 );
        if ( !data || fread( data, 1, length, file ) != length )
        {
            fprintf( stderr, "%s: truncated frame %d\n", path, pcap->count );
            free( data );
            fclose( file );
            return 0;
        }
        pcap->data[pcap->count] = data;
        pcap->size[pcap->count] = (int) length;
        pcap->count++;
    }
    fclose( file );
    return 1;
}

static void write32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) v; p[1] = (uint8_t) ( v >> 8 ); p[2] = (uint8_t) ( v >> 16 ); p[3] = (uint8_t) ( v >> 24 );
}

static FILE * pcap_create( const char * path )
{
    FILE * file = fopen( path, "wb" );
    if ( !file )
    {
        fprintf( stderr, "could not create %s: %s\n", path, strerror( errno ) );
        return NULL;
    }
    uint8_t header[24];
    memset( header, 0, sizeof(header) );
    write32( header, 0xa1b2c3d4 );
    header[4] = 2; header[6] = 4;               // version 2.4, little endian fields
    write32( header + 16, 65535 );              // snaplen
    write32( header + 20, 1 );                  // ethernet
    fwrite( header, 1, sizeof(header), file );
    return file;
}

// timestamps are zero so a crafted file is the same bytes on every run

static void pcap_append( FILE * file, const uint8_t * data, int size )
{
    uint8_t record[16];
    memset( record, 0, sizeof(record) );
    write32( record + 8, (uint32_t) size );
    write32( record + 12, (uint32_t) size );
    fwrite( record, 1, sizeof(record), file );
    fwrite( data, 1, (size_t) size, file );
}

// --------------------------------------------------------------------------
// the network the corpus lives on: the server end of the veth, the client
// end, the server's port, and the published development filter value at
// epoch 0. run.sh, guest.sh and manifest.tsv's #config line use the same.

static const uint8_t server_ipv4[4] = { 10, 211, 0, 1 };
static const uint8_t client_ipv4[4] = { 10, 211, 0, 2 };
static const uint8_t other_ipv4[4] = { 10, 211, 0, 3 };
static const uint8_t server_ipv6[16] = { 0xfd, 0x00, 0x02, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01 };
static const uint8_t client_ipv6[16] = { 0xfd, 0x00, 0x02, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02 };
static const uint8_t other_ipv6[16] = { 0xfd, 0x00, 0x02, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x03 };
static const uint16_t server_port = 40000;
static const uint8_t filter_value[4] = { 0xb7, 0x59, 0x01, 0x00 };
static const uint8_t filter_epoch = 0;

// --------------------------------------------------------------------------
// the judge, from STANDARD.md

#define RECEIVE_CEILING 1472                    // Packet sizes: 1500 - 20 - 8, fixed for every receiver
#define SMALLEST_INBOUND 33                     // disconnect and ack, the smallest server-inbound floor

struct verdict_t
{
    const char * verdict;                       // XDP_PASS, XDP_DROP, XDP_REDIRECT
    const char * counter;                       // a counter name, or "-"
};

// The packet size table: the types a server receives, each at its fixed size
// or its range. DENIED (1), CHALLENGE (2) and COOKIE (7) travel server to
// client only; 10 and up are reserved. A conforming server refuses them all.

static int standard_server_accepts( int type, int bytes )
{
    switch ( type )
    {
        case 0: return bytes == 710;                                    // connection request
        case 3: return bytes == 50;                                     // connection response
        case 4: return bytes >= 41 && bytes <= RECEIVE_CEILING;         // keep-alive
        case 5: return bytes >= 34 && bytes <= RECEIVE_CEILING;         // payload
        case 6: return bytes >= 33 && bytes <= RECEIVE_CEILING;         // disconnect
        case 8: return bytes >= 33 && bytes <= RECEIVE_CEILING;         // ack
        case 9: return bytes >= 40 && bytes <= RECEIVE_CEILING;         // payload fragment
        default: return 0;
    }
}

static struct verdict_t judge_udp( const uint8_t * packet, int bytes )
{
    struct verdict_t v = { "XDP_DROP", "-" };
    if ( bytes < SMALLEST_INBOUND )             { v.counter = "DROPPED_TOO_SMALL"; return v; }
    if ( bytes > RECEIVE_CEILING )              { v.counter = "DROPPED_TOO_LARGE"; return v; }
    if ( packet[1] != filter_epoch )            { v.counter = "DROPPED_WRONG_EPOCH"; return v; }
    if ( memcmp( packet + 2, filter_value, 4 ) != 0 ) { v.counter = "DROPPED_WRONG_FILTER"; return v; }
    if ( !standard_server_accepts( packet[0], bytes ) ) { v.counter = "DROPPED_INVALID_TYPE"; return v; }
    v.verdict = "XDP_REDIRECT";
    v.counter = "REDIRECTED";
    return v;
}

static int is_ipv6_extension( int next_header )
{
    return next_header == 0 || next_header == 43 || next_header == 44 || next_header == 50 || next_header == 51 || next_header == 60;
}

static struct verdict_t classify( const uint8_t * frame, int size )
{
    struct verdict_t pass = { "XDP_PASS", "-" };
    struct verdict_t drop = { "XDP_DROP", "-" };
    if ( size < 14 )
        return pass;
    const int ethertype = ( frame[12] << 8 ) | frame[13];
    if ( ethertype == 0x0800 )
    {
        if ( size < 14 + 20 )
            return drop;                        // claims IPv4 and cannot hold its header
        const uint8_t * ip = frame + 14;
        if ( ( ip[0] & 0x0F ) != 5 || ip[9] != 17 )
            return pass;                        // IPv4 options, or not UDP: the kernel stack's business
        if ( size < 14 + 20 + 8 )
            return pass;
        const uint8_t * udp = ip + 20;
        const int port = ( udp[2] << 8 ) | udp[3];
        if ( port != server_port || memcmp( ip + 16, server_ipv4, 4 ) != 0 )
            return pass;
        return judge_udp( udp + 8, size - 14 - 20 - 8 );
    }
    if ( ethertype == 0x86DD )
    {
        if ( size < 14 + 40 )
            return drop;                        // claims IPv6 and cannot hold its header
        const uint8_t * ip = frame + 14;
        const int next_header = ip[6];
        if ( is_ipv6_extension( next_header ) )
        {
            struct verdict_t extension = { "XDP_PASS", "PASSED_IPV6_EXTENSION" };
            return extension;
        }
        if ( next_header != 17 || size < 14 + 40 + 8 )
            return pass;                        // ICMPv6 and neighbour discovery above all
        const uint8_t * udp = ip + 40;
        const int port = ( udp[2] << 8 ) | udp[3];
        if ( port != server_port || memcmp( ip + 24, server_ipv6, 16 ) != 0 )
            return pass;
        return judge_udp( udp + 8, size - 14 - 40 - 8 );
    }
    return pass;                                // ARP, VLAN-tagged and every other ethertype
}

// --------------------------------------------------------------------------
// crafting: each negative frame breaks exactly one rule

struct craft_t
{
    const char * label;
    int ipv6;
    int ethertype;                              // 0 = by family
    int vlan;                                   // put an 802.1Q tag in front of the IP header
    int ihl;                                    // IPv4 header length in words; 0 = 5
    int protocol;                               // IPv4 protocol or IPv6 next header; -1 = UDP
    int destination_other;                      // address the frame to another host
    int port;                                   // 0 = the server's port
    int packet_bytes;
    int type;
    int epoch;
    int bad_filter;
    int truncate_to;                            // cut the frame to this many bytes; 0 = whole
};

static void put16( uint8_t * p, int v )
{
    p[0] = (uint8_t) ( v >> 8 );
    p[1] = (uint8_t) v;
}

static int build( uint8_t * frame, const struct craft_t * c )
{
    memset( frame, 0, FRAME_CAPACITY );
    uint8_t * p = frame;
    static const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x02, 0x01 };
    static const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x02, 0x02 };
    memcpy( p, server_mac, 6 );
    memcpy( p + 6, client_mac, 6 );
    p += 12;
    if ( c->vlan )
    {
        put16( p, 0x8100 );
        put16( p + 2, 7 );                      // vlan 7
        p += 4;
    }
    put16( p, c->ethertype ? c->ethertype : ( c->ipv6 ? 0x86DD : 0x0800 ) );
    p += 2;

    if ( c->ethertype == 0x0806 )
    {
        // an ARP request for the server's address
        put16( p, 1 ); put16( p + 2, 0x0800 ); p[4] = 6; p[5] = 4; put16( p + 6, 1 );
        memcpy( p + 8, client_mac, 6 ); memcpy( p + 14, client_ipv4, 4 ); memcpy( p + 24, server_ipv4, 4 );
        p += 28;
        return (int) ( p - frame );
    }

    const int protocol = c->protocol >= 0 ? c->protocol : 17;
    const int udp_bytes = 8 + c->packet_bytes;
    const int extension_bytes = ( c->ipv6 && is_ipv6_extension( protocol ) ) ? 8 : 0;

    if ( !c->ipv6 )
    {
        const int ihl = c->ihl ? c->ihl : 5;
        p[0] = (uint8_t) ( 0x40 | ihl );
        put16( p + 2, ihl * 4 + udp_bytes );
        p[8] = 64;
        p[9] = (uint8_t) protocol;
        memcpy( p + 12, client_ipv4, 4 );
        memcpy( p + 16, c->destination_other ? other_ipv4 : server_ipv4, 4 );
        if ( ihl > 5 )
            p[20] = 1;                          // a no-operation option, then zeros (end of options)
        p += ihl * 4;
    }
    else
    {
        p[0] = 0x60;
        put16( p + 4, extension_bytes + udp_bytes );
        p[6] = (uint8_t) protocol;
        p[7] = 64;
        memcpy( p + 8, client_ipv6, 16 );
        memcpy( p + 24, c->destination_other ? other_ipv6 : server_ipv6, 16 );
        p += 40;
        if ( extension_bytes )
        {
            p[0] = 17;                          // UDP follows this header
            if ( protocol == 44 )
            {
                p[3] = 0x01;                    // offset 0, more fragments
                p[7] = 0x2A;
            }
            p += extension_bytes;
        }
    }

    if ( protocol == 58 )
    {
        p[0] = 128;                             // ICMPv6 echo request
        put16( p + 4, 1 );
        p += 8;
        return (int) ( p - frame );
    }

    put16( p, 50000 );
    put16( p + 2, c->port ? c->port : server_port );
    put16( p + 4, udp_bytes );
    p += 8;
    if ( c->packet_bytes > 0 ) p[0] = (uint8_t) c->type;
    if ( c->packet_bytes > 1 ) p[1] = (uint8_t) c->epoch;
    if ( c->packet_bytes >= 6 )
    {
        memcpy( p + 2, filter_value, 4 );
        if ( c->bad_filter )
            p[5] ^= 0x5A;
    }
    for ( int i = 6; i < c->packet_bytes; i++ )
        p[i] = (uint8_t) ( i * 7 );             // stands in for the sealed bytes
    p += c->packet_bytes;

    int size = (int) ( p - frame );
    if ( c->truncate_to && c->truncate_to < size )
        size = c->truncate_to;
    return size;
}

//                 label                           v6 eth     vlan ihl proto  other port  bytes type ep bad trunc
static const struct craft_t crafted_ipv4[] =
{
    { "ipv4_payload_smallest",                    0, 0,      0, 0, -1,    0, 0,     34,  5,   0, 0, 0 },
    { "ipv4_payload_at_ceiling",                  0, 0,      0, 0, -1,    0, 0,   1472,  5,   0, 0, 0 },
    { "ipv4_request",                             0, 0,      0, 0, -1,    0, 0,    710,  0,   0, 0, 0 },
    { "ipv4_response",                            0, 0,      0, 0, -1,    0, 0,     50,  3,   0, 0, 0 },
    { "ipv4_too_small",                           0, 0,      0, 0, -1,    0, 0,     32,  6,   0, 0, 0 },
    { "ipv4_too_large",                           0, 0,      0, 0, -1,    0, 0,   1473,  5,   0, 0, 0 },
    { "ipv4_wrong_epoch",                         0, 0,      0, 0, -1,    0, 0,    100,  5,   1, 0, 0 },
    { "ipv4_wrong_filter",                        0, 0,      0, 0, -1,    0, 0,    100,  5,   0, 1, 0 },
    { "ipv4_challenge_is_server_to_client",       0, 0,      0, 0, -1,    0, 0,     66,  2,   0, 0, 0 },
    { "ipv4_reserved_type",                       0, 0,      0, 0, -1,    0, 0,    100, 10,   0, 0, 0 },
    { "ipv4_request_one_byte_short",              0, 0,      0, 0, -1,    0, 0,    709,  0,   0, 0, 0 },
    { "ipv4_keep_alive_below_floor",              0, 0,      0, 0, -1,    0, 0,     40,  4,   0, 0, 0 },
    { "ipv4_other_port",                          0, 0,      0, 0, -1,    0, 40001, 100,  5,   0, 0, 0 },
    { "ipv4_wrong_address",                       0, 0,      0, 0, -1,    1, 0,    100,  5,   0, 0, 0 },
    { "ipv4_options",                             0, 0,      0, 6, -1,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv4_not_udp",                             0, 0,      0, 0,  6,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv4_header_truncated",                    0, 0,      0, 0, -1,    0, 0,    100,  5,   0, 0, 24 },
};

static const struct craft_t crafted_ipv6[] =
{
    { "ipv6_payload_smallest",                    1, 0,      0, 0, -1,    0, 0,     34,  5,   0, 0, 0 },
    { "ipv6_payload_at_ceiling",                  1, 0,      0, 0, -1,    0, 0,   1472,  5,   0, 0, 0 },
    { "ipv6_request",                             1, 0,      0, 0, -1,    0, 0,    710,  0,   0, 0, 0 },
    { "ipv6_too_small",                           1, 0,      0, 0, -1,    0, 0,     32,  6,   0, 0, 0 },
    { "ipv6_too_large",                           1, 0,      0, 0, -1,    0, 0,   1473,  5,   0, 0, 0 },
    { "ipv6_wrong_epoch",                         1, 0,      0, 0, -1,    0, 0,    100,  5,   1, 0, 0 },
    { "ipv6_wrong_filter",                        1, 0,      0, 0, -1,    0, 0,    100,  5,   0, 1, 0 },
    { "ipv6_reserved_type",                       1, 0,      0, 0, -1,    0, 0,    100, 10,   0, 0, 0 },
    { "ipv6_other_port",                          1, 0,      0, 0, -1,    0, 40001, 100,  5,   0, 0, 0 },
    { "ipv6_wrong_address",                       1, 0,      0, 0, -1,    1, 0,    100,  5,   0, 0, 0 },
    { "ipv6_hop_by_hop",                          1, 0,      0, 0,  0,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv6_routing",                             1, 0,      0, 0, 43,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv6_fragment",                            1, 0,      0, 0, 44,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv6_destination_options",                 1, 0,      0, 0, 60,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv6_icmpv6",                              1, 0,      0, 0, 58,    0, 0,    100,  5,   0, 0, 0 },
    { "ipv6_header_truncated",                    1, 0,      0, 0, -1,    0, 0,    100,  5,   0, 0, 40 },
};

static const struct craft_t crafted_other[] =
{
    { "vlan_tagged_ipv4",                         0, 0,      1, 0, -1,    0, 0,    100,  5,   0, 0, 0 },
    { "vlan_tagged_ipv6",                         1, 0,      1, 0, -1,    0, 0,    100,  5,   0, 0, 0 },
    { "arp_request",                              0, 0x0806, 0, 0, -1,    0, 0,      0,  0,   0, 0, 0 },
};

static int craft_file( const char * dir, const char * name, const struct craft_t * table, int count )
{
    char path[1024];
    snprintf( path, sizeof(path), "%s/%s", dir, name );
    FILE * file = pcap_create( path );
    if ( !file )
        return 0;
    static uint8_t frame[FRAME_CAPACITY];
    for ( int i = 0; i < count; i++ )
    {
        const int size = build( frame, &table[i] );
        pcap_append( file, frame, size );
        const struct verdict_t v = classify( frame, size );
        printf( "%s\t%d\t0\t%s\t%s\t%s\n", name, i, v.verdict, v.counter, table[i].label );
    }
    fclose( file );
    return 1;
}

static int command_craft( const char * dir )
{
    return craft_file( dir, "crafted_ipv4.pcap", crafted_ipv4, (int) ( sizeof(crafted_ipv4) / sizeof(crafted_ipv4[0]) ) ) &&
           craft_file( dir, "crafted_ipv6.pcap", crafted_ipv6, (int) ( sizeof(crafted_ipv6) / sizeof(crafted_ipv6[0]) ) ) &&
           craft_file( dir, "crafted_other.pcap", crafted_other, (int) ( sizeof(crafted_other) / sizeof(crafted_other[0]) ) ) ? 0 : 1;
}

static const char * base_name( const char * path )
{
    const char * slash = strrchr( path, '/' );
    return slash ? slash + 1 : path;
}

static int command_classify( const char * path )
{
    static struct pcap_t pcap;
    if ( !pcap_read( path, &pcap ) )
        return 1;
    for ( int i = 0; i < pcap.count; i++ )
    {
        const struct verdict_t v = classify( pcap.data[i], pcap.size[i] );
        printf( "%s\t%d\t0\t%s\t%s\trecorded\n", base_name( path ), i, v.verdict, v.counter );
    }
    return 0;
}

// --------------------------------------------------------------------------
// the manifest: file, index, rx queue, verdict, counter, label; # lines are comments

struct row_t
{
    char file[256];
    int index;
    int queue;
    char verdict[32];
    char counter[64];
    char label[128];
};

static int manifest_read( const char * path, struct row_t * rows )
{
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        fprintf( stderr, "could not open %s: %s\n", path, strerror( errno ) );
        return -1;
    }
    char line[1024];
    int count = 0;
    while ( fgets( line, sizeof(line), file ) )
    {
        if ( line[0] == '#' || line[0] == '\n' )
            continue;
        if ( count == MAX_ROWS )
            break;
        struct row_t * row = &rows[count];
        memset( row, 0, sizeof(*row) );
        if ( sscanf( line, "%255[^\t]\t%d\t%d\t%31[^\t]\t%63[^\t\n]\t%127[^\t\n]", row->file, &row->index, &row->queue, row->verdict, row->counter, row->label ) < 5 )
        {
            fprintf( stderr, "%s: malformed row: %s", path, line );
            fclose( file );
            return -1;
        }
        count++;
    }
    fclose( file );
    return count;
}

static struct pcap_t * pcap_cached( const char * dir, const char * name )
{
    static char names[16][256];
    static struct pcap_t * files[16];
    for ( int i = 0; i < 16; i++ )
    {
        if ( files[i] && strcmp( names[i], name ) == 0 )
            return files[i];
        if ( !files[i] )
        {
            char path[1024];
            if ( snprintf( path, sizeof(path), "%s/%s", dir, name ) >= (int) sizeof(path) )
                return NULL;
            files[i] = (struct pcap_t*) calloc( 1, sizeof(struct pcap_t) );
            if ( !files[i] || !pcap_read( path, files[i] ) )
            {
                free( files[i] );
                files[i] = NULL;
                return NULL;
            }
            const size_t length = strnlen( name, sizeof(names[i]) - 1 );
            memcpy( names[i], name, length );
            names[i][length] = '\0';
            return files[i];
        }
    }
    return NULL;
}

static int row_frame( const char * dir, const struct row_t * row, const uint8_t ** data, int * size )
{
    struct pcap_t * pcap = pcap_cached( dir, row->file );
    if ( !pcap || row->index < 0 || row->index >= pcap->count )
    {
        fprintf( stderr, "manifest row %s:%d names no frame\n", row->file, row->index );
        return 0;
    }
    *data = pcap->data[row->index];
    *size = pcap->size[row->index];
    return 1;
}

static struct row_t rows[MAX_ROWS];

#if defined(__linux__)

static int packet_socket( const char * ifname, struct sockaddr_ll * address )
{
    const int fd = socket( AF_PACKET, SOCK_RAW, htons( ETH_P_ALL ) );
    if ( fd < 0 )
    {
        fprintf( stderr, "AF_PACKET socket: %s\n", strerror( errno ) );
        return -1;
    }
    memset( address, 0, sizeof(*address) );
    address->sll_family = AF_PACKET;
    address->sll_protocol = htons( ETH_P_ALL );
    address->sll_ifindex = (int) if_nametoindex( ifname );
    if ( address->sll_ifindex == 0 || bind( fd, (struct sockaddr*) address, sizeof(*address) ) != 0 )
    {
        fprintf( stderr, "could not bind AF_PACKET to %s: %s\n", ifname, strerror( errno ) );
        close( fd );
        return -1;
    }
    return fd;
}

static int command_send( const char * ifname, const char * manifest, const char * dir )
{
    const int count = manifest_read( manifest, rows );
    if ( count < 0 )
        return 1;
    struct sockaddr_ll address;
    const int fd = packet_socket( ifname, &address );
    if ( fd < 0 )
        return 1;
    int sent = 0;
    for ( int i = 0; i < count; i++ )
    {
        const uint8_t * data;
        int size;
        if ( !row_frame( dir, &rows[i], &data, &size ) )
        {
            close( fd );
            return 1;
        }
        if ( sendto( fd, data, (size_t) size, 0, (struct sockaddr*) &address, sizeof(address) ) != size )
        {
            fprintf( stderr, "send of %s:%d failed: %s\n", rows[i].file, rows[i].index, strerror( errno ) );
            close( fd );
            return 1;
        }
        sent++;
        // one frame at a time keeps the receive ring from overflowing, so a
        // missing frame is the filter's doing and never the ring's
        usleep( 2000 );
    }
    close( fd );
    printf( "sent %d\n", sent );
    return 0;
}

static int command_capture( const char * ifname, const char * out, int port, int max_frames, const char * stopfile )
{
    struct sockaddr_ll address;
    const int fd = packet_socket( ifname, &address );
    if ( fd < 0 )
        return 1;
    FILE * file = pcap_create( out );
    if ( !file )
    {
        close( fd );
        return 1;
    }
    static uint8_t frame[FRAME_CAPACITY];
    int captured = 0;
    while ( captured < max_frames && access( stopfile, F_OK ) != 0 )
    {
        struct pollfd p = { fd, POLLIN, 0 };
        if ( poll( &p, 1, 100 ) <= 0 )
            continue;
        struct sockaddr_ll from;
        socklen_t from_size = sizeof(from);
        const ssize_t size = recvfrom( fd, frame, sizeof(frame), 0, (struct sockaddr*) &from, &from_size );
        if ( size <= 0 || from.sll_pkttype == PACKET_OUTGOING )
            continue;
        // inbound UDP to the server's port, either family
        const int ethertype = ( frame[12] << 8 ) | frame[13];
        int destination_port = -1;
        if ( ethertype == 0x0800 && size >= 14 + 20 + 8 && frame[14 + 9] == 17 )
        {
            const int ihl = ( frame[14] & 0x0F ) * 4;
            if ( size >= 14 + ihl + 8 )
                destination_port = ( frame[14 + ihl + 2] << 8 ) | frame[14 + ihl + 3];
        }
        else if ( ethertype == 0x86DD && size >= 14 + 40 + 8 && frame[14 + 6] == 17 )
        {
            destination_port = ( frame[14 + 40 + 2] << 8 ) | frame[14 + 40 + 3];
        }
        if ( destination_port != port )
            continue;
        pcap_append( file, frame, (int) size );
        captured++;
    }
    fclose( file );
    close( fd );
    printf( "captured %d\n", captured );
    return 0;
}

#endif // #if defined(__linux__)

// The redirect frames, in manifest order, against what the AF_XDP socket
// received; then the counters the program kept against the manifest totals.
// COUNTERS holds one "NAME VALUE" per line, as xdp_vm_attach serve writes it.

static int command_compare( const char * manifest, const char * dir, const char * received_path, const char * counters_path )
{
    const int count = manifest_read( manifest, rows );
    if ( count < 0 )
        return 1;
    static struct pcap_t received;
    if ( !pcap_read( received_path, &received ) )
        return 1;

    int failures = 0;
    int next = 0;
    int expected = 0;
    for ( int i = 0; i < count; i++ )
    {
        if ( strcmp( rows[i].verdict, "XDP_REDIRECT" ) != 0 )
            continue;
        expected++;
        const uint8_t * data;
        int size;
        if ( !row_frame( dir, &rows[i], &data, &size ) )
            return 1;
        if ( next >= received.count )
        {
            printf( "  missing on the socket: %s:%d %s\n", rows[i].file, rows[i].index, rows[i].label );
            failures++;
            continue;
        }
        if ( received.size[next] != size || memcmp( received.data[next], data, (size_t) size ) != 0 )
        {
            printf( "  not byte-identical: %s:%d %s (sent %d bytes, socket had %d)\n", rows[i].file, rows[i].index, rows[i].label, size, received.size[next] );
            failures++;
        }
        next++;
    }
    if ( received.count > next )
    {
        printf( "  %d frames arrived on the socket that the manifest does not redirect\n", received.count - next );
        failures++;
    }
    printf( "socket frames: expected %d, received %d\n", expected, received.count );

    // counter totals

    char names[32][64];
    long long want[32];
    int names_count = 0;
    for ( int i = 0; i < count; i++ )
    {
        if ( strcmp( rows[i].counter, "-" ) == 0 )
            continue;
        int j;
        for ( j = 0; j < names_count; j++ )
            if ( strcmp( names[j], rows[i].counter ) == 0 )
                break;
        if ( j == names_count )
        {
            if ( names_count == 32 )
                return 1;
            snprintf( names[names_count], sizeof(names[0]), "%s", rows[i].counter );
            want[names_count++] = 0;
        }
        want[j]++;
    }

    FILE * file = fopen( counters_path, "r" );
    if ( !file )
    {
        printf( "  no counters file %s\n", counters_path );
        return 1;
    }
    char name[64];
    long long value;
    int seen[32];
    memset( seen, 0, sizeof(seen) );
    while ( fscanf( file, "%63s %lld", name, &value ) == 2 )
    {
        int j;
        for ( j = 0; j < names_count; j++ )
            if ( strcmp( names[j], name ) == 0 )
                break;
        const long long expect = j < names_count ? want[j] : 0;
        if ( j < names_count )
            seen[j] = 1;
        if ( value != expect )
        {
            printf( "  counter %s is %lld, the manifest says %lld\n", name, value, expect );
            failures++;
        }
        else
            printf( "  counter %s %lld\n", name, value );
    }
    fclose( file );
    for ( int j = 0; j < names_count; j++ )
    {
        if ( !seen[j] )
        {
            printf( "  counter %s is in the manifest and not in this build of the program\n", names[j] );
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}

static int usage( void )
{
    fprintf( stderr,
             "usage: xdp_vm_replay craft OUTDIR\n"
             "       xdp_vm_replay classify FILE.pcap\n"
             "       xdp_vm_replay send IFNAME MANIFEST PCAPDIR\n"
             "       xdp_vm_replay capture IFNAME OUT.pcap PORT MAX STOPFILE\n"
             "       xdp_vm_replay compare MANIFEST PCAPDIR RECEIVED.pcap COUNTERS\n" );
    return 2;
}

int main( int argc, char ** argv )
{
    if ( argc == 3 && strcmp( argv[1], "craft" ) == 0 )
        return command_craft( argv[2] );
    if ( argc == 3 && strcmp( argv[1], "classify" ) == 0 )
        return command_classify( argv[2] );
#if defined(__linux__)
    if ( argc == 5 && strcmp( argv[1], "send" ) == 0 )
        return command_send( argv[2], argv[3], argv[4] );
    if ( argc == 7 && strcmp( argv[1], "capture" ) == 0 )
        return command_capture( argv[2], argv[3], atoi( argv[4] ), atoi( argv[5] ), argv[6] );
#endif
    if ( argc == 6 && strcmp( argv[1], "compare" ) == 0 )
        return command_compare( argv[2], argv[3], argv[4], argv[5] );
    return usage();
}
