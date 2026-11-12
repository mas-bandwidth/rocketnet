/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_TESTS_FUNCTIONAL_XDP_TEST_NET_H
#define ROCKETNET_TESTS_FUNCTIONAL_XDP_TEST_NET_H

// The test network of an xdp veth case: a namespace, a veth pair with one end
// in it, an address on each end, and whatever routes and neighbour entries
// the case adds. Every one of them is one per kernel, and the suite runs its
// cases in parallel, so two veth cases in one kernel must not share any of
// them. When they did (fixed names, one subnet), the second case's set-up
// deleted the first case's pair, or its xsk create failed EBUSY on queue 0.
//
// So each case takes its own:
//
// - names from its process id and a hash of its case name: veth x<pid><hh>a
//   and x<pid><hh>b, namespace rnx<pid><hh>. No two live processes share a
//   process id, and a pid is at most 7 digits, so a name is at most 13
//   characters, within IFNAMSIZ's 15. The names are locked
//   (/run/rocketnet-xdp-net-<tag>.lock): two cases that derive one name
//   refuse, naming the collision, rather than take each other's pair down.
//
// - addresses and a route metric from a slot it locks for the life of its
//   process (/run/rocketnet-xdp-net.<slot>.lock): 10.79.<slot>.0/24 and
//   2001:db8:79:<slot>::/64, metric 4242 + slot. The near end is .1, the far
//   end .2, and .3 is a second gateway for a case that moves its route.
//
// A slot's lock file names the namespace and veth its holder made. A process
// that died without its exit handlers (a signal, a sanitizer halt) leaves its
// pair behind, so the next holder of the slot removes them first.

#if defined( __linux__ ) && ROCKETNET_XDP

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define ROCKETNET_XDP_TEST_NET_SLOTS 200
#define ROCKETNET_XDP_TEST_NET_METRIC 4242

struct rocketnet_xdp_test_net_t
{
    int slot;
    int metric;
    char tag[16];            // x<pid><hh>
    char netns[24];          // rnx<pid><hh>
    char near_name[24];      // x<pid><hh>a, in the case's own namespace
    char far_name[24];       // x<pid><hh>b, in netns
    char netns_path[64];     // /var/run/netns/<netns>
    char near4[20];
    char far4[20];
    char third4[20];
    char near6[48];
    char far6[48];
};

// an 8 bit hash of the case name (FNV-1a, folded)
static inline unsigned rocketnet_xdp_test_net_hash( const char * case_name )
{
    uint32_t hash = 2166136261u;
    for ( const char * c = case_name; *c != '\0'; c++ )
    {
        hash ^= (uint8_t) *c;
        hash *= 16777619u;
    }

    return ( hash ^ ( hash >> 8 ) ^ ( hash >> 16 ) ^ ( hash >> 24 ) ) & 0xffu;
}

// the holder written into a lock file: "<pid> <case>"
static inline void rocketnet_xdp_test_net_read_holder( int fd, char * holder, size_t holder_bytes )
{
    const ssize_t bytes = pread( fd, holder, holder_bytes - 1, 0 );
    holder[bytes > 0 ? bytes : 0] = '\0';
    holder[strcspn( holder, "\r\n" )] = '\0';
}

