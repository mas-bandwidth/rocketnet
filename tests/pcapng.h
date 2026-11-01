/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A header-only pcapng (draft-ietf-opsawg-pcapng) reader and writer for the
    test corpus under tests/pcapng/ (tests/pcapng/README.md is the design). It
    knows exactly the subset the corpus uses: one little-endian section, an
    Interface Description Block per interface, Enhanced Packet Blocks, and the
    opt_comment option. The writer emits only that subset; the reader refuses
    everything else by name and never reads past its buffer.

    Interface 0 is LINKTYPE_IPV4 (228): each packet a synthetic IPv4 + UDP
    frame around the exact datagram the transport sent. Interface 1 is
    LINKTYPE_USER0 (147): each record one kind byte, then the bytes the
    transport handed that reader.

    No allocation through the rocketnet allocator, no sockets, no clock: the
    buffers are std::vector and the timestamps come from the caller.
*/

#pragma once

#ifndef ROCKETNET_TESTS_PCAPNG_H
#define ROCKETNET_TESTS_PCAPNG_H

#include "resource_file.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#if defined( _WIN32 )
#include <io.h>
#else
#include <dirent.h>
#endif // #if defined( _WIN32 )

#define ROCKETNET_PCAPNG_LINKTYPE_IPV4              228
#define ROCKETNET_PCAPNG_LINKTYPE_USER0             147

#define ROCKETNET_PCAPNG_INTERFACE_WIRE             0
#define ROCKETNET_PCAPNG_INTERFACE_OPENED           1

#define ROCKETNET_PCAPNG_KIND_ACK_BLOCK             1
#define ROCKETNET_PCAPNG_KIND_MESSAGE_PAYLOAD       3

#define ROCKETNET_PCAPNG_MAX_FILE_BYTES             ( 128 * 1024 )
#define ROCKETNET_PCAPNG_MAX_CORPUS_BYTES           ( 512 * 1024 )

#define ROCKETNET_PCAPNG_BLOCK_SHB                  0x0A0D0D0Au
#define ROCKETNET_PCAPNG_BLOCK_IDB                  0x00000001u
#define ROCKETNET_PCAPNG_BLOCK_EPB                  0x00000006u
#define ROCKETNET_PCAPNG_BYTE_ORDER_MAGIC           0x1A2B3C4Du

#define ROCKETNET_PCAPNG_OPT_ENDOFOPT               0
#define ROCKETNET_PCAPNG_OPT_COMMENT                1

#define ROCKETNET_PCAPNG_SNAPLEN                    65535

// the reader's refusals, one name per reason

enum
{
    ROCKETNET_PCAPNG_OK = 0,
    ROCKETNET_PCAPNG_TRUNCATED_BLOCK,               // a block header or body runs past the buffer, or the trailing length disagrees
    ROCKETNET_PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4,
    ROCKETNET_PCAPNG_UNKNOWN_LINKTYPE,              // an interface that is neither 228 nor 147
    ROCKETNET_PCAPNG_FILE_OVER_CAP,                 // more than ROCKETNET_PCAPNG_MAX_FILE_BYTES
    ROCKETNET_PCAPNG_NOT_A_SECTION,                 // the first block is not a little-endian section header, version 1.0
    ROCKETNET_PCAPNG_SECOND_SECTION,                // the corpus is one section per file
    ROCKETNET_PCAPNG_UNKNOWN_BLOCK,                 // a block type outside the corpus subset
    ROCKETNET_PCAPNG_UNKNOWN_INTERFACE,             // a packet naming an interface no IDB described
    ROCKETNET_PCAPNG_BAD_PACKET_LENGTH,             // captured length past the block, or captured != original
    ROCKETNET_PCAPNG_BAD_OPTION,                    // an option runs past its block
    ROCKETNET_PCAPNG_CANNOT_OPEN,                   // the file did not open or read
    ROCKETNET_PCAPNG_NUM_RESULTS
};

