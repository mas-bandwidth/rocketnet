/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Cases for the survivors of the 2026-11-13 candidate-delta mutation run over
// the Linux XDP transport's pure logic: the /proc/interrupts reader of the
// pre-flight (sdk/rocketnet_server_socket_linux.cpp,
// rocketnet_server_socket_interrupt_next_irq), fed from memory, and the XDP
// filter (sdk/xdp/server_xdp.c) run as host C. Each pins a rule a surviving
// mutant broke with no test noticing: IRQ 0 and INT_MAX are IRQs; a blank
// line ends a line; a line longer than the read is joined wherever the read
// splits it, after a blank or a tab, and the line after it starts clean; a
// token too long to carry is dropped at exactly the read's size; the filter
// reads on behind its own tag even when the tag ends the frame, and an
// untagged server passes a priority tag.
//
// The interrupt text is a memory stream (fmemopen), never /proc; the filter
// runs on bytes in memory. Nothing here reads a clock, opens a socket or
// sleeps.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#define check rocketnet_unit_check

// ----------------------------------------------------------------
// the /proc/interrupts reader
// ----------------------------------------------------------------

#if defined( ROCKETNET_XDP ) && ROCKETNET_XDP

#include "../sdk/rocketnet_server_socket.h"

static char gaps_interrupts_text[65536];
static int gaps_interrupts_length = 0;

static void gaps_interrupts_clear()
{
    gaps_interrupts_length = 0;
    gaps_interrupts_text[0] = '\0';
}

static void gaps_interrupts_append( const char * text )
{
    const int written = snprintf( gaps_interrupts_text + gaps_interrupts_length, sizeof( gaps_interrupts_text ) - (size_t) gaps_interrupts_length, "%s", text );
    check( written >= 0 && gaps_interrupts_length + written < (int) sizeof( gaps_interrupts_text ) );
    gaps_interrupts_length += written;
}

static void gaps_interrupts_repeat( char c, int count )
{
    check( count >= 0 && gaps_interrupts_length + count < (int) sizeof( gaps_interrupts_text ) );
    memset( gaps_interrupts_text + gaps_interrupts_length, c, (size_t) count );
    gaps_interrupts_length += count;
    gaps_interrupts_text[gaps_interrupts_length] = '\0';
}