// Takes the case's names and a slot, and removes what an earlier holder of
// either left behind. Returns false with the reason when the names are held
// by another live process or every slot is taken; the caller fails the case
// with it. A second call for the same case in one process returns the same
// network (inline, not static: one table per process, whichever file asks).
inline bool rocketnet_xdp_test_net_take( const char * case_name, struct rocketnet_xdp_test_net_t * net, char * reason, size_t reason_bytes )
{
    static struct rocketnet_xdp_test_net_t taken[4];
    static int num_taken = 0;

    const unsigned pid = (unsigned) getpid();
    const unsigned hash = rocketnet_xdp_test_net_hash( case_name );

    char tag[16];
    snprintf( tag, sizeof( tag ), "x%u%02x", pid, hash );

    for ( int i = 0; i < num_taken; i++ )
    {
        if ( strcmp( taken[i].tag, tag ) == 0 )
        {
            *net = taken[i];
            return true;
        }
    }

    if ( num_taken == (int) ( sizeof( taken ) / sizeof( taken[0] ) ) )
    {
        snprintf( reason, reason_bytes, "xdp test network: one process takes at most %d networks", num_taken );
        return false;
    }

    memset( net, 0, sizeof( *net ) );
    snprintf( net->tag, sizeof( net->tag ), "%s", tag );
    snprintf( net->near_name, sizeof( net->near_name ), "%sa", tag );
    snprintf( net->far_name, sizeof( net->far_name ), "%sb", tag );
    snprintf( net->netns, sizeof( net->netns ), "rn%s", tag );
    snprintf( net->netns_path, sizeof( net->netns_path ), "/var/run/netns/%s", net->netns );

    // the names: held for the life of the process

    char path[96];
    snprintf( path, sizeof( path ), "/run/rocketnet-xdp-net-%s.lock", tag );
    const int name_lock = open( path, O_RDWR | O_CREAT | O_CLOEXEC, 0644 );
    if ( name_lock < 0 )
    {
        snprintf( reason, reason_bytes, "xdp test network: cannot open %s", path );
        return false;
    }

    if ( flock( name_lock, LOCK_EX | LOCK_NB ) != 0 )
    {
        char holder[128];
        rocketnet_xdp_test_net_read_holder( name_lock, holder, sizeof( holder ) );
        close( name_lock );
        snprintf( reason, reason_bytes,
                  "xdp test network name collision: %s (namespace %s, veth %s/%s) is held by %s, and case %s in pid %u derived the same names. "
                  "A case's names come from its process id and case name, so two live cases never derive one name",
                  tag, net->netns, net->near_name, net->far_name, holder[0] ? holder : "(unknown)", case_name, pid );
        return false;
    }

    char text[160];
    int length = snprintf( text, sizeof( text ), "%u %s\n", pid, case_name );
    if ( ftruncate( name_lock, 0 ) != 0 || pwrite( name_lock, text, (size_t) length, 0 ) != (ssize_t) length )
    {
        close( name_lock );
        snprintf( reason, reason_bytes, "xdp test network: cannot write %s", path );
        return false;
    }

    // the slot: the first free one, held for the life of the process

    int slot_lock = -1;
    for ( int slot = 0; slot < ROCKETNET_XDP_TEST_NET_SLOTS; slot++ )
    {
        snprintf( path, sizeof( path ), "/run/rocketnet-xdp-net.%d.lock", slot );
        const int fd = open( path, O_RDWR | O_CREAT | O_CLOEXEC, 0644 );
        if ( fd < 0 )
        {
            continue;
        }

        if ( flock( fd, LOCK_EX | LOCK_NB ) == 0 )
        {
            slot_lock = fd;
            net->slot = slot;
            break;
        }

        close( fd );
    }

    if ( slot_lock < 0 )
    {
        close( name_lock );
        snprintf( reason, reason_bytes, "xdp test network: all %d slots (/run/rocketnet-xdp-net.<slot>.lock) are held", ROCKETNET_XDP_TEST_NET_SLOTS );
        return false;
    }

    // what the slot's last holder made, and anything left under this case's
    // own names, goes before the case makes its own

    char last[128];
    rocketnet_xdp_test_net_read_holder( slot_lock, last, sizeof( last ) );

    char last_netns[32] = "";
    char last_near[32] = "";
    if ( sscanf( last, "%31s %31s", last_netns, last_near ) == 2 && strncmp( last_netns, "rnx", 3 ) == 0 && last_near[0] == 'x' )
    {
        char command[256];
        snprintf( command, sizeof( command ), "ip netns del %s 2>/dev/null; ip link del %s 2>/dev/null", last_netns, last_near );
        if ( system( command ) != 0 )
        {
            // already gone
        }
    }

    {
        char command[256];
        snprintf( command, sizeof( command ), "ip netns del %s 2>/dev/null; ip link del %s 2>/dev/null", net->netns, net->near_name );
        if ( system( command ) != 0 )
        {
            // nothing left from an earlier process with this pid
        }
    }

    length = snprintf( text, sizeof( text ), "%s %s %u %s\n", net->netns, net->near_name, pid, case_name );
    if ( ftruncate( slot_lock, 0 ) != 0 || pwrite( slot_lock, text, (size_t) length, 0 ) != (ssize_t) length )
    {
        close( slot_lock );
        close( name_lock );
        snprintf( reason, reason_bytes, "xdp test network: cannot write the slot %d lock", net->slot );
        return false;
    }

    net->metric = ROCKETNET_XDP_TEST_NET_METRIC + net->slot;
    snprintf( net->near4, sizeof( net->near4 ), "10.79.%d.1", net->slot );
    snprintf( net->far4, sizeof( net->far4 ), "10.79.%d.2", net->slot );
    snprintf( net->third4, sizeof( net->third4 ), "10.79.%d.3", net->slot );
    snprintf( net->near6, sizeof( net->near6 ), "2001:db8:79:%x::1", (unsigned) net->slot );
    snprintf( net->far6, sizeof( net->far6 ), "2001:db8:79:%x::2", (unsigned) net->slot );

    // the two locks stay open until the process exits

    taken[num_taken++] = *net;
    return true;
}

#endif // #if defined( __linux__ ) && ROCKETNET_XDP

#endif // ROCKETNET_TESTS_FUNCTIONAL_XDP_TEST_NET_H