inline const char * rocketnet_pcapng_result_name( int result )
{
    switch ( result )
    {
        case ROCKETNET_PCAPNG_OK:                             return "PCAPNG_OK";
        case ROCKETNET_PCAPNG_TRUNCATED_BLOCK:                return "PCAPNG_TRUNCATED_BLOCK";
        case ROCKETNET_PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4: return "PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4";
        case ROCKETNET_PCAPNG_UNKNOWN_LINKTYPE:               return "PCAPNG_UNKNOWN_LINKTYPE";
        case ROCKETNET_PCAPNG_FILE_OVER_CAP:                  return "PCAPNG_FILE_OVER_CAP";
        case ROCKETNET_PCAPNG_NOT_A_SECTION:                  return "PCAPNG_NOT_A_SECTION";
        case ROCKETNET_PCAPNG_SECOND_SECTION:                 return "PCAPNG_SECOND_SECTION";
        case ROCKETNET_PCAPNG_UNKNOWN_BLOCK:                  return "PCAPNG_UNKNOWN_BLOCK";
        case ROCKETNET_PCAPNG_UNKNOWN_INTERFACE:              return "PCAPNG_UNKNOWN_INTERFACE";
        case ROCKETNET_PCAPNG_BAD_PACKET_LENGTH:              return "PCAPNG_BAD_PACKET_LENGTH";
        case ROCKETNET_PCAPNG_BAD_OPTION:                     return "PCAPNG_BAD_OPTION";
        case ROCKETNET_PCAPNG_CANNOT_OPEN:                    return "PCAPNG_CANNOT_OPEN";
        default:                                              return "PCAPNG_UNKNOWN_RESULT";
    }
}

// one packet as the corpus holds it. the comment is the EPB opt_comment, empty
// when absent; on interface 1 it names the receiving end and, for an ack
// block, the packet type ("server 5", "client 8").

struct rocketnet_pcapng_packet_t
{
    int interface_id;
    int linktype;
    uint64_t timestamp_us;
    std::vector<uint8_t> data;
    std::string comment;
};

// -----------------------------------------
//                                the writer
// -----------------------------------------

inline void rocketnet_pcapng_put_u16( std::vector<uint8_t> & out, uint32_t value )
{
    out.push_back( (uint8_t) ( value & 0xFF ) );
    out.push_back( (uint8_t) ( ( value >> 8 ) & 0xFF ) );
}

inline void rocketnet_pcapng_put_u32( std::vector<uint8_t> & out, uint32_t value )
{
    for ( int i = 0; i < 4; i++ )
    {
        out.push_back( (uint8_t) ( ( value >> ( 8 * i ) ) & 0xFF ) );
    }
}

inline void rocketnet_pcapng_pad4( std::vector<uint8_t> & out )
{
    while ( out.size() % 4 )
    {
        out.push_back( 0 );
    }
}

inline void rocketnet_pcapng_close_block( std::vector<uint8_t> & out, size_t start )
{
    rocketnet_pcapng_pad4( out );

    const uint32_t total = (uint32_t) ( out.size() - start + 4 );
    rocketnet_pcapng_put_u32( out, total );
    out[start + 4] = (uint8_t) ( total & 0xFF );
    out[start + 5] = (uint8_t) ( ( total >> 8 ) & 0xFF );
    out[start + 6] = (uint8_t) ( ( total >> 16 ) & 0xFF );
    out[start + 7] = (uint8_t) ( ( total >> 24 ) & 0xFF );
}

// the section header and the two corpus interfaces, in interface order

