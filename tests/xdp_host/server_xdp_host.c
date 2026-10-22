/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP filter compiled as host C. This file includes the very source the
// library embeds (ROCKETNET_XDP_FILTER_SOURCE, by default
// server_xdp/server_xdp.c), with -D__linux__ -D__BPF__ -ffreestanding and
// this directory first on the include path, so <linux/*.h> and
// <bpf/bpf_helpers.h> resolve to the shim. The helpers and the map storage
// follow the include, where the map variables are in scope.

#ifndef ROCKETNET_XDP_FILTER_SOURCE
#error "ROCKETNET_XDP_FILTER_SOURCE names the filter source to compile"
#endif

#include ROCKETNET_XDP_FILTER_SOURCE

#include "server_xdp_host.h"
#include "rocketnet_xdp_limits.h"

// the host allocator, declared here because the filter's translation unit is
// freestanding

extern void * malloc( __SIZE_TYPE__ size );
extern void free( void * pointer );

static struct rocketnet_server_xdp_config_t server_xdp_host_config;
static struct rocketnet_server_xdp_stats_t server_xdp_host_stats;
static unsigned char server_xdp_host_slots[ROCKETNET_XDP_MAX_QUEUES];
static int server_xdp_host_config_lookup_fails;
static int server_xdp_host_redirects;
static __u64 server_xdp_host_redirect_key;

// The kernel never writes the replay table, so the shim declares no
// bpf_map_update_elem and no bpf_map_delete_elem; the host owns the table
// alone.

