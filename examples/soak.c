/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
    Soak: 1000 clients against one server over a lossy link for N seconds,
    checking every message's integrity and order, and measuring server tick
    and client update timings, packet/byte rates, and allocator activity.
    This is a stress demo and profiling harness, not a load claim.
    Usage: ./bin/example_soak [--clients 1000] [--seconds 3] [--seed 1]
                              [--transport loopback] [--out soak-results.tsv]
                              [--pause] [--self-test-corrupt] [--arena-mib N]
           development only: [--allocator-self-test]
*/

#include "shutdown.h"
#include "rocketnet.h"
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#endif

// Uses the message calls declared in sdk/rocketnet.h: start_message,
// finish_message, receive_messages (one batch, no release call) and
// send_packets, with int client indexes.
#define CHECK( cond ) do { if ( !( cond ) ) { printf( "FAIL: %s (line %d)\n", #cond, __LINE__ ); goto cleanup; } } while ( 0 )

// serialize a uint32_t little-endian straight into a started message
static int write_u32( uint8_t * p, uint32_t value )
{
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) ( value >> 8 );
    p[2] = (uint8_t) ( value >> 16 );
    p[3] = (uint8_t) ( value >> 24 );
    return 4;
}

static uint32_t read_u32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

#define MAX_SOAK_CLIENTS ROCKETNET_MAX_CLIENTS
#define BATCH_CLIENTS 250
#define DISCONNECT_BATCH_CLIENTS 64
#define MAX_TICKS 7200

// MSVC deprecates fopen, so this uses fopen_s there
static FILE * soak_fopen( const char * path, const char * mode ) {
#if defined( _MSC_VER )
    FILE * file = NULL;
    return fopen_s( &file, path, mode ) == 0 ? file : NULL;
#else
    return fopen( path, mode );
#endif
}

struct soak_alloc_t { uint64_t live_allocs, live_bytes, peak_bytes, steady_allocs; bool steady; };
static struct soak_alloc_t g_alloc;

// Allocator callbacks also run on workers (including thread-shim cleanup).
// This static lock allocates nothing and protects arena metadata and telemetry.
// Backing allocation/release happens outside SDK calls and outside this lock.
#if defined(_WIN32)
static SRWLOCK g_alloc_mutex = SRWLOCK_INIT;
static void soak_alloc_lock( void ) { AcquireSRWLockExclusive( &g_alloc_mutex ); }
static void soak_alloc_unlock( void ) { ReleaseSRWLockExclusive( &g_alloc_mutex ); }
#else
static pthread_mutex_t g_alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
static void soak_alloc_lock( void ) { if ( pthread_mutex_lock( &g_alloc_mutex ) != 0 ) abort(); }
static void soak_alloc_unlock( void ) { if ( pthread_mutex_unlock( &g_alloc_mutex ) != 0 ) abort(); }
#endif

static void soak_alloc_set_steady( bool steady ) {
    soak_alloc_lock();
    g_alloc.steady = steady;
    soak_alloc_unlock();
}

static struct soak_alloc_t soak_alloc_snapshot( void ) {
    struct soak_alloc_t snapshot;
    soak_alloc_lock();
    snapshot = g_alloc;
    soak_alloc_unlock();
    return snapshot;
}

// The harness owns backing storage until every SDK object and worker is gone.
// Rounded-capacity classes reuse returned blocks across reconnects. No SDK callback
// calls the system allocator, including partial-create failure cleanup. Class and
// free-sequence metadata occupy the beginning of the requested arena and are
// included in its high-water accounting. Sixty-four retained blocks per maximum
// client is a fixed 64,000-entry bound in the ordinary 1,000-client build;
// overflowing it is an allocator invariant failure, never a silently lost block.
#define SOAK_ARENA_CLASSES 1024
#define SOAK_ARENA_FREE_CAPACITY ((size_t)MAX_SOAK_CLIENTS * 64)
struct soak_arena_block_t { size_t bytes; unsigned bucket, live; };
struct soak_arena_class_t { size_t stride; };
struct soak_arena_free_entry_t { size_t offset; unsigned bucket; };
static struct {
    void * allocation;
    uint8_t * base;
    size_t bytes, used;
    size_t metadata_bytes, free_count;
    struct soak_arena_class_t * classes;
    struct soak_arena_free_entry_t * free_entries;
} g_arena;

static bool soak_arena_create( size_t bytes ) {
    const size_t class_bytes = sizeof(struct soak_arena_class_t) * SOAK_ARENA_CLASSES;
    const size_t free_bytes = sizeof(struct soak_arena_free_entry_t) * SOAK_ARENA_FREE_CAPACITY;
    if ( bytes > SIZE_MAX - 63 || class_bytes > SIZE_MAX - free_bytes ||
         class_bytes + free_bytes > SIZE_MAX - 63 || sizeof(struct soak_arena_block_t) > 64 ) return false;
    const size_t metadata_bytes = ( class_bytes + free_bytes + 63 ) & ~(size_t)63;
    if ( metadata_bytes > bytes ) return false;
#if defined(__linux__)
    // Reserve the bounded virtual arena without eagerly committing every page.
    // Used SDK records fault in pages naturally; capacity is not a RAM claim.
    g_arena.allocation = mmap(NULL, bytes + 63, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_arena.allocation == MAP_FAILED) { g_arena.allocation = NULL; return false; }
#else
    g_arena.allocation = malloc(bytes + 63);
    if (!g_arena.allocation) return false;
#endif
    g_arena.base = (uint8_t *) (((uintptr_t)g_arena.allocation + 63) & ~(uintptr_t)63);
    g_arena.bytes = bytes;
    g_arena.metadata_bytes = metadata_bytes;
    g_arena.used = metadata_bytes;
    g_arena.classes = (struct soak_arena_class_t *)g_arena.base;
    g_arena.free_entries = (struct soak_arena_free_entry_t *)(g_arena.base + class_bytes);
    memset(g_arena.base, 0, metadata_bytes);
    return true;
}