inline void rocketnet_pcapng_write_header( std::vector<uint8_t> & out )
{
    size_t start = out.size();
    rocketnet_pcapng_put_u32( out, ROCKETNET_PCAPNG_BLOCK_SHB );
    rocketnet_pcapng_put_u32( out, 0 );
    rocketnet_pcapng_put_u32( out, ROCKETNET_PCAPNG_BYTE_ORDER_MAGIC );
    rocketnet_pcapng_put_u16( out, 1 );
    rocketnet_pcapng_put_u16( out, 0 );
    rocketnet_pcapng_put_u32( out, 0xFFFFFFFFu );                 // section length -1: not given
    rocketnet_pcapng_put_u32( out, 0xFFFFFFFFu );
    rocketnet_pcapng_close_block( out, start );

    const int linktypes[2] = { ROCKETNET_PCAPNG_LINKTYPE_IPV4, ROCKETNET_PCAPNG_LINKTYPE_USER0 };

    for ( int i = 0; i < 2; i++ )
    {
        start = out.size();
        rocketnet_pcapng_put_u32( out, ROCKETNET_PCAPNG_BLOCK_IDB );
        rocketnet_pcapng_put_u32( out, 0 );
        rocketnet_pcapng_put_u16( out, (uint32_t) linktypes[i] );
        rocketnet_pcapng_put_u16( out, 0 );
        rocketnet_pcapng_put_u32( out, ROCKETNET_PCAPNG_SNAPLEN );
        rocketnet_pcapng_close_block( out, start );               // no options: if_tsresol defaults to microseconds
    }
}

// one Enhanced Packet Block. returns the block's bytes.

inline size_t rocketnet_pcapng_write_packet( std::vector<uint8_t> & out, int interface_id, uint64_t timestamp_us,
                                             const uint8_t * data, int bytes, const char * comment )
{
    const size_t start = out.size();
    rocketnet_pcapng_put_u32( out, ROCKETNET_PCAPNG_BLOCK_EPB );
    rocketnet_pcapng_put_u32( out, 0 );
    rocketnet_pcapng_put_u32( out, (uint32_t) interface_id );
    rocketnet_pcapng_put_u32( out, (uint32_t) ( timestamp_us >> 32 ) );
    rocketnet_pcapng_put_u32( out, (uint32_t) ( timestamp_us & 0xFFFFFFFFu ) );
    rocketnet_pcapng_put_u32( out, (uint32_t) bytes );
    rocketnet_pcapng_put_u32( out, (uint32_t) bytes );
    if ( bytes > 0 )
    {
        out.insert( out.end(), data, data + bytes );
    }
    rocketnet_pcapng_pad4( out );

    if ( comment && comment[0] )
    {
        const size_t length = strlen( comment );
        rocketnet_pcapng_put_u16( out, ROCKETNET_PCAPNG_OPT_COMMENT );
        rocketnet_pcapng_put_u16( out, (uint32_t) length );
        out.insert( out.end(), (const uint8_t *) comment, (const uint8_t *) comment + length );
        rocketnet_pcapng_pad4( out );
        rocketnet_pcapng_put_u16( out, ROCKETNET_PCAPNG_OPT_ENDOFOPT );
        rocketnet_pcapng_put_u16( out, 0 );
    }

    rocketnet_pcapng_close_block( out, start );

    return out.size() - start;
}

// the bytes one packet block would take, so a capped writer decides before it writes

inline size_t rocketnet_pcapng_packet_block_bytes( int bytes, const char * comment )
{
    size_t total = 28 + ( ( (size_t) bytes + 3 ) & ~(size_t) 3 ) + 4;
    if ( comment && comment[0] )
    {
        total += 4 + ( ( strlen( comment ) + 3 ) & ~(size_t) 3 ) + 4;
    }
    return total;
}

// the synthetic interface-0 frame: an IPv4 header (no options, DF clear, TTL
// 64, a correct header checksum) and a UDP header (checksum 0, which IPv4
// permits) around the datagram. addresses and ports are host order.