// every IRQ the reader finds for interface_name in the text so far, in order;
// returns how many (at most max_irqs are kept)
static int gaps_interrupts_read( const char * interface_name, int * irqs, int max_irqs )
{
    FILE * interrupts = fmemopen( gaps_interrupts_text, (size_t) gaps_interrupts_length, "r" );
    check( interrupts );

    int found = 0;
    int irq = -1;
    while ( rocketnet_server_socket_interrupt_next_irq( interrupts, interface_name, &irq ) )
    {
        if ( found < max_irqs )
        {
            irqs[found] = irq;
        }
        found++;
        check( found < 1000 );
    }

    (void) fclose( interrupts );
    return found;
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1079 relational and off_by_one (number > 0, >= 1, < INT_MAX)
static void test_gaps_delta_server_interrupt_irq_range()
{
    int irqs[4] = { -1, -1, -1, -1 };

    gaps_interrupts_clear();
    gaps_interrupts_append( "  0:  12  IO-APIC  2-edge  eth0\n" );
    check( gaps_interrupts_read( "eth0", irqs, 4 ) == 1 && "IRQ 0 was not read as an IRQ line" );
    check( irqs[0] == 0 && "IRQ 0 read as another number" );

    gaps_interrupts_clear();
    gaps_interrupts_append( "2147483647:  12  PCI-MSI  eth0-TxRx-0\n" );
    check( gaps_interrupts_read( "eth0", irqs, 4 ) == 1 && "IRQ INT_MAX was not read as an IRQ line" );
    check( irqs[0] == INT_MAX && "IRQ INT_MAX read as another number" );
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1073 off_by_one (length > 1)
// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1105 off_by_one (carry = 1 at a line end)
static void test_gaps_delta_server_interrupt_lines_start_clean()
{
    int irqs[4] = { -1, -1, -1, -1 };

    // a blank line is a whole line: the IRQ line after it is read from its start

    gaps_interrupts_clear();
    gaps_interrupts_append( "\n  45:  12  PCI-MSI  eth0-TxRx-0\n" );
    check( gaps_interrupts_read( "eth0", irqs, 4 ) == 1 && "an IRQ line after a blank line was not read: the blank line did not end a line" );
    check( irqs[0] == 45 && "the IRQ line after a blank line read the wrong IRQ" );

    // lines with no leading blank: nothing of one line is carried into the next

    gaps_interrupts_clear();
    gaps_interrupts_append( "45: 12 PCI-MSI eth1-TxRx-0\n46: 12 PCI-MSI eth0-TxRx-0\n47: 12 PCI-MSI eth0-TxRx-1\n" );
    check( gaps_interrupts_read( "eth0", irqs, 4 ) == 2 && "the reader found the wrong number of eth0 lines" );
    check( irqs[0] == 46 && irqs[1] == 47 && "a byte of one /proc/interrupts line was carried into the next line's IRQ number" );
}

// One line whose first read (4095 bytes) ends at byte split of the region
// "<s>1234567890<s><token><s>9876543210\n", s the separator (split 0: just
// before it; split at its length: the whole line is one read).
static void gaps_interrupts_split_line( char separator, const char * token, int split )
{
    char region[512];
    const int region_bytes = snprintf( region, sizeof( region ), "%c1234567890%c%s%c9876543210\n", separator, separator, token, separator );
    check( region_bytes > 0 && region_bytes < (int) sizeof( region ) );
    check( split >= 0 && split <= region_bytes );

    gaps_interrupts_clear();
    gaps_interrupts_append( "  45:" );
    gaps_interrupts_repeat( separator, 4095 - split - gaps_interrupts_length );
    gaps_interrupts_append( region );
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1090 delete_plus_minus_one, arithmetic and off_by_one (the walk back to the blank, space and tab)
// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1094 delete_plus_minus_one and off_by_one (judged, judged - 0, judged - 2)
static void test_gaps_delta_server_interrupt_split_at_every_byte()
{
    struct gaps_token_t
    {
        const char * token;
        bool names_eth0;
    };

    static const gaps_token_t tokens[] =
    {
        { "eth0", true },
        { "eth0-TxRx-0", true },
        { "i40e-eth0-TxRx-0", true },
        { "eth0@pci", true },
        { "xeth0", false },
        { "eth00", false },
        { "veth0x", false },
        { "eth1-TxRx-0", false },
    };

    const char separators[2] = { ' ', '\t' };

    for ( int s = 0; s < 2; s++ )
    {
        for ( size_t t = 0; t < sizeof( tokens ) / sizeof( tokens[0] ); t++ )
        {
            const int region_bytes = 1 + 10 + 1 + (int) strlen( tokens[t].token ) + 1 + 10 + 1;
            for ( int split = 0; split <= region_bytes; split++ )
            {
                gaps_interrupts_split_line( separators[s], tokens[t].token, split );

                int irqs[4] = { -1, -1, -1, -1 };
                const int found = gaps_interrupts_read( "eth0", irqs, 4 );
                if ( found != ( tokens[t].names_eth0 ? 1 : 0 ) )
                {
                    printf( "interrupt line split at byte %d of its region, separator %s, token \"%s\": %d IRQs found, want %d\n",
                            split, separators[s] == ' ' ? "space" : "tab", tokens[t].token, found, tokens[t].names_eth0 ? 1 : 0 );
                    fflush( stdout );
                }
                check( found == ( tokens[t].names_eth0 ? 1 : 0 ) && "a /proc/interrupts line split by the read was matched differently from the whole line" );
                if ( tokens[t].names_eth0 )
                {
                    check( irqs[0] == 45 && "a split /proc/interrupts line read the wrong IRQ" );
                }
            }
        }
    }
}

// a token ending in "-eth0" (it names eth0 whole), split by the first read
// split_in_name bytes into "eth0", with in_part of its bytes in the first
// read: carried when those and the blank before them fit 256 bytes, dropped
// when more
static int gaps_interrupts_long_token( char separator, int in_part, int split_in_name )
{
    const int token_bytes = in_part + 4 - split_in_name;
    char token[600];
    check( token_bytes >= 6 && token_bytes < (int) sizeof( token ) );
    memset( token, 'a', (size_t) token_bytes - 5 );
    memcpy( token + token_bytes - 5, "-eth0", 6 );

    gaps_interrupts_clear();
    gaps_interrupts_append( "  45:" );
    gaps_interrupts_repeat( separator, 4095 - in_part - gaps_interrupts_length );
    gaps_interrupts_append( token );
    gaps_interrupts_append( "\n" );

    int irqs[4] = { -1, -1, -1, -1 };
    return gaps_interrupts_read( "eth0", irqs, 4 );
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1116 relational and off_by_one (< 256, <= 255, <= 257, : 1)
// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1094 delete_plus_minus_one and off_by_one (the blank in the carry)
// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1090 off_by_one (line[judged - 2])
static void test_gaps_delta_server_interrupt_carry_bound()
{
    const char separators[2] = { ' ', '\t' };

    for ( int s = 0; s < 2; s++ )
    {
        for ( int split = 1; split < 4; split++ )
        {
            // 255 bytes of the token in the read and its blank: 256, carried

            check( gaps_interrupts_long_token( separators[s], 255, split ) == 1 && "a token with 255 bytes in the read was not carried (the carry bound is 256 with its blank)" );
            check( gaps_interrupts_long_token( separators[s], 254, split ) == 1 && "a token with 254 bytes in the read was not carried" );

            // 256 bytes and its blank: 257, dropped

            check( gaps_interrupts_long_token( separators[s], 256, split ) == 0 && "a token with 256 bytes in the read was carried (the carry bound is 256 with its blank)" );
            check( gaps_interrupts_long_token( separators[s], 400, split ) == 0 && "a token with 400 bytes in the read was carried, or its tail matched alone" );
        }
    }
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1094 off_by_one (judged - 1 : 1)
static void test_gaps_delta_server_interrupt_part_with_no_blank()
{
    // a token longer than a read: a part holds no blank at all, and nothing
    // of it is matched until the line ends. Interface "y": the token is all 'y'

    gaps_interrupts_clear();
    gaps_interrupts_append( "  45: " );
    gaps_interrupts_repeat( 'y', 9000 );
    gaps_interrupts_append( "\n" );

    int irqs[4] = { -1, -1, -1, -1 };
    check( gaps_interrupts_read( "y", irqs, 4 ) == 0 && "a part of one 9000-byte token was matched as a whole token" );
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1064 off_by_one (line[4095], line[4097])
static void test_gaps_delta_server_interrupt_read_size()
{
    // a line of exactly 4095 bytes with its newline is one read: its 300-byte
    // token is matched whole. One byte more and the read splits the line
    // before its newline, where the over-long token is dropped

    char token[301];
    memset( token, 'a', 295 );
    memcpy( token + 295, "-eth0", 6 );

    for ( int line_bytes = 4095; line_bytes <= 4096; line_bytes++ )
    {
        gaps_interrupts_clear();
        gaps_interrupts_append( "  45:" );
        gaps_interrupts_repeat( ' ', line_bytes - 300 - 1 - gaps_interrupts_length );
        gaps_interrupts_append( token );
        gaps_interrupts_append( "\n" );
        check( gaps_interrupts_length == line_bytes );

        int irqs[4] = { -1, -1, -1, -1 };
        const int found = gaps_interrupts_read( "eth0", irqs, 4 );
        if ( line_bytes == 4095 )
        {
            check( found == 1 && "a 4095-byte /proc/interrupts line was not read in one part (the read is 4096 bytes with its terminator)" );
        }
        else
        {
            check( found == 0 && "a 4096-byte /proc/interrupts line was read in one part (the read is 4096 bytes with its terminator)" );
        }
    }
}

// mutation gap: delta sdk/rocketnet_server_socket_linux.cpp:1070 arithmetic (sizeof( line ) + carry)
static void test_gaps_delta_server_interrupt_three_part_line()
{
    // a line of three reads and more, of 200-byte tokens: a token is carried
    // at every split, and every read after the first is bounded by the room
    // left after the carry

    gaps_interrupts_clear();
    gaps_interrupts_append( "  45:" );
    while ( gaps_interrupts_length < 3 * 4096 )
    {
        gaps_interrupts_append( " " );
        gaps_interrupts_repeat( 'b', 200 );
    }
    gaps_interrupts_append( " eth0-TxRx-0\n" );
    gaps_interrupts_append( "  46: 1 eth0-TxRx-1\n" );

    int irqs[4] = { -1, -1, -1, -1 };
    check( gaps_interrupts_read( "eth0", irqs, 4 ) == 2 && "a /proc/interrupts line of three reads was not read whole" );
    check( irqs[0] == 45 && irqs[1] == 46 && "a /proc/interrupts line of three reads read the wrong IRQs" );
}

REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_irq_range );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_lines_start_clean );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_split_at_every_byte );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_carry_bound );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_part_with_no_blank );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_read_size );
REGISTER_UNIT_TEST( test_gaps_delta_server_interrupt_three_part_line );