static struct rocketnet_server_xdp_replay_key_t server_xdp_host_replay_keys[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static struct rocketnet_server_xdp_replay_value_t server_xdp_host_replay_values[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static int server_xdp_host_replay_count;

static struct rocketnet_server_xdp_replay_value_t * server_xdp_host_replay_find( const void * key )
{
    const struct rocketnet_server_xdp_replay_key_t * k = (const struct rocketnet_server_xdp_replay_key_t*) key;
    for ( int i = 0; i < server_xdp_host_replay_count; i++ )
    {
        if ( __builtin_memcmp( &server_xdp_host_replay_keys[i], k, sizeof(struct rocketnet_server_xdp_replay_key_t) ) == 0 )
            return &server_xdp_host_replay_values[i];
    }
    return NULL;
}

// bpf_map_lookup_elem tests the map first. On the replay hash map the key is
// the 20-byte source key and the result is its entry or NULL, as
// htab_map_lookup_elem in kernel/bpf/hashtab.c returns. On the filter's two
// arrays of one: key 0 is the one element and any other key is out of range,
// NULL, as array_map_lookup_elem in kernel/bpf/arraymap.c returns for an
// index at or past max_entries. The stats map is per-CPU in the kernel; the
// host is one CPU.

static void * bpf_map_lookup_elem( void * map, const void * key )
{
    if ( map == (void*) &server_xdp_replay_map )
        return (void*) server_xdp_host_replay_find( key );
    const __u32 index = *(const __u32*) key;
    if ( index != 0 )
        return NULL;
    if ( map == (void*) &server_xdp_config_map )
        return server_xdp_host_config_lookup_fails ? NULL : (void*) &server_xdp_host_config;
    if ( map == (void*) &server_xdp_stats_map )
        return (void*) &server_xdp_host_stats;
    return NULL;
}

// bpf_redirect_map on the XSKMAP, with the kernel's semantics: the helper
// (bpf_xdp_redirect_map in net/core/filter.c) calls the map's map_redirect,
// which for an XSKMAP is xsk_map_redirect (net/xdp/xskmap.c); that calls
// __bpf_xdp_redirect_map (include/linux/filter.h) with no map-specific
// flags, which
//
//   - returns XDP_ABORTED when flags holds any bit outside the action mask
//     (XDP_ABORTED | XDP_DROP | XDP_PASS | XDP_TX, the low two bits);
//   - looks the key up (__xsk_map_lookup_elem in net/xdp/xskmap.c: NULL at
//     or past max_entries, else the slot's socket, NULL when empty) and, on
//     NULL, returns flags & the action mask as the verdict, so BPF_F_INGRESS
//     (1) yields XDP_DROP;
//   - otherwise records the target and returns XDP_REDIRECT.
//
// It never returns 0 for these flags. The kernel tier (test_xdp_filter,
// BPF_PROG_TEST_RUN) judges the same frames against the real helper.

#define SERVER_XDP_HOST_ACTION_MASK ( (__u64) ( XDP_ABORTED | XDP_DROP | XDP_PASS | XDP_TX ) )

static long bpf_redirect_map( void * map, __u64 key, __u64 flags )
{
    if ( flags & ~SERVER_XDP_HOST_ACTION_MASK )
        return XDP_ABORTED;
    server_xdp_host_redirects++;
    server_xdp_host_redirect_key = key;
    const __u32 index = (__u32) key;
    if ( map != (void*) &server_xdp_socket_map || index >= ROCKETNET_XDP_MAX_QUEUES || !server_xdp_host_slots[index] )
        return (long) ( flags & SERVER_XDP_HOST_ACTION_MASK );
    return XDP_REDIRECT;
}

void rocketnet_xdp_host_reset( void )
{
    __builtin_memset( &server_xdp_host_config, 0, sizeof(server_xdp_host_config) );
    __builtin_memset( &server_xdp_host_stats, 0, sizeof(server_xdp_host_stats) );
    __builtin_memset( server_xdp_host_slots, 0, sizeof(server_xdp_host_slots) );
    server_xdp_host_config_lookup_fails = 0;
    server_xdp_host_redirects = 0;
    server_xdp_host_redirect_key = 0;
    __builtin_memset( server_xdp_host_replay_keys, 0, sizeof(server_xdp_host_replay_keys) );
    __builtin_memset( server_xdp_host_replay_values, 0, sizeof(server_xdp_host_replay_values) );
    server_xdp_host_replay_count = 0;
}

void rocketnet_xdp_host_set_config( const struct rocketnet_server_xdp_config_t * config )
{
    __builtin_memcpy( &server_xdp_host_config, config, sizeof(server_xdp_host_config) );
}

void rocketnet_xdp_host_set_config_lookup_fails( int fails )
{
    server_xdp_host_config_lookup_fails = fails;
}

void rocketnet_xdp_host_set_socket_slot( uint32_t index, int populated )
{
    if ( index < ROCKETNET_XDP_MAX_QUEUES )
        server_xdp_host_slots[index] = populated ? 1 : 0;
}

void rocketnet_xdp_host_set_replay_entry( const struct rocketnet_server_xdp_replay_key_t * key, uint32_t high_water )
{
    struct rocketnet_server_xdp_replay_value_t * entry = server_xdp_host_replay_find( key );
    if ( entry )
    {
        entry->high_water = high_water;
        return;
    }
    // the kernel's hash map refuses an insert past max_entries, so a full
    // table ignores a new key
    if ( server_xdp_host_replay_count >= ROCKETNET_XDP_REPLAY_MAX_ENTRIES )
        return;
    __builtin_memcpy( &server_xdp_host_replay_keys[server_xdp_host_replay_count], key, sizeof(struct rocketnet_server_xdp_replay_key_t) );
    struct rocketnet_server_xdp_replay_value_t * value = &server_xdp_host_replay_values[server_xdp_host_replay_count];
    value->high_water = high_water;
    value->pad = 0;
    server_xdp_host_replay_count++;
}

int rocketnet_xdp_host_get_replay_entry( const struct rocketnet_server_xdp_replay_key_t * key, uint32_t * high_water )
{
    struct rocketnet_server_xdp_replay_value_t * entry = server_xdp_host_replay_find( key );
    if ( entry == NULL )
        return 0;
    *high_water = entry->high_water;
    return 1;
}

int rocketnet_xdp_host_run( const uint8_t * frame, int frame_bytes, uint32_t rx_queue_index )
{
    if ( frame == NULL || frame_bytes < 1 )
        return -1;
    unsigned char * copy = (unsigned char*) malloc( (__SIZE_TYPE__) frame_bytes );
    if ( copy == NULL )
        return -1;
    __builtin_memcpy( copy, frame, (__SIZE_TYPE__) frame_bytes );
    struct xdp_md ctx;
    __builtin_memset( &ctx, 0, sizeof(ctx) );
    ctx.data = (unsigned long) copy;
    ctx.data_end = (unsigned long) ( copy + frame_bytes );
    ctx.rx_queue_index = rx_queue_index;
    const int verdict = server_xdp_filter( &ctx );
    free( copy );
    return verdict;
}

uint64_t rocketnet_xdp_host_counter( int counter )
{
    if ( counter < 0 || counter >= ROCKETNET_SERVER_XDP_NUM_COUNTERS )
        return 0;
    return server_xdp_host_stats.counters[counter];
}

int rocketnet_xdp_host_redirect_calls( void )
{
    return server_xdp_host_redirects;
}

uint64_t rocketnet_xdp_host_last_redirect_key( void )
{
    return server_xdp_host_redirect_key;
}