static void * soak_malloc( void * ctx, size_t bytes ) {
    (void) ctx;
    if (bytes > SIZE_MAX - 127) return NULL;
    const size_t stride = ((bytes ? bytes : 1) + 127) & ~(size_t)63;
    soak_alloc_lock();
    unsigned bucket = (unsigned)((stride >> 6) * UINT64_C(11400714819323198485)) & (SOAK_ARENA_CLASSES - 1);
    unsigned probes = 0;
    while (g_arena.classes[bucket].stride && g_arena.classes[bucket].stride != stride && ++probes < SOAK_ARENA_CLASSES)
        bucket = (bucket + 1) & (SOAK_ARENA_CLASSES - 1);
    if (probes == SOAK_ARENA_CLASSES) { soak_alloc_unlock(); return NULL; }
    struct soak_arena_class_t * cls = &g_arena.classes[bucket];
    size_t free_index = SIZE_MAX;
    for (size_t i = g_arena.free_count; i > 0; i--) {
        if (g_arena.free_entries[i - 1].bucket == bucket) { free_index = i - 1; break; }
    }
    struct soak_arena_block_t * block = NULL;
    if (free_index != SIZE_MAX) {
        block = (struct soak_arena_block_t *)(g_arena.base + g_arena.free_entries[free_index].offset);
        if (free_index + 1 < g_arena.free_count)
            memmove(&g_arena.free_entries[free_index], &g_arena.free_entries[free_index + 1],
                    (g_arena.free_count - free_index - 1) * sizeof(g_arena.free_entries[0]));
        g_arena.free_count--;
    } else {
        if (stride > g_arena.bytes - g_arena.used) { soak_alloc_unlock(); return NULL; }
        block = (struct soak_arena_block_t *)(g_arena.base + g_arena.used);
        g_arena.used += stride;
        cls->stride = stride;
    }
    block->bytes = bytes;
    block->bucket = bucket;
    block->live = 1;
    g_alloc.live_allocs++;
    g_alloc.live_bytes += (uint64_t)bytes;
    if (g_alloc.live_bytes > g_alloc.peak_bytes) g_alloc.peak_bytes = g_alloc.live_bytes;
    if (g_alloc.steady) g_alloc.steady_allocs++;
    soak_alloc_unlock();
    return (uint8_t *)block + 64;
}

static void soak_free( void * ctx, void * p ) {
    (void) ctx;
    if (!p) return;
    soak_alloc_lock();
    const uintptr_t address = (uintptr_t)p;
    const uintptr_t base = (uintptr_t)g_arena.base;
    if (address < base + 64 || address - base >= g_arena.used || ((address - base) & 63)) abort();
    struct soak_arena_block_t * block = (struct soak_arena_block_t *)((uint8_t *)p - 64);
    if (!block->live || block->bucket >= SOAK_ARENA_CLASSES) abort();
    g_alloc.live_allocs--;
    g_alloc.live_bytes -= block->bytes;
    block->live = 0;
    if (g_arena.free_count == SOAK_ARENA_FREE_CAPACITY) {
        fprintf(stderr, "FAIL: soak arena free sequence capacity %zu exceeded\n", SOAK_ARENA_FREE_CAPACITY);
        fflush(stderr);
        abort();
    }
    g_arena.free_entries[g_arena.free_count].offset = (size_t)((uint8_t *)block - g_arena.base);
    g_arena.free_entries[g_arena.free_count].bucket = block->bucket;
    g_arena.free_count++;
    soak_alloc_unlock();
}

static int soak_arena_release( void ) {
    int ok = 1;
    if (g_arena.allocation) {
#if defined(__linux__)
        if (munmap(g_arena.allocation, g_arena.bytes + 63) != 0) ok = 0;
#else
        free(g_arena.allocation);
#endif
    }
    memset(&g_arena, 0, sizeof(g_arena));
    return ok;
}