inline void rocketnet_pcapng_ipv4_udp_frame( std::vector<uint8_t> & out,
                                             uint32_t source_address, uint16_t source_port,
                                             uint32_t destination_address, uint16_t destination_port,
                                             const uint8_t * datagram, int bytes )
{
    out.clear();

    const uint32_t total = 20 + 8 + (uint32_t) bytes;
    const uint8_t header[20] = {
        0x45,
        0x00,
        (uint8_t) ( total >> 8 ),
        (uint8_t) total,
        0x00,
        0x00,
        0x00,
        0x00,
        64,
        17,
        0x00,
        0x00,
        (uint8_t) ( source_address >> 24 ),
        (uint8_t) ( source_address >> 16 ),
        (uint8_t) ( source_address >> 8 ),
        (uint8_t) source_address,
        (uint8_t) ( destination_address >> 24 ),
        (uint8_t) ( destination_address >> 16 ),
        (uint8_t) ( destination_address >> 8 ),
        (uint8_t) destination_address,
    };
    out.insert( out.end(), header, header + 20 );

    uint32_t sum = 0;
    for ( size_t i = 0; i < 20; i += 2 )
    {
        sum += ( (uint32_t) out[i] << 8 ) | out[i + 1];
    }
    while ( sum >> 16 )
    {
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    }

    const uint16_t checksum = (uint16_t) ~sum;
    out[10] = (uint8_t) ( checksum >> 8 );
    out[11] = (uint8_t) checksum;

    const uint32_t udp_length = 8 + (uint32_t) bytes;
    const uint8_t udp[8] = {
        (uint8_t) ( source_port >> 8 ),
        (uint8_t) source_port,
        (uint8_t) ( destination_port >> 8 ),
        (uint8_t) destination_port,
        (uint8_t) ( udp_length >> 8 ),
        (uint8_t) udp_length,
        0x00,
        0x00,
    };
    out.insert( out.end(), udp, udp + 8 );
    if ( bytes > 0 )
    {
        out.insert( out.end(), datagram, datagram + bytes );
    }
}

// -----------------------------------------
//                                the reader
// -----------------------------------------