#endif // #if defined( ROCKETNET_XDP ) && ROCKETNET_XDP

// ----------------------------------------------------------------
// the XDP filter as host C: the 802.1Q tag
// ----------------------------------------------------------------

#if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST

#include "../sdk/rocketnet_wire.h"
#include "xdp_host/server_xdp_host.h"

static const int GAPS_XDP_DROP = 1;
static const int GAPS_XDP_PASS = 2;

// a server on 192.0.2.10:40000 at VLAN vlan_id (0 untagged)
static void gaps_xdp_config( uint16_t vlan_id )
{
    struct rocketnet_server_xdp_config_t config;
    memset( &config, 0, sizeof( config ) );
    const uint8_t address[4] = { 192, 0, 2, 10 };
    memcpy( &config.public_address, address, 4 );
    const uint8_t port[2] = { (uint8_t) ( 40000 >> 8 ), (uint8_t) ( 40000 & 0xFF ) };
    memcpy( &config.public_port, port, 2 );
    const uint8_t vlan[2] = { (uint8_t) ( vlan_id >> 8 ), (uint8_t) ( vlan_id & 0xFF ) };
    memcpy( &config.vlan_id, vlan, 2 );         // network order, as stored

    rocketnet_xdp_host_reset();
    rocketnet_xdp_host_set_config( &config );
    rocketnet_xdp_host_set_socket_slot( 0, 1 );
}