#if defined(ROCKETNET_DEV) && ROCKETNET_DEV
// The development-only allocator oracle makes exhaustion cheap and
// deterministic: metadata plus two 64-byte blocks. It proves aligned admission,
// explicit exhaustion, LIFO class reuse, accounting, and reset without adding a
// production API or changing the real soak path.
static int soak_arena_self_test( void ) {
    const size_t class_bytes = sizeof(struct soak_arena_class_t) * SOAK_ARENA_CLASSES;
    const size_t free_bytes = sizeof(struct soak_arena_free_entry_t) * SOAK_ARENA_FREE_CAPACITY;
    const size_t metadata_bytes = ( class_bytes + free_bytes + 63 ) & ~(size_t)63;
    const size_t arena_bytes = metadata_bytes + 256;
    memset(&g_alloc, 0, sizeof(g_alloc));
    if (!soak_arena_create(arena_bytes)) return 1;
    void * first = soak_malloc(NULL, 1);
    void * second = soak_malloc(NULL, 1);
    if (!first || !second || soak_malloc(NULL, 1) != NULL ||
        ((uintptr_t)first & 63) != 0 || ((uintptr_t)second & 63) != 0 ||
        g_arena.used != arena_bytes) return 1;
    soak_free(NULL, first);
    void * reused = soak_malloc(NULL, 1);
    if (reused != first || g_arena.used != arena_bytes) return 1;
    soak_free(NULL, second);
    soak_free(NULL, reused);
    const struct soak_alloc_t before_reset = soak_alloc_snapshot();
    if (before_reset.live_allocs != 0 || before_reset.live_bytes != 0 ||
        g_arena.free_count != 2 || !soak_arena_release()) return 1;

    memset(&g_alloc, 0, sizeof(g_alloc));
    if (!soak_arena_create(arena_bytes) || g_arena.used != metadata_bytes) return 1;
    first = soak_malloc(NULL, 1);
    if (!first || (size_t)((uint8_t *)first - g_arena.base) != metadata_bytes + 64 ||
        g_arena.used != metadata_bytes + 128) return 1;
    soak_free(NULL, first);
    if (!soak_arena_release()) return 1;
    memset(&g_alloc, 0, sizeof(g_alloc));
    printf("allocator self-test: aligned exhaustion reuse accounting reset\nPASS\n");
    return 0;
}
#endif

struct soak_stream_t {
    uint32_t tx_seq[2], rx_ordered_seq, rx_sequenced_max, acked_events;
    bool rx_has_sequenced;
};

static uint8_t soak_byte( uint64_t seed, uint64_t client_idx, uint32_t kind, uint32_t seq, int i ) {
    uint64_t x = seed ^ ( client_idx * UINT64_C( 0x9e3779b185ebca87 ) ) ^
                 ( (uint64_t) kind << 40 ) ^ ( (uint64_t) seq << 16 ) ^ (uint64_t) i;
    return (uint8_t) ( ( x ^ ( x >> 17 ) ^ ( x >> 33 ) ) & 0xffu );
}

static void soak_write_msg( uint8_t * out, int len, uint64_t seed, uint64_t client_idx, uint32_t kind, uint32_t seq ) {
    uint32_t hash = 2166136261u;
    for ( int i = 16; i < len; i++ ) {
        out[i] = soak_byte( seed, client_idx, kind, seq, i );
        hash = ( hash ^ out[i] ) * 16777619u;
    }
    uint8_t * p = out;
    p += write_u32( p, (uint32_t) client_idx );
    p += write_u32( p, ( kind & 0xffu ) | ( (uint32_t) len << 8 ) );
    p += write_u32( p, seq );
    p += write_u32( p, hash );
}

static bool soak_check_msg( const uint8_t * data, int len, uint64_t seed, uint64_t want_client,
                            int ch, struct soak_stream_t * rx, uint64_t * corrupt, uint64_t * gaps ) {
    if ( len < 16 ) { (*corrupt)++; printf( "FAIL: short message client=%" PRIu64 " seq=0\n", want_client ); return false; }
    const uint32_t hdr[4] = { read_u32( data ), read_u32( data + 4 ), read_u32( data + 8 ), read_u32( data + 12 ) };
    uint32_t client_id = hdr[0], kind = hdr[1] & 0xffu, seq = hdr[2], want_hash = hdr[3];
    int msg_len = (int) ( hdr[1] >> 8 );
    uint32_t hash = 2166136261u;
    for ( int i = 16; i < len; i++ ) {
        if ( data[i] != soak_byte( seed, client_id, kind, seq, i ) ) {
            (*corrupt)++;
            printf( "FAIL: corrupt body client=%u seq=%u byte=%d\n", client_id, seq, i );
            return false;
        }
        hash = ( hash ^ data[i] ) * 16777619u;
    }
    if ( client_id != (uint32_t) want_client || ch != (int) kind || msg_len != len || hash != want_hash ) {
        (*corrupt)++;
        printf( "FAIL: header/checksum mismatch client=%u seq=%u\n", client_id, seq );
        return false;
    }
    if ( kind == 1 ) {
        if ( seq != rx->rx_ordered_seq ) {
            (*gaps)++;
            printf( "FAIL: reliable-ordered gap client=%u expected=%u seq=%u\n", client_id, rx->rx_ordered_seq, seq );
            return false;
        }
        rx->rx_ordered_seq = seq + 1;
    } else {
        if ( rx->rx_has_sequenced && seq <= rx->rx_sequenced_max ) {
            (*gaps)++;
            printf( "FAIL: unreliable-sequenced backwards client=%u prev=%u seq=%u\n", client_id, rx->rx_sequenced_max, seq );
            return false;
        }
        rx->rx_has_sequenced = true;
        rx->rx_sequenced_max = seq;
    }
    return true;
}