inline uint32_t rocketnet_pcapng_get_u16( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

inline uint32_t rocketnet_pcapng_get_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

// Reads a whole file image. Every length is checked against what remains
// before it is used, so a hostile image is refused, never overread. On
// refusal, packets holds what was read before the refusing block and
// *error_offset (when not NULL) the refusing block's offset.

inline int rocketnet_pcapng_read_buffer( const uint8_t * buffer, size_t bytes,
                                         std::vector<rocketnet_pcapng_packet_t> & packets,
                                         size_t * error_offset = NULL )
{
    packets.clear();
    if ( error_offset )
    {
        *error_offset = 0;
    }
    if ( bytes > ROCKETNET_PCAPNG_MAX_FILE_BYTES )
        return ROCKETNET_PCAPNG_FILE_OVER_CAP;

    std::vector<int> interfaces;
    size_t offset = 0;
    bool section = false;

    while ( offset < bytes )
    {
        if ( error_offset )
        {
            *error_offset = offset;
        }

        const size_t remaining = bytes - offset;
        if ( remaining < 12 )
            return ROCKETNET_PCAPNG_TRUNCATED_BLOCK;

        const uint8_t * block = buffer + offset;
        const uint32_t type = rocketnet_pcapng_get_u32( block );
        const uint32_t total = rocketnet_pcapng_get_u32( block + 4 );
        if ( total % 4 )
            return ROCKETNET_PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4;
        if ( total < 12 || total > remaining )
            return ROCKETNET_PCAPNG_TRUNCATED_BLOCK;
        if ( rocketnet_pcapng_get_u32( block + total - 4 ) != total )
            return ROCKETNET_PCAPNG_TRUNCATED_BLOCK;

        const uint8_t * body = block + 8;
        const uint32_t body_bytes = total - 12;

        if ( !section )
        {
            if ( type != ROCKETNET_PCAPNG_BLOCK_SHB || body_bytes < 16 ||
                 rocketnet_pcapng_get_u32( body ) != ROCKETNET_PCAPNG_BYTE_ORDER_MAGIC ||
                 rocketnet_pcapng_get_u16( body + 4 ) != 1 || rocketnet_pcapng_get_u16( body + 6 ) != 0 )
                return ROCKETNET_PCAPNG_NOT_A_SECTION;
            section = true;
        }
        else if ( type == ROCKETNET_PCAPNG_BLOCK_SHB )
        {
            return ROCKETNET_PCAPNG_SECOND_SECTION;
        }
        else if ( type == ROCKETNET_PCAPNG_BLOCK_IDB )
        {
            if ( body_bytes < 8 )
                return ROCKETNET_PCAPNG_TRUNCATED_BLOCK;

            const int linktype = (int) rocketnet_pcapng_get_u16( body );
            if ( linktype != ROCKETNET_PCAPNG_LINKTYPE_IPV4 && linktype != ROCKETNET_PCAPNG_LINKTYPE_USER0 )
                return ROCKETNET_PCAPNG_UNKNOWN_LINKTYPE;
            interfaces.push_back( linktype );
        }
        else if ( type == ROCKETNET_PCAPNG_BLOCK_EPB )
        {
            if ( body_bytes < 20 )
                return ROCKETNET_PCAPNG_TRUNCATED_BLOCK;

            const uint32_t interface_id = rocketnet_pcapng_get_u32( body );
            if ( interface_id >= interfaces.size() )
                return ROCKETNET_PCAPNG_UNKNOWN_INTERFACE;

            const uint64_t high = rocketnet_pcapng_get_u32( body + 4 );
            const uint64_t low = rocketnet_pcapng_get_u32( body + 8 );
            const uint32_t captured = rocketnet_pcapng_get_u32( body + 12 );
            const uint32_t original = rocketnet_pcapng_get_u32( body + 16 );
            const uint32_t room = body_bytes - 20;
            if ( captured != original || captured > room )
                return ROCKETNET_PCAPNG_BAD_PACKET_LENGTH;

            const uint32_t padded = ( captured + 3 ) & ~3u;
            if ( padded > room )
                return ROCKETNET_PCAPNG_BAD_PACKET_LENGTH;

            rocketnet_pcapng_packet_t packet;
            packet.interface_id = (int) interface_id;
            packet.linktype = interfaces[interface_id];
            packet.timestamp_us = ( high << 32 ) | low;
            packet.data.assign( body + 20, body + 20 + captured );

            // options: code u16, length u16, value padded to 4; endofopt ends them

            uint32_t option = 20 + padded;
            while ( option < body_bytes )
            {
                if ( body_bytes - option < 4 )
                    return ROCKETNET_PCAPNG_BAD_OPTION;

                const uint32_t code = rocketnet_pcapng_get_u16( body + option );
                const uint32_t length = rocketnet_pcapng_get_u16( body + option + 2 );
                if ( code == ROCKETNET_PCAPNG_OPT_ENDOFOPT )
                    break;

                const uint32_t value_room = body_bytes - option - 4;
                const uint32_t value_padded = ( length + 3 ) & ~3u;
                if ( value_padded > value_room )
                    return ROCKETNET_PCAPNG_BAD_OPTION;
                if ( code == ROCKETNET_PCAPNG_OPT_COMMENT )
                {
                    packet.comment.assign( (const char *) body + option + 4, length );
                }
                option += 4 + value_padded;
            }

            packets.push_back( packet );
        }
        else
        {
            return ROCKETNET_PCAPNG_UNKNOWN_BLOCK;
        }

        offset += total;
    }

    if ( !section )
        return ROCKETNET_PCAPNG_NOT_A_SECTION;

    return ROCKETNET_PCAPNG_OK;
}

// reads at most the cap plus one byte, so an oversized file is refused by
// name without reading all of it

inline int rocketnet_pcapng_read_file( const std::string & path, std::vector<uint8_t> & image )
{
    image.clear();

    FILE * file = rocketnet_test_fopen( path, "rb" );
    if ( !file )
        return ROCKETNET_PCAPNG_CANNOT_OPEN;

    uint8_t chunk[4096];
    size_t n;
    while ( image.size() <= ROCKETNET_PCAPNG_MAX_FILE_BYTES && ( n = fread( chunk, 1, sizeof( chunk ), file ) ) > 0 )
    {
        image.insert( image.end(), chunk, chunk + n );
    }
    fclose( file );

    return image.size() > ROCKETNET_PCAPNG_MAX_FILE_BYTES ? ROCKETNET_PCAPNG_FILE_OVER_CAP : ROCKETNET_PCAPNG_OK;
}

// The interface-0 frame back to its datagram: IPv4, header length 20, UDP,
// lengths that agree with the frame, a header checksum that verifies.
// Returns 1 with the datagram span and the endpoints (host order), or 0.

inline int rocketnet_pcapng_udp_payload( const std::vector<uint8_t> & frame,
                                         const uint8_t ** datagram, int * bytes,
                                         uint32_t * source_address, uint16_t * source_port,
                                         uint32_t * destination_address, uint16_t * destination_port )
{
    if ( frame.size() < 28 )
        return 0;

    const uint8_t * p = frame.data();
    if ( p[0] != 0x45 || p[9] != 17 )
        return 0;

    const uint32_t total = ( (uint32_t) p[2] << 8 ) | p[3];
    if ( total != frame.size() )
        return 0;

    uint32_t sum = 0;
    for ( int i = 0; i < 20; i += 2 )
    {
        sum += ( (uint32_t) p[i] << 8 ) | p[i + 1];
    }
    while ( sum >> 16 )
    {
        sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    }
    if ( sum != 0xFFFF )
        return 0;

    const uint32_t udp_length = ( (uint32_t) p[24] << 8 ) | p[25];
    if ( udp_length != total - 20 )
        return 0;
    *source_address = ( (uint32_t) p[12] << 24 ) | ( (uint32_t) p[13] << 16 ) | ( (uint32_t) p[14] << 8 ) | p[15];
    *destination_address = ( (uint32_t) p[16] << 24 ) | ( (uint32_t) p[17] << 16 ) | ( (uint32_t) p[18] << 8 ) | p[19];
    *source_port = (uint16_t) ( ( p[20] << 8 ) | p[21] );
    *destination_port = (uint16_t) ( ( p[22] << 8 ) | p[23] );
    *datagram = p + 28;
    *bytes = (int) ( total - 28 );

    return 1;
}

// the regular files of one directory whose names end in .pcapng, sorted

inline void rocketnet_pcapng_list( const std::string & directory, std::vector<std::string> & names )
{
    names.clear();
#if defined( _WIN32 )
    const std::wstring pattern = rocketnet_test_wide_path( directory + "\\*.pcapng" );
    WIN32_FIND_DATAW found;
    const HANDLE handle = FindFirstFileW( pattern.c_str(), &found );
    if ( handle != INVALID_HANDLE_VALUE )
    {
        do
        {
            if ( !( found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
            {
                names.push_back( rocketnet_test_wide_to_utf8( found.cFileName, (int) wcslen( found.cFileName ) ) );
            }
        } while ( FindNextFileW( handle, &found ) );

        FindClose( handle );
    }
#else
    DIR * dir = opendir( directory.c_str() );
    if ( dir )
    {
        struct dirent * entry;
        while ( ( entry = readdir( dir ) ) != NULL )
        {
            const std::string name = entry->d_name;
            if ( name.size() > 7 && name.compare( name.size() - 7, 7, ".pcapng" ) == 0 )
            {
                names.push_back( name );
            }
        }

        closedir( dir );
    }
#endif // #if defined( _WIN32 )
    for ( size_t i = 1; i < names.size(); i++ )
        for ( size_t j = i; j > 0 && names[j] < names[j - 1]; j-- )
            names[j].swap( names[j - 1] );
}

#endif // ROCKETNET_TESTS_PCAPNG_H