// an Ethernet header and an 802.1Q tag of tag_vlan with the inner ethertype
// IPv4, and nothing after it: 18 bytes, the tag ending the frame
static int gaps_xdp_tag_only_frame( uint8_t * frame, uint16_t tag_vlan, uint16_t inner_ethertype )
{
    memset( frame, 0, 18 );
    const uint8_t server_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t client_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    memcpy( frame, server_mac, 6 );
    memcpy( frame + 6, client_mac, 6 );
    frame[12] = 0x81;
    frame[13] = 0x00;
    frame[14] = (uint8_t) ( tag_vlan >> 8 );
    frame[15] = (uint8_t) ( tag_vlan & 0xFF );
    frame[16] = (uint8_t) ( inner_ethertype >> 8 );
    frame[17] = (uint8_t) ( inner_ethertype & 0xFF );
    return 18;
}

static bool gaps_xdp_no_counter()
{
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        if ( rocketnet_xdp_host_counter( i ) != 0 )
        {
            return false;
        }
    }
    return true;
}

// mutation gap: delta sdk/xdp/server_xdp.c:500 relational (>= data_end)
static void test_gaps_delta_server_xdp_own_tag_ending_the_frame_is_read_on()
{
    // a server on VLAN 42 (and 1, and 256, whatever the byte order): a frame
    // that is its own tag and no more is read on as an untagged frame, whose
    // IP header is short, so it drops; it is not passed as a cut tag

    const uint16_t vlans[3] = { 42, 1, 256 };
    const uint16_t inner[2] = { 0x0800, 0x86DD };
    for ( int v = 0; v < 3; v++ )
    {
        for ( int i = 0; i < 2; i++ )
        {
            gaps_xdp_config( vlans[v] );
            uint8_t frame[18];
            const int length = gaps_xdp_tag_only_frame( frame, vlans[v], inner[i] );
            const int verdict = rocketnet_xdp_host_run( frame, length, 0 );
            if ( verdict != GAPS_XDP_DROP )
            {
                printf( "VLAN %d server, its own tag ending the frame, inner ethertype 0x%04x: verdict %d, want XDP_DROP (%d)\n", vlans[v], inner[i], verdict, GAPS_XDP_DROP );
                fflush( stdout );
            }
            check( verdict == GAPS_XDP_DROP && "a frame ending at the server's own 802.1Q tag was passed as a cut tag, not read on as a short IP frame" );
            check( gaps_xdp_no_counter() );
        }
    }
}

// mutation gap: delta sdk/xdp/server_xdp.c:506 off_by_one (config->vlan_id == 1)
static void test_gaps_delta_server_xdp_untagged_server_passes_a_priority_tag()
{
    // an untagged server passes any tagged frame, a priority tag (VLAN 0) too:
    // it is not read on as if it were the server's own tag

    const uint16_t inner[2] = { 0x0800, 0x86DD };
    for ( int i = 0; i < 2; i++ )
    {
        gaps_xdp_config( 0 );
        uint8_t frame[18];
        const int length = gaps_xdp_tag_only_frame( frame, 0, inner[i] );
        check( rocketnet_xdp_host_run( frame, length, 0 ) == GAPS_XDP_PASS && "an untagged server read a priority-tagged frame (VLAN 0) as its own tag" );
        check( gaps_xdp_no_counter() );
    }
}

REGISTER_UNIT_TEST( test_gaps_delta_server_xdp_own_tag_ending_the_frame_is_read_on );
REGISTER_UNIT_TEST( test_gaps_delta_server_xdp_untagged_server_passes_a_priority_tag );

#endif // #if defined( ROCKETNET_XDP_FILTER_HOST ) && ROCKETNET_XDP_FILTER_HOST