// the peak resident set so far, in bytes: VmHWM from /proc/self/status on
// Linux, 0 elsewhere (the shutdown check below then has nothing to compare)
static uint64_t soak_peak_rss_bytes( void ) {
    uint64_t kb = 0;
#if defined( __linux__ )
    FILE * f = soak_fopen( "/proc/self/status", "r" );
    char line[256];
    if ( f ) {
        while ( fgets( line, sizeof( line ), f ) )
            if ( strncmp( line, "VmHWM:", 6 ) == 0 ) kb = (uint64_t) strtoull( line + 6, NULL, 10 );
        fclose( f );
    }
#endif
    return kb * 1024;
}

// The lifecycle's cost, timed by phase: create, start, connect, and the
// shutdown, which must never need more memory than the run did.
struct soak_lifecycle_t {
    double server_create_ms, server_start_ms, clients_create_ms;
    double clients_destroy_ms, server_stop_ms, server_destroy_ms;
    uint64_t running_peak_rss, shutdown_peak_rss;
    bool shut_down;
};
static struct soak_lifecycle_t g_life;

// every client stopped and destroyed, then the server stopped, then destroyed
static void soak_shutdown( struct rocketnet_client_t ** clients, int num_clients, struct rocketnet_server_t * server ) {
    g_life.running_peak_rss = soak_peak_rss_bytes();
    double t0 = rocketnet_time();
    if ( clients ) for ( int c = 0; c < num_clients; c++ ) if ( clients[c] ) { rn_cli_client_stop_and_destroy( clients[c] ); clients[c] = NULL; }
    double t1 = rocketnet_time();
    if ( server ) { rocketnet_server_stop( server ); rn_cli_server_wait_stopped( server ); }
    double t2 = rocketnet_time();
    if ( server ) rocketnet_server_destroy( server );
    double t3 = rocketnet_time();
    g_life.clients_destroy_ms = ( t1 - t0 ) * 1000.0;
    g_life.server_stop_ms = ( t2 - t1 ) * 1000.0;
    g_life.server_destroy_ms = ( t3 - t2 ) * 1000.0;
    g_life.shutdown_peak_rss = soak_peak_rss_bytes();
    g_life.shut_down = true;
    printf( "lifecycle: server_create_ms=%.3f server_start_ms=%.3f clients_create_ms=%.3f clients_destroy_ms=%.3f server_stop_ms=%.3f server_destroy_ms=%.3f running_peak_rss=%" PRIu64 " shutdown_peak_rss=%" PRIu64 "\n",
            g_life.server_create_ms, g_life.server_start_ms, g_life.clients_create_ms, g_life.clients_destroy_ms, g_life.server_stop_ms, g_life.server_destroy_ms,
            g_life.running_peak_rss, g_life.shutdown_peak_rss );
    fflush( stdout );
}

static int cmp_double( const void * a, const void * b ) {
    double da = *(const double *) a, db = *(const double *) b;
    return ( da > db ) - ( da < db );
}

int main( int argc, char ** argv ) {
    int num_clients = 1000, seconds = 3, exit_code = 1;
    uint64_t seed = 1, arena_mib = 0;
    const char * transport_name = "loopback";
    const char * out_path = "soak-results.tsv";
    bool pause_start = false, self_test_corrupt = false, inited = false;
#if defined(ROCKETNET_DEV) && ROCKETNET_DEV
    bool allocator_self_test = false;
#endif
    for ( int i = 1; i < argc; i++ ) {
        if ( strcmp( argv[i], "--clients" ) == 0 && i + 1 < argc ) num_clients = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--seconds" ) == 0 && i + 1 < argc ) seconds = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--arena-mib" ) == 0 && i + 1 < argc ) arena_mib = (uint64_t)strtoull(argv[++i], NULL, 10);
        else if ( strcmp( argv[i], "--seed" ) == 0 && i + 1 < argc ) seed = (uint64_t) strtoull( argv[++i], NULL, 10 );
        else if ( strcmp( argv[i], "--transport" ) == 0 && i + 1 < argc ) transport_name = argv[++i];
        else if ( strcmp( argv[i], "--out" ) == 0 && i + 1 < argc ) out_path = argv[++i];
        else if ( strcmp( argv[i], "--pause" ) == 0 ) pause_start = true;
        else if ( strcmp( argv[i], "--self-test-corrupt" ) == 0 ) self_test_corrupt = true;
#if defined(ROCKETNET_DEV) && ROCKETNET_DEV
        else if ( strcmp( argv[i], "--allocator-self-test" ) == 0 ) allocator_self_test = true;
#endif
        else { printf( "FAIL: unknown argument %s\n", argv[i] ); return 1; }
    }
#if defined(ROCKETNET_DEV) && ROCKETNET_DEV
    if ( allocator_self_test ) return soak_arena_self_test();
#endif
    if ( num_clients < 1 || num_clients > MAX_SOAK_CLIENTS || seconds < 1 || seconds * 60 > MAX_TICKS ) return 1;
    if ( pause_start ) {
        int pid = 0, ch = 0;
        char line[64];
        FILE * f = soak_fopen( "/proc/self/stat", "r" );
        if ( f ) { if ( fgets( line, sizeof( line ), f ) ) pid = (int) strtol( line, NULL, 10 ); fclose( f ); }
        printf( "PAUSED pid=%d (press Enter to start)\n", pid );
        fflush( stdout );
        while ( ( ch = getchar() ) != 10 && ch != EOF ) {}
    }
    struct soak_stream_t * c2s = (struct soak_stream_t *) calloc( (size_t) num_clients, sizeof( *c2s ) );
    struct soak_stream_t * s2c = (struct soak_stream_t *) calloc( (size_t) num_clients, sizeof( *s2c ) );
    struct rocketnet_client_t ** clients = (struct rocketnet_client_t **) calloc( (size_t) num_clients, sizeof( *clients ) );
    double * srv_us = (double *) calloc( MAX_TICKS, sizeof( double ) );
    double * cli_us = (double *) calloc( MAX_TICKS, sizeof( double ) );
    struct rocketnet_server_t * server = NULL;
    int * index_client = NULL;
    CHECK( c2s && s2c && clients && srv_us && cli_us );
    if (!arena_mib) arena_mib = 2048 + (uint64_t)num_clients * 128;
    CHECK(arena_mib <= SIZE_MAX / (1024 * 1024));
    CHECK(soak_arena_create((size_t)arena_mib * 1024 * 1024));
    printf("allocator: preallocated arena bytes=%" PRIu64 " metadata_bytes=%" PRIu64 " free_entries=%" PRIu64 " (capacity, not resident bytes)\n",
           (uint64_t)g_arena.bytes, (uint64_t)g_arena.metadata_bytes, (uint64_t)SOAK_ARENA_FREE_CAPACITY);
    CHECK( rocketnet_init( NULL, soak_malloc, soak_free ) == ROCKETNET_STATUS_OK );
    inited = true;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    struct rocketnet_server_config_t s_cfg;
    struct rocketnet_client_config_t c_cfg;
    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &s_cfg );
    s_cfg.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( s_cfg.backend_public_key, dev_backend_public_key, sizeof( s_cfg.backend_public_key ) );
    CHECK( rocketnet_client_default_config( &c_cfg ) == ROCKETNET_STATUS_OK );
    s_cfg.max_clients = num_clients; s_cfg.transport = c_cfg.transport = transport_name;
    s_cfg.per_client_send_kbps = c_cfg.send_kbps = 100000; s_cfg.max_message_bytes = c_cfg.max_message_bytes = 2048;
    s_cfg.num_channels = c_cfg.num_channels = s_cfg.max_fragmented_messages = c_cfg.max_fragmented_messages = 2;
    s_cfg.reliable_messages_in_flight = c_cfg.reliable_messages_in_flight = s_cfg.send_queue_messages = c_cfg.send_queue_messages = 64;
    s_cfg.packets_in_flight = c_cfg.packets_in_flight = s_cfg.unreliable_messages_per_update = c_cfg.unreliable_messages_per_update = s_cfg.max_message_receipts = c_cfg.max_message_receipts = 64;
    s_cfg.receive_message_buffer_bytes = c_cfg.receive_message_buffer_bytes = s_cfg.send_message_buffer_bytes = c_cfg.send_message_buffer_bytes = 32768;
    s_cfg.sequenced_messages_held = c_cfg.sequenced_messages_held = 8;
    s_cfg.message_resend_delay_min_ms = c_cfg.message_resend_delay_min_ms = s_cfg.message_resend_delay_initial_ms = c_cfg.message_resend_delay_initial_ms = 200; s_cfg.message_resend_delay_max_ms = c_cfg.message_resend_delay_max_ms = 500;
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    double phase_start = rocketnet_time();
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &s_cfg, &create_status );
    g_life.server_create_ms = ( rocketnet_time() - phase_start ) * 1000.0;
    // the development network simulator: lose 1% of every packet both ways
    if ( server )
        rocketnet_server_network_simulator( server, 0, 0, 1, 0 );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    phase_start = rocketnet_time();
    const enum rocketnet_status_t start_status = server ? rocketnet_server_start( server ) : ROCKETNET_STATUS_OK;
    g_life.server_start_ms = ( rocketnet_time() - phase_start ) * 1000.0;
    if ( server && start_status != ROCKETNET_STATUS_OK )
    {
        rn_cli_server_stop_and_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    char addr[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    CHECK( rocketnet_server_ipv4_address( server, addr, sizeof( addr ) ) );
    rocketnet_server_public_key( server, server_public_key );
    phase_start = rocketnet_time();
    for ( uint64_t c = 0; c < (uint64_t) num_clients; c++ ) {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        const time_t token_time = time( NULL );
        CHECK( token_time != (time_t) -1 );
        CHECK( rocketnet_create_dev_connect_token( addr, NULL, server_public_key, 30, 10, c + 1, EXAMPLE_PROTOCOL_ID, (uint64_t) token_time, NULL, token ) == ROCKETNET_STATUS_OK );
        enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
        clients[c] = rocketnet_client_create( NULL, &c_cfg, &create_status_2 );
        if ( clients[c] )
            rocketnet_client_network_simulator( clients[c], 0, 0, 1, 0 );
        if ( !clients[c] ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
        if ( clients[c] && rocketnet_client_connect( clients[c], token ) != ROCKETNET_STATUS_OK )
        {
            rn_cli_client_stop_and_destroy( clients[c] );
            clients[c] = NULL;
        }
        CHECK( clients[c] != NULL );
    }
    g_life.clients_create_ms = ( rocketnet_time() - phase_start ) * 1000.0;
    // the handshake crosses the client and server worker threads, which need
    // real time to run: frames with no real time between them can finish
    // before a reply arrives. The connect is bounded by real time, like the
    // shutdown below, and the game clock follows it.
    double sim_time = 1000.0;
    const double connect_started = rocketnet_time();
    const double connect_sim_start = sim_time;
    while ( rocketnet_time() - connect_started < 10.0 ) {
        int all_conn = ( rocketnet_server_num_connected_clients( server ) == num_clients );
        for ( int c = 0; all_conn && c < num_clients; c++ )
            if ( rocketnet_client_state( clients[c] ) != ROCKETNET_CLIENT_STATE_CONNECTED ) all_conn = 0;
        if ( all_conn ) break;
        sim_time = connect_sim_start + ( rocketnet_time() - connect_started );
        for ( int base = 0; base < num_clients; base += BATCH_CLIENTS ) {
            int end = base + BATCH_CLIENTS < num_clients ? base + BATCH_CLIENTS : num_clients;
            for ( int c = base; c < end; c++ ) {
                rocketnet_client_update( clients[c], sim_time );
                rocketnet_client_send_packets( clients[c] ); // send last so this frame's messages leave this frame
            }
            rocketnet_server_update( server, sim_time );
            rocketnet_server_send_packets( server ); // send last so this frame's messages leave this frame
        }
        rocketnet_sleep( 0.001 );
    }
    for ( int c = 0; c < num_clients; c++ )
        CHECK( rocketnet_client_state( clients[c] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    CHECK( rocketnet_server_num_connected_clients( server ) == num_clients );
    // the server receives from one queue across every client, each message
    // naming its sender's client index: this maps an index back to its client
    index_client = (int *) calloc( (size_t) num_clients, sizeof( int ) );
    CHECK( index_client != NULL );
    for ( int c = 0; c < num_clients; c++ )
        index_client[rocketnet_client_index( clients[c] )] = c;
    soak_alloc_set_steady( true );
    const int soak_ticks = seconds * 60, drain_ticks = 90, stride = num_clients > 25 ? ( num_clients / 25 ) : 1;
    uint64_t sent_rel = 0, recv_rel = 0, sent_seq = 0, recv_seq = 0, corrupt = 0, gaps = 0;
    bool injected = false;
    for ( int tick = 0; tick < soak_ticks + drain_ticks; tick++ ) {
        sim_time += 1.0 / 60.0;
        double tick_cli_us = 0.0, tick_srv_us = 0.0;
        for ( int base = 0; base < num_clients; base += BATCH_CLIENTS ) {
            int end = base + BATCH_CLIENTS < num_clients ? base + BATCH_CLIENTS : num_clients;
            double t0 = rocketnet_time();
            for ( int c = base; c < end; c++ ) {
                // (1) update pumps the transport and takes in what arrived
                rocketnet_client_update( clients[c], sim_time );
                // (2) receive every received message, in batches from the one
                // queue: each entry names its channel, and the next batch
                // call or update releases the batch
                const struct rocketnet_message_batch_t * batch = NULL;
                while ( ( batch = rocketnet_client_receive_messages( clients[c] ) ) != NULL ) {
                    for ( int m = 0; m < batch->num_messages; m++ ) {
                        const int ch = batch->channel[m];
                        bool ok = soak_check_msg( batch->data[m], batch->bytes[m], seed, (uint64_t) c, ch, &s2c[c], &corrupt, &gaps );
                        CHECK( ok );
                        if ( ch == 1 ) recv_rel++; else recv_seq++;
                    }
                }
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_client_receipts( clients[c], recs, 16 );
                CHECK( nrec <= 16 );
                for ( int r = 0; r < nrec && r < 16; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ) c2s[c].acked_events++;
                // (3) simulate & (4) queue this frame's outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_client_start_message( clients[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, c2s[c].tx_seq[0]++ );
                    CHECK( rocketnet_client_finish_message( clients[c], 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_client_start_message( clients[c], 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
                        CHECK( p1 != NULL );
                        soak_write_msg( p1, 96, seed, (uint64_t) c, 1, c2s[c].tx_seq[1]++ );
                        if ( self_test_corrupt && !injected ) { p1[32] ^= 0xffu; injected = true; }
                        CHECK( rocketnet_client_finish_message( clients[c], 96 ) != 0 );
                        sent_rel++;
                    }
                }
                // (5) send_packets last so this frame's messages leave this frame
                rocketnet_client_send_packets( clients[c] );
            }
            tick_cli_us += ( rocketnet_time() - t0 ) * 1e6;
            double s0 = rocketnet_time();
            // (1) server update
            rocketnet_server_update( server, sim_time );
            // (2) server receive, in batches from the one queue across every
            // client: each entry names its sender and its channel
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL ) {
                for ( int m = 0; m < batch->num_messages; m++ ) {
                    CHECK( batch->client_index[m] >= 0 && batch->client_index[m] < num_clients );
                    const int c = index_client[batch->client_index[m]];
                    const int ch = batch->channel[m];
                    bool ok = soak_check_msg( batch->data[m], batch->bytes[m], seed, (uint64_t) c, ch, &c2s[c], &corrupt, &gaps );
                    CHECK( ok );
                    if ( ch == 1 ) recv_rel++; else recv_seq++;
                }
            }
            for ( int c = base; c < end; c++ ) {
                const int client_idx = rocketnet_client_index( clients[c] );
                struct rocketnet_receipt_t recs[16];
                int nrec = rocketnet_server_receipts( server, client_idx, recs, 16 );
                CHECK( nrec <= 16 );
                for ( int r = 0; r < nrec && r < 16; r++ ) if ( recs[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ) s2c[c].acked_events++;
                // (3) simulate & (4) server queue outgoing messages
                if ( tick < soak_ticks && ( ( c + tick ) % stride == 0 ) ) {
                    uint8_t * p0 = rocketnet_server_start_message( server, (uint64_t) client_idx, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 48 );
                    CHECK( p0 != NULL );
                    soak_write_msg( p0, 48, seed, (uint64_t) c, 0, s2c[c].tx_seq[0]++ );
                    CHECK( rocketnet_server_finish_message( server, 48 ) != 0 );
                    sent_seq++;
                    if ( ( tick % 6 ) == 0 ) {
                        uint8_t * p1 = rocketnet_server_start_message( server, (uint64_t) client_idx, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 96 );
                        CHECK( p1 != NULL );
                        soak_write_msg( p1, 96, seed, (uint64_t) c, 1, s2c[c].tx_seq[1]++ );
                        CHECK( rocketnet_server_finish_message( server, 96 ) != 0 );
                        sent_rel++;
                    }
                }
            }
            // (5) server send_packets last so this frame's messages leave this frame
            rocketnet_server_send_packets( server );
            tick_srv_us += ( rocketnet_time() - s0 ) * 1e6;
        }
        if ( tick < soak_ticks ) { cli_us[tick] = tick_cli_us; srv_us[tick] = tick_srv_us; }
        // the drain frames let real time pass, so what the worker threads
        // still hold arrives (a few clients run the soak frames in well
        // under a millisecond)
        else rocketnet_sleep( 0.001 );
    }
    soak_alloc_set_steady( false );
    uint64_t acked_total = 0;
    for ( int c = 0; c < num_clients; c++ ) {
        acked_total += c2s[c].acked_events + s2c[c].acked_events;
        CHECK( c2s[c].rx_ordered_seq == c2s[c].tx_seq[1] && s2c[c].rx_ordered_seq == s2c[c].tx_seq[1] );
    }
    struct rocketnet_server_stats_t srv_stats;
    rocketnet_server_stats( server, &srv_stats );
    uint64_t srv_pkts_tx = srv_stats.packets_sent;
    uint64_t srv_pkts_rx = srv_stats.packets_received;
    uint64_t srv_bytes_tx = srv_stats.bytes_sent;
    uint64_t srv_bytes_rx = srv_stats.bytes_received;
    CHECK( srv_stats.receipts_lost == 0 );
    CHECK( srv_stats.messages_dropped_over_budget == 0 );
    CHECK( srv_stats.api_misuses == 0 );
    CHECK( sent_rel > 0 && recv_rel == sent_rel && acked_total == sent_rel && recv_seq > 0 && corrupt == 0 && gaps == 0 );
    // Clients disconnect in batches, each once the batch before it has left
    // and the server has read it. Each client sends a redundant burst of
    // datagrams, and the server's loopback endpoint queues 2048 datagrams
    // between updates (TRANSPORTS.md): every client leaving between two
    // server updates overflows it, and the clients whose bursts are dropped
    // end only on timeout. At most two batches' bursts are queued at once.
    const double shutdown_started = rocketnet_time();
    const double shutdown_sim_start = sim_time;
    int disconnected = 0, next_disconnect = 0, batch_left = 1;
    while ( rocketnet_time() - shutdown_started < 10.0 ) {
        if ( batch_left && next_disconnect < num_clients ) {
            const int end = next_disconnect + DISCONNECT_BATCH_CLIENTS < num_clients ? next_disconnect + DISCONNECT_BATCH_CLIENTS : num_clients;
            for ( int c = next_disconnect; c < end; c++ )
                rocketnet_client_disconnect( clients[c] );
            next_disconnect = end;
        }
        disconnected = 1;
        batch_left = 1;
        sim_time = shutdown_sim_start + ( rocketnet_time() - shutdown_started );
        for ( int c = 0; c < num_clients; c++ ) {
            rocketnet_client_update( clients[c], sim_time );
            if ( c >= next_disconnect || !rocketnet_client_disconnected( clients[c] ) ) {
                disconnected = 0;
                if ( c < next_disconnect ) batch_left = 0;
            }
        }
        // Local completion does not prove that the server received a burst.
        // Keep processing frames until both sides have finished disconnecting.
        rocketnet_server_update( server, sim_time );
        rocketnet_server_send_packets( server );
        if ( disconnected && rocketnet_server_num_connected_clients( server ) == 0 )
            break;
        rocketnet_sleep( 0.001 );
    }
    CHECK( disconnected );
    CHECK( rocketnet_server_num_connected_clients( server ) == 0 );
    soak_shutdown( clients, num_clients, server ); server = NULL;
    rocketnet_term(); inited = false;
    // destroy never needs more memory than the run did (Linux: VmHWM)
    CHECK( g_life.shutdown_peak_rss <= g_life.running_peak_rss );
    const struct soak_alloc_t allocation_stats = soak_alloc_snapshot();
    CHECK( allocation_stats.live_allocs == 0 && allocation_stats.live_bytes == 0 );
    qsort( srv_us, (size_t) soak_ticks, sizeof( double ), cmp_double );
    qsort( cli_us, (size_t) soak_ticks, sizeof( double ), cmp_double );
    double srv_p50 = srv_us[soak_ticks / 2], srv_p99 = srv_us[( soak_ticks * 99 ) / 100], srv_max = srv_us[soak_ticks - 1];
    double cli_p50 = cli_us[soak_ticks / 2], cli_p99 = cli_us[( soak_ticks * 99 ) / 100], cli_max = cli_us[soak_ticks - 1];
    FILE * fp = soak_fopen( out_path, "w" );
    CHECK( fp != NULL );
    const char * m_names[] = { "sent_reliable", "recv_reliable", "acked_reliable", "sent_sequenced", "recv_sequenced", "corrupt", "seq_gaps", "peak_bytes",
                               "server_tick_us_p50", "server_tick_us_p99", "server_tick_us_max", "client_frame_us_p50", "client_frame_us_p99", "client_frame_us_max",
                               "client_us_per_client_p50", "client_us_per_client_p99", "server_pkts_per_s_tx", "server_pkts_per_s_rx", "server_bytes_per_s_tx", "server_bytes_per_s_rx", "allocs_per_tick",
                               "server_create_ms", "server_start_ms", "clients_create_ms", "clients_destroy_ms", "server_stop_ms", "server_destroy_ms", "running_peak_rss", "shutdown_peak_rss" };
    double m_vals[] = { (double) sent_rel, (double) recv_rel, (double) acked_total, (double) sent_seq, (double) recv_seq, (double) corrupt, (double) gaps, (double) allocation_stats.peak_bytes,
                        srv_p50, srv_p99, srv_max, cli_p50, cli_p99, cli_max, cli_p50 / num_clients, cli_p99 / num_clients,
                        (double) srv_pkts_tx / seconds, (double) srv_pkts_rx / seconds, (double) srv_bytes_tx / seconds, (double) srv_bytes_rx / seconds, (double) allocation_stats.steady_allocs / soak_ticks,
                        g_life.server_create_ms, g_life.server_start_ms, g_life.clients_create_ms, g_life.clients_destroy_ms, g_life.server_stop_ms, g_life.server_destroy_ms,
                        (double) g_life.running_peak_rss, (double) g_life.shutdown_peak_rss };
    fprintf( fp, "clients\tseconds\tseed\ttransport\tbuild\tmetric\tvalue\n" );
    const int num_metrics = (int) ( sizeof( m_vals ) / sizeof( m_vals[0] ) );
    for ( int m = 0; m < num_metrics; m++ )
        fprintf( fp, m < 8 || m >= num_metrics - 2 ? "%d\t%d\t%" PRIu64 "\t%s\tRelease\t%s\t%.0f\n" : "%d\t%d\t%" PRIu64 "\t%s\tRelease\t%s\t%.2f\n", num_clients, seconds, seed, transport_name, m_names[m], m_vals[m] );
    fclose( fp );
    printf( "soak: clients=%d seconds=%d seed=%" PRIu64 " transport=%s rel=%" PRIu64 "/%" PRIu64 " seq=%" PRIu64 "/%" PRIu64 " srv_p50=%.1fus srv_p99=%.1fus cli_per_client_p50=%.2fus cli_per_client_p99=%.2fus (budget 100us) allocs/tick=%.2f peak_bytes=%" PRIu64 "\n",
            num_clients, seconds, seed, transport_name, recv_rel, sent_rel, recv_seq, sent_seq,
            srv_p50, srv_p99, cli_p50 / num_clients, cli_p99 / num_clients,
            (double) allocation_stats.steady_allocs / soak_ticks, allocation_stats.peak_bytes );
    exit_code = 0;

cleanup:
    if ( !g_life.shut_down ) soak_shutdown( clients, num_clients, server );
    if ( inited ) rocketnet_term();
    const struct soak_alloc_t final_allocations = soak_alloc_snapshot();
    if (final_allocations.live_allocs || final_allocations.live_bytes) {
        printf("FAIL: arena still has live SDK allocations\n");
        exit_code = 1;
    } else if (g_arena.allocation) {
        const double release_started = rocketnet_time();
        const uint64_t high_water_bytes = (uint64_t)g_arena.used;
        if (!soak_arena_release()) exit_code = 1;
        printf("allocator: backing_release_ms=%.3f high_water_bytes=%" PRIu64 "\n", (rocketnet_time() - release_started) * 1000.0, high_water_bytes);
    }
    free( index_client ); free( clients ); free( c2s ); free( s2c ); free( srv_us ); free( cli_us );
    // PASS after the cleanup, so it stays the last line
    if ( exit_code == 0 )
        printf( "PASS\n" );
    return exit_code;
}
