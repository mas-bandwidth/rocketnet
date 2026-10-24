/* Distributed public-API load. The established same-process load gate remains
 * unchanged; including its C translation unit here shares its payload,
 * timing, resource and shutdown helpers in a separate executable only. */
#define main load_same_process_main
#include "load.c"
#undef main
#include "tools/load_split_schedule.h"
#include "tools/load_split_startup.h"
#include "tools/load_split_barrier.h"
#include "tools/load_split_capture.h"
#include "tools/load_split_verify.h"

#define SPLIT_ARM_MAGIC 0x524E414Du
#define SPLIT_ACK_MAGIC 0x524E414Bu
#define SPLIT_GO_MAGIC 0x524E474Fu
#define SPLIT_GO_ACK_MAGIC 0x524E4741u
#define SPLIT_RUN_MAGIC 0x524E5255u

#define SPLIT_DONE_MAGIC 0x524E444Eu
#define SPLIT_DRAINED_MAGIC 0x524E4452u
#define SPLIT_FINISH_MAGIC 0x524E464Eu
#define SPLIT_FINISH_ACK_MAGIC 0x524E4641u
#define SPLIT_CONFIRM_MAGIC 0x524E4643u
#define SPLIT_CONTROL_CHANNEL 0
#define SPLIT_BARRIER_DEADLINE_S 15.0
#define SPLIT_CONFIRM_GRACE_S 3.0
#define SPLIT_MAX_DURATION_S 3600

/* The load mode. The default offers one message per client per second in
 * each direction, the server ticking at 100 Hz. Full load is the worst case
 * the server budget is measured at: the server ticks at 10 Hz and offers one
 * message to every client every update, and every client updates at 10 Hz
 * and offers one message every update. Proof load offers on full load's
 * clock, 10 Hz per client each way, and each offer is the proof's pair of
 * messages (split_proof_offer); its server ticks at 100 Hz, like the default
 * mode, with the clients' offer slots spread over five ticks. At 10 Hz the
 * server socket's receive buffer (ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE)
 * overflows between updates: 1000 clients at 256 kbps are 32 MB/s inbound. */
static int split_full_load = 0;
static int split_proof_load = 0;

static double split_server_tick_s( void ) { return split_full_load ? 0.100 : 0.010; }
static double split_offer_period_s( void ) { return split_full_load || split_proof_load ? 0.100 : 1.0; }
static int split_rate_hz( void ) { return split_full_load || split_proof_load ? 10 : 1; }
static const char * split_load_name( void ) { return split_proof_load ? "proof" : split_full_load ? "full" : "rate_1hz"; }
static const char * split_payload_label( void ) { return split_proof_load ? "proof" : "32"; }

/* The proof load (docs/LOAD_SPLIT.md, "Proof load"). Every offer, in each
 * direction, is one small message in one of the eight reliability modes and
 * one fragmented message in one of the five reliable modes, each rotating by
 * offer from the client's index: 3200 payload bytes per offer at 10 Hz, an
 * offered 256 kbps per client per direction before packet overhead. Each
 * (mode, size class) stream has its own channel. Payloads are written and
 * checked by tools/load_split_verify.h, which owns the rotation
 * (split_verify_small_mode, split_verify_fragmented_mode). An unreliable mode
 * has no fragmented stream: the SDK sends an unreliable message too large for
 * one packet as its reliable twin. */
#define SPLIT_PROOF_SMALL_BYTES 1200u
#define SPLIT_PROOF_FRAGMENTED_BYTES 2000u
#define SPLIT_PROOF_FIRST_CHANNEL 16
#define SPLIT_PROOF_TARGET_KBPS 256.0
#define SPLIT_PROOF_RECEIPT_BATCH 256

/* One client's proof record at one end: the streams it receives, the count
 * it sent on each stream, and its receipt ledger for the direction it sends. */
struct split_proof_t
{
    struct split_verify_stream_t receive[SPLIT_VERIFY_STREAMS_PER_CLIENT];
    uint32_t sent[SPLIT_VERIFY_STREAMS_PER_CLIENT];
    struct split_verify_ledger_t ledger;
    uint32_t offers;
};

struct split_message_t
{
    uint32_t client, combo, seq;
};

struct split_client_t
{
    struct rocketnet_client_t * api;
    struct load_stream_state_t c2s[LOAD_NUM_COMBOS];
    struct load_stream_state_t s2c;
    struct load_wire_sample_t wire_start, wire_end;
    double next_due;
    uint32_t next_combo;
    struct split_startup_t startup;
    int connected, finish, confirmed, disconnect_reported;
    struct split_proof_t * proof;
};

struct split_server_client_t
{
    struct load_stream_state_t c2s[LOAD_NUM_COMBOS];
    struct load_stream_state_t s2c;
    struct load_wire_sample_t wire_start, wire_end;
    int slot, connected, arm_acked, go_acked, done, drained, finish_sent, finish_acked, disconnect_reported;
    double next_due;
    struct split_proof_t * proof;
};

struct split_result_t
{
    uint64_t c2s_offered, c2s_accepted, c2s_received;
    uint64_t s2c_offered, s2c_accepted, s2c_received;
    uint64_t expected_offers, missed_due_slots;
    uint64_t late_application_deliveries;
    uint64_t wire_up, wire_down;
    double max_client_up_kbps, max_client_down_kbps;
    int connected, identity_errors, payload_errors, order_errors, duplicate_errors;
    int duration_target;
    int unexpected_disconnects, offer_refusals, protocol_errors;
    int startup_complete, traffic_complete, drain_complete, barrier_complete, load_complete, wire_valid;
    int barrier_active;
    int timing_valid, timing_budget_met;
    double connect_seconds, start_seconds, traffic_seconds, drain_seconds, barrier_seconds;
    double delivery_deadline;
    double aggregate_up_kbps, aggregate_down_kbps;
    double min_client_up_kbps, min_client_down_kbps;
    int wire_sampled;
    uint64_t proof_checksum_errors, proof_receipt_errors, proof_receipts_dropped;
    int proof_disconnected_clients;
    uint64_t startup_utc_s, traffic_start_utc_s, traffic_end_utc_s;
};

static uint64_t split_read_u64_le( const uint8_t * src )
{
    return (uint64_t) read_u32_le(src) | ((uint64_t) read_u32_le(src + 4) << 32);
}

static void split_write_u64_le( uint8_t * dst, uint64_t value )
{
    write_u32_le(dst, (uint32_t) value);
    write_u32_le(dst + 4, (uint32_t) (value >> 32));
}

static void split_build_message( uint8_t out[LOAD_PAYLOAD_BYTES], uint32_t magic,
                                 uint32_t client, uint32_t combo, uint32_t seq, uint64_t run_id )
{
    write_u32_le(out, magic);
    write_u32_le(out + 4, client);
    write_u32_le(out + 8, combo);
    write_u32_le(out + 12, seq);
    split_write_u64_le(out + 16, run_id);
    for ( int i = 24; i < LOAD_PAYLOAD_BYTES; ++i )
        out[i] = pattern_byte(magic ^ (uint32_t) run_id ^ (uint32_t) (run_id >> 32),
                              client, combo, seq, i);
}

static int split_verify_message( const uint8_t * data, int bytes, uint32_t magic,
                                 uint64_t run_id, struct split_message_t * message )
{
    if ( !data || bytes != LOAD_PAYLOAD_BYTES || read_u32_le(data) != magic ||
         split_read_u64_le(data + 16) != run_id )
        return 0;
    message->client = read_u32_le(data + 4);
    message->combo = read_u32_le(data + 8);
    message->seq = read_u32_le(data + 12);
    for ( int i = 24; i < LOAD_PAYLOAD_BYTES; ++i )
        if ( data[i] != pattern_byte(magic ^ (uint32_t) run_id ^ (uint32_t) (run_id >> 32),
                                      message->client, message->combo, message->seq, i) )
            return 0;
    return 1;
}

static int split_parse_run_id( const char * text, uint64_t * value )
{
    if ( !text || strlen(text) != 16 ) return 0;
    for ( int i = 0; i < 16; ++i )
        if ( !((text[i] >= '0' && text[i] <= '9') ||
               (text[i] >= 'a' && text[i] <= 'f') ||
               (text[i] >= 'A' && text[i] <= 'F')) ) return 0;
    char * end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 16);
    if ( errno || !end || *end || !parsed ) return 0;
    *value = (uint64_t) parsed;
    return 1;
}

static int split_parse_int( const char * text, int low, int high, int * value )
{
    if ( !text || !*text ) return 0;
    char * end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 10);
    if ( errno || !end || *end || parsed < low || parsed > high ) return 0;
    *value = (int) parsed;
    return 1;
}

static uint32_t split_expected_combo_offers( uint32_t client, uint32_t combo, int duration )
{
    const uint32_t offset = (combo + LOAD_NUM_COMBOS - client % LOAD_NUM_COMBOS) % LOAD_NUM_COMBOS;
    return (uint32_t) (duration / LOAD_NUM_COMBOS) +
        (offset < (uint32_t) (duration % LOAD_NUM_COMBOS) ? 1u : 0u);
}

static int split_parse_limit( const char * text, double * value )
{
    if ( !text || !*text ) return 0;
    char * end = NULL;
    errno = 0;
    const double parsed = strtod(text, &end);
    if ( errno || !end || *end || !(parsed > 0.0 && parsed < DBL_MAX) ) return 0;
    *value = parsed;
    return 1;
}

static int split_decode_key( const char * hex, uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES] )
{
    if ( !hex || strlen(hex) != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES ) return 0;
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; ++i )
    {
        const char hi = hex[2 * i], lo = hex[2 * i + 1];
        const int h = hi >= '0' && hi <= '9' ? hi - '0' :
            hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 :
            hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
        const int l = lo >= '0' && lo <= '9' ? lo - '0' :
            lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 :
            lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
        if ( h < 0 || l < 0 ) return 0;
        key[i] = (uint8_t) ((h << 4) | l);
    }
    return 1;
}

static int split_stock_profile_environment( void )
{
#if defined(_WIN32)
    extern char ** _environ;
    char ** variables = _environ;
#else
    extern char ** environ;
    char ** variables = environ;
#endif
    if ( !variables ) return 0;
    for ( char ** item = variables; *item; ++item )
    {
        const char * variable = *item;
        const char * equals = strchr(variable, '=');
        if ( !equals || equals[1] == 0 ) continue;
        const size_t length = (size_t) (equals - variable);
        /* The transport and its core layout (AF_XDP queues and the cores
         * left to the operating system) are deployment placement, not
         * profile: a dedicated XDP server is measured on its own layout. */
        const int server_placement =
            (length == 26 && strncmp(variable, "ROCKETNET_SERVER_TRANSPORT", 26) == 0) ||
            (length == 31 && strncmp(variable, "ROCKETNET_SERVER_NUM_XDP_QUEUES", 31) == 0) ||
            (length == 28 && strncmp(variable, "ROCKETNET_SERVER_NUM_OS_CPUS", 28) == 0);
        if ( (length >= 17 && strncmp(variable, "ROCKETNET_SERVER_", 17) == 0 && !server_placement) ||
             (length >= 17 && strncmp(variable, "ROCKETNET_CLIENT_", 17) == 0 &&
              !(length == 26 && strncmp(variable, "ROCKETNET_CLIENT_TRANSPORT", 26) == 0)) ||
             (length >= 14 && strncmp(variable, "ROCKETNET_DEV_", 14) == 0) )
        {
            fprintf(stderr, "load_split: stock profile refuses ambient RocketNet override %.*s\n",
                    (int) length, variable);
            return 0;
        }
    }
    return 1;
}

static void split_encode_key( const uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES],
                              char hex[2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 1] )
{
    static const char digits[] = "0123456789abcdef";
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; ++i )
    {
        hex[2 * i] = digits[key[i] >> 4];
        hex[2 * i + 1] = digits[key[i] & 15];
    }
    hex[2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES] = 0;
}

static void split_server_config( struct rocketnet_server_config_t * config, int total )
{
    static const uint8_t dev_backend_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    rocketnet_server_default_config(config);
    config->protocol_id = LOAD_PROTOCOL_ID;
    config->max_clients = total;
    config->handshake_deadline_seconds = 10;
    memcpy(config->backend_public_key, dev_backend_key, sizeof(config->backend_public_key));
}

static void split_client_config( struct rocketnet_client_config_t * config )
{
    rocketnet_client_default_config(config);
}

static int split_plan( const char * role, int total, int local )
{
    load_warm_cycle();
    const long fds_pre_init = count_open_fds(), threads_pre_init = count_threads();
    LOAD_CENSUS(LOAD_PRE_INIT);
    const enum rocketnet_status_t init_status = rocketnet_init(NULL, NULL, NULL);
    if ( init_status != ROCKETNET_STATUS_OK ) return 1;
    const long fds_post_init = count_open_fds(), threads_post_init = count_threads();
    LOAD_CENSUS(LOAD_POST_INIT);
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    split_server_config(&server_config, total);
    split_client_config(&client_config);
    const size_t server_bytes = strcmp(role, "server") == 0 ? rocketnet_server_required_memory(&server_config) : 0;
    const size_t client_bytes = strcmp(role, "client-shard") == 0 ? rocketnet_client_required_memory(&client_config) : 0;
    const int valid = (server_bytes || client_bytes) && local >= 0 &&
        (client_bytes == 0 || (size_t) local <= SIZE_MAX / client_bytes);
    const size_t public_bytes = valid ? server_bytes + (size_t) local * client_bytes : 0;
    const long fds_pre_term = count_open_fds(), threads_pre_term = count_threads();
    LOAD_CENSUS(LOAD_PRE_TERM);
    const enum rocketnet_status_t term_status = rocketnet_term();
    const long fds_post_term = count_open_fds(), threads_post_term = count_threads();
    LOAD_CENSUS(LOAD_POST_TERM);
    struct load_census_survivors_t operational, whole;
    int census = 0;
    const int resources = load_resource_verdict(role, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
                                                 threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
                                                 &operational, &whole, &census);
    const int pass = valid && resources && term_status == ROCKETNET_STATUS_OK;
    printf("split_plan: role=%s total=%d local=%d server_public_required_bytes=%zu "
           "client_public_required_bytes=%zu total_public_required_bytes=%zu "
           "scope=create_only transport_memory=excluded native_resources=excluded "
           "fds_pre_init=%ld fds_post_init=%ld fds_pre_term=%ld fds_post_term=%ld "
           "threads_pre_init=%ld threads_post_init=%ld threads_pre_term=%ld threads_post_term=%ld "
           "resource_baseline_schema=%s warm_cycle=1 survivor_threads=%ld survivor_handles=%ld "
           "os_pool_threads=%ld term_status_code=%d verdict=%s\n", role, total, local, server_bytes,
           client_bytes, public_bytes, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
           threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
           census ? "census_v1" : "phase_counts_v1", whole.threads + operational.threads,
           whole.handles + operational.handles, whole.os_pool_threads,
           (int) term_status, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

/* The server main calls reported one line each by split_print_server_calls.
 * The p99 budget for every one of them is 100 microseconds. */
static const enum load_timing_call_t split_server_calls[] =
{
    LOAD_TIMING_SERVER_UPDATE,
    LOAD_TIMING_SERVER_RECEIVE_MESSAGES,
    LOAD_TIMING_SERVER_START_MESSAGE,
    LOAD_TIMING_SERVER_FINISH_MESSAGE,
    LOAD_TIMING_SERVER_SEND_PACKETS,
};

static struct load_timing_t split_untimed;

static int split_server_send( struct rocketnet_server_t * server, int slot, uint32_t magic,
                              uint32_t client, uint32_t combo, uint32_t seq, uint64_t run_id,
                              enum rocketnet_reliability_t reliability, int channel,
                              struct load_timing_t * timing )
{
    if ( !timing ) timing = &split_untimed;
    uint8_t * out = NULL;
    LOAD_TIMED_VALUE(timing, LOAD_TIMING_SERVER_START_MESSAGE, out,
                     rocketnet_server_start_message(server, (uint64_t) slot, channel, reliability, LOAD_PAYLOAD_BYTES));
    if ( !out ) return 0;
    split_build_message(out, magic, client, combo, seq, run_id);
    int finished = 0;
    LOAD_TIMED_VALUE(timing, LOAD_TIMING_SERVER_FINISH_MESSAGE, finished,
                     rocketnet_server_finish_message(server, LOAD_PAYLOAD_BYTES) != 0);
    return finished;
}

/* Receive every batch the last update published, timing each call,
 * including the final call that returns NULL. */
static const struct rocketnet_message_batch_t * split_server_receive( struct rocketnet_server_t * server,
                                                                      struct load_timing_t * timing )
{
    const struct rocketnet_message_batch_t * batch = NULL;
    LOAD_TIMED_VALUE(timing, LOAD_TIMING_SERVER_RECEIVE_MESSAGES, batch, rocketnet_server_receive_messages(server));
    return batch;
}

static void split_print_server_calls( const struct load_timing_t * timing, uint64_t run_id, int total,
                                      const char * transport )
{
    for ( size_t i = 0; i < sizeof(split_server_calls) / sizeof(split_server_calls[0]); ++i )
    {
        const enum load_timing_call_t call = split_server_calls[i];
        printf("split_server_call: run_id=%016" PRIx64 " total=%d transport=%s scope=traffic_plus_drain "
               "load=%s call=%s count=%" PRIu64 " p50_upper_us=%" PRIu64 " p99_upper_us=%" PRIu64 " "
               "max_us=%.3f overbudget_count=%" PRIu64 " budget_us=100\n",
               run_id, total, transport, split_load_name(), load_timing_names[call], timing->count[call],
               load_timing_quantile_upper_us(timing, call, 50),
               load_timing_quantile_upper_us(timing, call, 99),
               (double) timing->max_ns[call] / 1000.0, timing->overbudget_count[call]);
    }
}

static int split_client_send( struct rocketnet_client_t * client, uint32_t magic,
                              uint32_t global_client, uint32_t combo, uint32_t seq,
                              uint64_t run_id, enum rocketnet_reliability_t reliability, int channel )
{
    uint8_t * out = rocketnet_client_start_message(client, channel, reliability, LOAD_PAYLOAD_BYTES);
    if ( !out ) return 0;
    split_build_message(out, magic, global_client, combo, seq, run_id);
    return rocketnet_client_finish_message(client, LOAD_PAYLOAD_BYTES) != 0;
}

static void split_proof_init( struct split_proof_t * proof, uint32_t client, uint32_t send_direction )
{
    memset(proof, 0, sizeof(*proof));
    for ( uint32_t direction = 0; direction < SPLIT_VERIFY_NUM_DIRECTIONS; ++direction )
        for ( uint32_t mode = 0; mode < SPLIT_VERIFY_NUM_MODES; ++mode )
            for ( uint32_t size_class = 0; size_class < SPLIT_VERIFY_NUM_SIZE_CLASSES; ++size_class )
                split_verify_stream_init(&proof->receive[split_verify_stream_index(direction, mode, size_class)],
                                         client, direction, mode, size_class);
    split_verify_ledger_init(&proof->ledger, client, send_direction);
}

/* Sends the next message of one stream through the server (to slot) or the
 * client. A receipt mode's serial goes into the ledger. */
static int split_proof_send( struct split_proof_t * proof, struct rocketnet_server_t * server, int slot,
                             struct rocketnet_client_t * api, uint32_t client, uint32_t direction,
                             uint32_t mode, uint32_t size_class, uint64_t run_id, struct load_timing_t * timing )
{
    if ( !timing ) timing = &split_untimed;
    const uint32_t index = split_verify_stream_index(direction, mode, size_class);
    struct split_verify_payload_t payload;
    payload.run_id = run_id;
    payload.client = client;
    payload.direction = direction;
    payload.mode = mode;
    payload.size_class = size_class;
    payload.sequence = proof->sent[index] + 1;
    payload.length = size_class == SPLIT_VERIFY_SMALL ? SPLIT_PROOF_SMALL_BYTES : SPLIT_PROOF_FRAGMENTED_BYTES;
    if ( !split_verify_payload_fields_valid(&payload) ) return 0;
    const int channel = SPLIT_PROOF_FIRST_CHANNEL + (int) (mode * SPLIT_VERIFY_NUM_SIZE_CLASSES + size_class);
    uint8_t * out = NULL;
    if ( server )
        LOAD_TIMED_VALUE(timing, LOAD_TIMING_SERVER_START_MESSAGE, out,
                         rocketnet_server_start_message(server, (uint64_t) slot, channel,
                                                        (enum rocketnet_reliability_t) mode, (int) payload.length));
    else
        out = rocketnet_client_start_message(api, channel, (enum rocketnet_reliability_t) mode, (int) payload.length);
    if ( !out ) return 0;
    split_verify_write_payload(out, payload.length, &payload);
    uint32_t serial = 0;
    if ( server )
        LOAD_TIMED_VALUE(timing, LOAD_TIMING_SERVER_FINISH_MESSAGE, serial,
                         rocketnet_server_finish_message(server, (int) payload.length));
    else
        serial = rocketnet_client_finish_message(api, (int) payload.length);
    if ( serial == 0 ) return 0;
    proof->sent[index] = payload.sequence;
    if ( split_verify_mode_has_receipt(mode) )
        (void) split_verify_ledger_send(&proof->ledger, serial, mode, size_class, payload.sequence);
    return 1;
}

/* One proof offer: the small message and the fragmented message. */
static int split_proof_offer( struct split_proof_t * proof, struct rocketnet_server_t * server, int slot,
                              struct rocketnet_client_t * api, uint32_t client, uint32_t direction,
                              uint64_t run_id, struct load_timing_t * timing )
{
    const uint32_t k = proof->offers++;
    const uint32_t small_mode = split_verify_small_mode(client, k);
    const uint32_t fragmented_mode = split_verify_fragmented_mode(client, k);
    const int small = split_proof_send(proof, server, slot, api, client, direction, small_mode,
                                       SPLIT_VERIFY_SMALL, run_id, timing);
    const int fragmented = split_proof_send(proof, server, slot, api, client, direction, fragmented_mode,
                                            SPLIT_VERIFY_FRAGMENTED, run_id, timing);
    return small && fragmented;
}

/* Reads, verifies and records one received proof payload from client
 * (server end) or for client (client end). */
static void split_proof_receive( struct split_result_t * result, struct split_proof_t * proof, uint32_t client,
                                 uint32_t direction, uint64_t run_id, const uint8_t * data, int bytes )
{
    struct split_verify_payload_t payload;
    const enum split_verify_read_t read = bytes < 0 ? SPLIT_VERIFY_READ_SHORT :
        split_verify_read_payload(data, (uint32_t) bytes, run_id, &payload);
    if ( read == SPLIT_VERIFY_READ_CHECKSUM ) { result->proof_checksum_errors++; return; }
    if ( read != SPLIT_VERIFY_READ_OK ) { result->payload_errors++; return; }
    if ( payload.client != client || payload.direction != direction ) { result->identity_errors++; return; }
    if ( result->barrier_active ||
         (result->delivery_deadline > 0.0 && rocketnet_time() >= result->delivery_deadline) )
    {
        result->late_application_deliveries++;
        return;
    }
    struct split_verify_stream_t * stream =
        &proof->receive[split_verify_stream_index(direction, payload.mode, payload.size_class)];
    const uint32_t duplicates = stream->duplicates;
    const uint32_t order = stream->order_errors + stream->stale_deliveries;
    const uint32_t identity = stream->identity_errors;
    (void) split_verify_stream_deliver(stream, &payload);
    result->duplicate_errors += (int) (stream->duplicates - duplicates);
    result->order_errors += (int) (stream->order_errors + stream->stale_deliveries - order);
    result->identity_errors += (int) (stream->identity_errors - identity);
    if ( direction == SPLIT_VERIFY_CLIENT_TO_SERVER ) result->c2s_received++;
    else result->s2c_received++;
}

/* Records the receipts the last update published. They are read once per
 * update: a second read copies the same entries. */
static void split_proof_take_receipts( struct split_result_t * result, struct split_proof_t * proof,
                                       const struct rocketnet_receipt_t * receipts, int available )
{
    if ( available > SPLIT_PROOF_RECEIPT_BATCH )
    {
        result->proof_receipt_errors += (uint64_t) (available - SPLIT_PROOF_RECEIPT_BATCH);
        available = SPLIT_PROOF_RECEIPT_BATCH;
    }
    for ( int i = 0; i < available; ++i )
        (void) split_verify_ledger_outcome(&proof->ledger, receipts[i].serial, (int) receipts[i].status);
}

static void split_proof_client_receipts( struct split_result_t * result, struct split_client_t * client )
{
    static struct rocketnet_receipt_t receipts[SPLIT_PROOF_RECEIPT_BATCH];
    if ( !client->proof ) return;
    const int available = rocketnet_client_receipts(client->api, receipts, SPLIT_PROOF_RECEIPT_BATCH);
    split_proof_take_receipts(result, client->proof, receipts, available);
}

/* A seen-bitmap as hex, through its last nonzero byte (at least one byte). */
static const char * split_proof_hex( const uint8_t * seen )
{
    static char text[2 * (SPLIT_VERIFY_WINDOW_SEQUENCES / 8) + 1];
    static const char digits[] = "0123456789abcdef";
    uint32_t bytes = SPLIT_VERIFY_WINDOW_SEQUENCES / 8;
    while ( bytes > 1 && seen[bytes - 1] == 0 ) bytes--;
    for ( uint32_t i = 0; i < bytes; ++i )
    {
        text[2 * i] = digits[seen[i] >> 4];
        text[2 * i + 1] = digits[seen[i] & 15];
    }
    text[2 * bytes] = 0;
    return text;
}

/* One split_verify: line per client, direction, mode and size class the
 * proof sends: the sending end's count (and receipt outcomes), the receiving
 * end's deliveries. The merger joins the two ends of every stream. Adds the
 * ledger's local failures to the result. */
static void split_proof_print( struct split_result_t * result, const char * role, uint64_t run_id,
                               uint32_t client, const struct split_proof_t * proof, uint32_t send_direction )
{
    static struct split_verify_ledger_stream_t share;
    const uint32_t ledger_errors = split_verify_ledger_errors(&proof->ledger);
    result->proof_receipt_errors += ledger_errors;
    for ( uint32_t direction = 0; direction < SPLIT_VERIFY_NUM_DIRECTIONS; ++direction )
        for ( uint32_t mode = 0; mode < SPLIT_VERIFY_NUM_MODES; ++mode )
            for ( uint32_t size_class = 0; size_class < SPLIT_VERIFY_NUM_SIZE_CLASSES; ++size_class )
            {
                if ( size_class == SPLIT_VERIFY_FRAGMENTED && !split_verify_mode_is_reliable(mode) ) continue;
                const uint32_t index = split_verify_stream_index(direction, mode, size_class);
                printf("split_verify: role=%s run_id=%016" PRIx64 " client=%u direction=%s mode=%u size=%s ",
                       role, run_id, client, direction == SPLIT_VERIFY_CLIENT_TO_SERVER ? "c2s" : "s2c", mode,
                       size_class == SPLIT_VERIFY_SMALL ? "small" : "fragmented");
                if ( direction == send_direction )
                {
                    printf("side=send sent=%u", proof->sent[index]);
                    if ( split_verify_mode_has_receipt(mode) )
                    {
                        split_verify_ledger_stream(&proof->ledger, mode, size_class, &share);
                        result->proof_receipt_errors += share.without_outcome;
                        printf(" receipt_sends=%u acked=%u lost=%u without_outcome=%u ledger_errors=%u acked_map=%s",
                               share.sends, share.acked, share.lost, share.without_outcome, ledger_errors,
                               split_proof_hex(share.acked_seen));
                    }
                }
                else
                {
                    const struct split_verify_stream_t * stream = &proof->receive[index];
                    printf("side=receive delivered=%u newest=%u duplicates=%u order_errors=%u stale=%u identity_errors=%u",
                           stream->delivered, stream->newest_sequence, stream->duplicates, stream->order_errors,
                           stream->stale_deliveries, stream->identity_errors);
                    if ( split_verify_mode_has_receipt(mode) )
                        printf(" delivered_map=%s", split_proof_hex(stream->seen));
                }
                printf("\n");
            }
}

/* The pin: ROCKETNET_SERVER_TRANSPORT naming one transport. NULL when the
 * variable is unset, empty, "auto" or a list. */
static const char * split_transport_pin( void )
{
    static char pin[64];
    if ( !rocketnet_test_getenv("ROCKETNET_SERVER_TRANSPORT", pin, sizeof(pin)) ||
         !pin[0] || strchr(pin, ',') || strcmp(pin, "auto") == 0 ) return NULL;
    return pin;
}

/* The pin as a printable field: the server's pinned transport, or "none"
 * when the variable names no single transport or the role is a shard (the
 * pin is the server's placement, not the client's). */
static const char * split_transport_pin_field( const char * role )
{
    if ( strcmp( role, "server" ) != 0 ) return "none";
    const char * pin = split_transport_pin();
    return pin ? pin : "none";
}

static void split_proof_role_line( const char * role, uint64_t run_id, const char * transport,
                                   const struct split_result_t * result, int disconnects, int pass )
{
    printf("split_proof: role=%s run_id=%016" PRIx64 " transport=%s transport_pin=%s small_bytes=%u "
           "fragmented_bytes=%u target_kbps=%.0f min_client_up_kbps=%.6f min_client_down_kbps=%.6f "
           "checksum_errors=%" PRIu64 " receipt_errors=%" PRIu64 " receipts_dropped=%" PRIu64 " "
           "disconnects=%d verdict=%s\n", role, run_id, transport ? transport : "none",
           split_transport_pin_field(role), SPLIT_PROOF_SMALL_BYTES,
           SPLIT_PROOF_FRAGMENTED_BYTES, SPLIT_PROOF_TARGET_KBPS, result->min_client_up_kbps,
           result->min_client_down_kbps, result->proof_checksum_errors, result->proof_receipt_errors,
           result->proof_receipts_dropped, disconnects, pass ? "PASS" : "FAIL");
}

static void split_receive_c2s( struct split_result_t * result, struct split_server_client_t * clients,
                               int total, uint64_t run_id, const struct rocketnet_message_batch_t * batch,
                               const struct rocketnet_server_t * server )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        struct split_message_t message;
        const uint32_t magic = batch->data[j] && batch->bytes[j] >= 4 ? read_u32_le(batch->data[j]) : 0;
        if ( magic == SPLIT_DONE_MAGIC || magic == SPLIT_DRAINED_MAGIC || magic == SPLIT_FINISH_ACK_MAGIC )
        {
            if ( !split_verify_message(batch->data[j], batch->bytes[j], magic, run_id, &message) ||
                 message.client >= (uint32_t) total || message.combo != 0 || message.seq != 1 ||
                 batch->channel[j] != SPLIT_CONTROL_CHANNEL ||
                 batch->client_index[j] < 0 || batch->client_index[j] >= total ||
                 clients[message.client].slot != batch->client_index[j] ||
                 rocketnet_server_client_id(server, batch->client_index[j]) != (uint64_t) message.client + 1 )
            { result->protocol_errors++; continue; }
            struct split_server_client_t * client = &clients[message.client];
            if ( magic == SPLIT_DONE_MAGIC && !client->done ) client->done = 1;
            else if ( magic == SPLIT_DRAINED_MAGIC && client->done && !client->drained ) client->drained = 1;
            else if ( magic == SPLIT_FINISH_ACK_MAGIC && client->finish_sent && !client->finish_acked ) client->finish_acked = 1;
            else result->protocol_errors++;
            continue;
        }
        if ( split_proof_load && magic == SPLIT_VERIFY_MAGIC )
        {
            /* The sender is the client in this slot; the payload must name it. */
            const int slot = batch->client_index[j];
            const uint64_t id = slot >= 0 && slot < total ? rocketnet_server_client_id(server, slot) : 0;
            if ( id < 1 || id > (uint64_t) total || clients[id - 1].slot != slot || !clients[id - 1].proof )
            { result->identity_errors++; continue; }
            split_proof_receive(result, clients[id - 1].proof, (uint32_t) (id - 1), SPLIT_VERIFY_CLIENT_TO_SERVER,
                                run_id, batch->data[j], batch->bytes[j]);
            continue;
        }
        if ( !split_verify_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_CLIENT_TO_SV, run_id, &message) )
        {
            result->payload_errors++;
            continue;
        }
        if ( message.client >= (uint32_t) total || message.combo >= LOAD_NUM_COMBOS ||
             message.seq > split_expected_combo_offers(message.client, message.combo, result->duration_target * split_rate_hz()) ||
             batch->client_index[j] < 0 || batch->client_index[j] >= total ||
             clients[message.client].slot != batch->client_index[j] ||
             rocketnet_server_client_id(server, batch->client_index[j]) != (uint64_t) message.client + 1 )
        {
            result->identity_errors++;
            continue;
        }
        /* The SDK reports its canonical receive channel for these four
         * modes, even though their send API channel argument is the combo. */
        const int channel = (message.combo == ROCKETNET_RELIABILITY_UNRELIABLE ||
                             message.combo == ROCKETNET_RELIABILITY_RELIABLE ||
                             message.combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ||
                             message.combo == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT) ? 0 : (int) message.combo;
        if ( batch->channel[j] != channel )
        {
            result->payload_errors++;
            continue;
        }
        if ( result->barrier_active ||
             (result->delivery_deadline > 0.0 && rocketnet_time() >= result->delivery_deadline) )
        {
            result->late_application_deliveries++;
            continue;
        }
        struct load_stream_state_t * stream = &clients[message.client].c2s[message.combo];
        const int duplicate = stream_seen_test_and_set(stream, message.seq);
        if ( duplicate != 0 ) result->duplicate_errors++;
        if ( combo_is_ordered(message.combo) && message.seq != stream->recv_count + 1 )
            result->order_errors++;
        if ( (message.combo == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
              message.combo == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED) &&
             message.seq <= stream->max_seq_seen )
            result->order_errors++;
        if ( message.seq > stream->max_seq_seen ) stream->max_seq_seen = message.seq;
        stream->recv_count++;
        result->c2s_received++;
    }
}

/* One message of a received batch as a batch of one. The batch struct holds
 * ROCKETNET_RECEIVE_BATCH_MESSAGES entries (about 1.5 MB), so it is never
 * copied onto the stack: a Windows main thread has a 1 MB stack, and the
 * copy overflowed it on the first server message (exit 0xC00000FD). The
 * shard is single-threaded, so one static batch serves every call. */
static const struct rocketnet_message_batch_t * split_batch_entry( const struct rocketnet_message_batch_t * batch, int j )
{
    static struct rocketnet_message_batch_t one;
    one.num_messages = 1;
    one.client_index[0] = batch->client_index[j];
    one.channel[0] = batch->channel[j];
    one.reliability[0] = batch->reliability[j];
    one.bytes[0] = batch->bytes[j];
    one.data[0] = batch->data[j];
    return &one;
}

static void split_receive_s2c( struct split_result_t * result, struct split_client_t * client,
                               uint32_t global_client, uint64_t run_id,
                               const struct rocketnet_message_batch_t * batch )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        struct split_message_t message;
        if ( !split_verify_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_SV_TO_CLIENT, run_id, &message) ||
             message.client != global_client || message.combo != ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
             message.seq > (uint32_t) (result->duration_target * split_rate_hz()) ||
             batch->channel[j] != ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
             batch->client_index[j] != 0 )
        {
            result->payload_errors++;
            continue;
        }
        if ( result->barrier_active ||
             (result->delivery_deadline > 0.0 && rocketnet_time() >= result->delivery_deadline) )
        {
            result->late_application_deliveries++;
            continue;
        }
        if ( stream_seen_test_and_set(&client->s2c, message.seq) != 0 ) result->duplicate_errors++;
        if ( message.seq != client->s2c.recv_count + 1 ) result->order_errors++;
        if ( message.seq > client->s2c.max_seq_seen ) client->s2c.max_seq_seen = message.seq;
        client->s2c.recv_count++;
        result->s2c_received++;
    }
}

/* Consume every delivery from the update that produced it. The SDK expires
 * unread batches on the next update, including during connection setup. */
/* tla/SplitStart.tla ShardArm, ShardGo and ShardRun: a shard client takes
 * ARM, GO and RUN off its receive queue into its startup step, and RUN
 * arriving while it waits for RUN starts it running (ShardRun). */
static void split_client_startup_batch( struct split_result_t * result, struct split_client_t * client,
                                        uint32_t global, uint64_t run_id,
                                        const struct rocketnet_message_batch_t * batch )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        struct split_message_t message;
        const uint32_t magic = batch->data[j] && batch->bytes[j] >= 4 ? read_u32_le(batch->data[j]) : 0;
        if ( magic == LOAD_MAGIC_SV_TO_CLIENT && client->startup.running )
        {
            split_receive_s2c(result, client, global, run_id, split_batch_entry(batch, j));
            continue;
        }
        /* Proof traffic rides its own channels, so it can overtake RUN. */
        if ( split_proof_load && magic == SPLIT_VERIFY_MAGIC && client->proof )
        {
            split_proof_receive(result, client->proof, global, SPLIT_VERIFY_SERVER_TO_CLIENT, run_id,
                                batch->data[j], batch->bytes[j]);
            continue;
        }
        if ( !split_verify_message(batch->data[j], batch->bytes[j], magic, run_id, &message) ||
             message.client != global || message.combo != 0 || message.seq != 1 ||
             batch->client_index[j] != 0 ||
             batch->channel[j] != (magic == SPLIT_RUN_MAGIC ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED : SPLIT_CONTROL_CHANNEL) )
        { result->protocol_errors++; continue; }
        const int kind = magic == SPLIT_ARM_MAGIC ? SPLIT_STARTUP_ARM :
                         magic == SPLIT_GO_MAGIC ? SPLIT_STARTUP_GO :
                         magic == SPLIT_RUN_MAGIC ? SPLIT_STARTUP_RUN : 0;
        if ( !split_startup_receive(&client->startup, kind) ) result->protocol_errors++;
    }
}

/* ARM and GO are answered once the client is connected: its transport hands
 * payloads from before it reports CONNECTED, and a send before then is
 * dropped (tools/load_split_startup.h). tla/SplitStart.tla ShardArm and
 * ShardGo: the ACK answering ARM and the GO_ACK answering GO. */
static void split_client_startup_answer( struct split_result_t * result, struct split_client_t * client,
                                         uint32_t global, uint64_t run_id )
{
    const int connected = rocketnet_client_state(client->api) == ROCKETNET_CLIENT_STATE_CONNECTED;
    const int send = split_startup_due(&client->startup, connected);
    if ( send == SPLIT_STARTUP_SEND_NONE ) return;
    if ( !split_client_send(client->api, send == SPLIT_STARTUP_SEND_ACK ? SPLIT_ACK_MAGIC : SPLIT_GO_ACK_MAGIC,
                            global, 0, 1, run_id, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        result->offer_refusals++;
    split_startup_sent(&client->startup, send);
}

static void split_client_runtime_batch( struct split_result_t * result, struct split_client_t * client,
                                        uint32_t global, uint64_t run_id,
                                        const struct rocketnet_message_batch_t * batch )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        const uint32_t magic = batch->data[j] && batch->bytes[j] >= 4 ? read_u32_le(batch->data[j]) : 0;
        if ( magic == SPLIT_FINISH_MAGIC || magic == SPLIT_CONFIRM_MAGIC )
        {
            struct split_message_t message;
            if ( !split_verify_message(batch->data[j], batch->bytes[j], magic, run_id, &message) ||
                 message.client != global || message.combo != 0 || message.seq != 1 ||
                 batch->channel[j] != SPLIT_CONTROL_CHANNEL || batch->client_index[j] != 0 )
            { result->protocol_errors++; continue; }
            if ( magic == SPLIT_FINISH_MAGIC && !client->finish ) client->finish = 1;
            else if ( magic == SPLIT_CONFIRM_MAGIC && client->finish && !client->confirmed ) client->confirmed = 1;
            else result->protocol_errors++;
            continue;
        }
        if ( split_proof_load && magic == SPLIT_VERIFY_MAGIC && client->proof )
        {
            split_proof_receive(result, client->proof, global, SPLIT_VERIFY_SERVER_TO_CLIENT, run_id,
                                batch->data[j], batch->bytes[j]);
            continue;
        }
        split_receive_s2c(result, client, global, run_id, split_batch_entry(batch, j));
    }
}

static void split_print_streams( const char * role, uint64_t run_id, int first, int count,
                                 const struct split_server_client_t * server_clients,
                                 const struct split_client_t * shard_clients )
{
    if ( split_proof_load ) return;
    for ( int local = 0; local < count; ++local )
    {
        const int client = first + local;
        const struct load_stream_state_t * c2s = server_clients ? server_clients[client].c2s : shard_clients[local].c2s;
        const struct load_stream_state_t * s2c = server_clients ? &server_clients[client].s2c : &shard_clients[local].s2c;
        for ( int combo = 0; combo < LOAD_NUM_COMBOS; ++combo )
            printf("split_stream: role=%s run_id=%016" PRIx64 " client=%d direction=c2s combo=%d sent=%u received=%u\n",
                   role, run_id, client, combo, c2s[combo].sent_count, c2s[combo].recv_count);
        printf("split_stream: role=%s run_id=%016" PRIx64 " client=%d direction=s2c combo=%d sent=%u received=%u\n",
               role, run_id, client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
               s2c->sent_count, s2c->recv_count);
    }
}

static void split_print_wire( const char * role, uint64_t run_id, int first, int count,
                              const struct split_server_client_t * server_clients,
                              const struct split_client_t * shard_clients )
{
    for ( int local = 0; local < count; ++local )
    {
        const struct load_wire_sample_t * start = server_clients ?
            &server_clients[first + local].wire_start : &shard_clients[local].wire_start;
        const struct load_wire_sample_t * end = server_clients ?
            &server_clients[first + local].wire_end : &shard_clients[local].wire_end;
        printf("split_wire: role=%s run_id=%016" PRIx64 " client=%d sent_bytes=%" PRIu64
               " received_bytes=%" PRIu64 "\n", role, run_id, first + local,
               end->sent >= start->sent ? end->sent - start->sent : 0,
               end->received >= start->received ? end->received - start->received : 0);
    }
}

static int split_print_receipt( const char * role, uint64_t run_id, int total, int first, int count,
                                 int duration, double aggregate_limit_kbps,
                                 const struct split_result_t * result,
                                 long fds_pre_init, long fds_post_init, long fds_pre_term, long fds_post_term,
                                 long threads_pre_init, long threads_post_init,
                                 long threads_pre_term, long threads_post_term,
                                 enum rocketnet_status_t term_status, const char * transport )
{
    struct load_census_survivors_t operational, whole;
    int census = 0;
    const int resources = load_resource_verdict(role, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
                                                 threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
                                                 &operational, &whole, &census) &&
        term_status == ROCKETNET_STATUS_OK;
    const int population = result->connected == count;
    const int correctness = result->startup_complete && result->traffic_complete && result->drain_complete &&
        result->barrier_complete &&
        result->connect_seconds <= LOAD_CONNECT_DEADLINE_S &&
        population && !result->identity_errors && !result->payload_errors && !result->order_errors &&
        !result->duplicate_errors && !result->unexpected_disconnects && !result->offer_refusals &&
        !result->protocol_errors && !result->late_application_deliveries && result->wire_valid &&
        (!split_proof_load || (!result->proof_checksum_errors && !result->proof_receipt_errors &&
                               !result->proof_receipts_dropped));
    /* The proof's floor replaces the default 100 kbps per-client cap: every
     * client at least SPLIT_PROOF_TARGET_KBPS on the wire each way. */
    const int per_client = split_proof_load ?
        result->wire_sampled == count && result->min_client_up_kbps >= SPLIT_PROOF_TARGET_KBPS &&
            result->min_client_down_kbps >= SPLIT_PROOF_TARGET_KBPS :
        result->max_client_up_kbps < 100.0 && result->max_client_down_kbps < 100.0;
    const int bandwidth = per_client &&
        result->aggregate_up_kbps < aggregate_limit_kbps && result->aggregate_down_kbps < aggregate_limit_kbps;
    const int timing = strcmp(role, "server") != 0 ||
        (result->timing_valid && result->timing_budget_met);
    const int pass = correctness && result->load_complete && bandwidth && resources && timing;
    printf("split_receipt: schema=split_v1 role=%s run_id=%016" PRIx64 " total=%d first=%d count=%d "
           "duration_target_s=%d connect_elapsed_s=%.6f start_elapsed_s=%.6f "
           "traffic_actual_s=%.6f drain_actual_s=%.6f barrier_actual_s=%.6f startup_utc_s=%" PRIu64 " "
           "traffic_start_utc_s=%" PRIu64 " traffic_end_utc_s=%" PRIu64 " "
           "connected=%d startup_complete=%d traffic_complete=%d drain_complete=%d barrier_complete=%d "
           "expected_offers=%" PRIu64 " missed_due_slots=%" PRIu64 " load_complete=%d "
           "late_application_deliveries=%" PRIu64 " "
           "c2s_offered=%" PRIu64 " c2s_accepted=%" PRIu64 " c2s_received=%" PRIu64 " "
           "s2c_offered=%" PRIu64 " s2c_accepted=%" PRIu64 " s2c_received=%" PRIu64 " "
           "identity_errors=%d payload_errors=%d order_errors=%d duplicate_errors=%d "
           "unexpected_disconnects=%d offer_refusals=%d protocol_errors=%d "
           "wire_valid=%d wire_up=%" PRIu64 " wire_down=%" PRIu64 " "
           "max_client_up_kbps=%.6f max_client_down_kbps=%.6f aggregate_up_kbps=%.6f "
           "aggregate_down_kbps=%.6f aggregate_limit_kbps=%.6f wire_scope=pre_run_to_traffic_end_conservative "
           "wire_rate_denominator_s=%d "
           "workload_scope=traffic_plus_drain transport=%s profile=default payload_bytes=%s "
           "rate_target_hz=%d load=%s fds_pre_init=%ld fds_post_init=%ld fds_pre_term=%ld fds_post_term=%ld "
           "threads_pre_init=%ld threads_post_init=%ld threads_pre_term=%ld threads_post_term=%ld "
           "resource_baseline_schema=%s warm_cycle=1 survivor_threads_operational=%ld "
           "survivor_handles_operational=%ld survivor_threads_total=%ld survivor_handles_total=%ld "
           "os_pool_threads=%ld "
           "term_status_code=%d resource_baseline=%s delivery_verdict=%s load_verdict=%s "
           "bandwidth_verdict=%s timing_verdict=%s timing_budget_us=100 "
           "timing_budget_basis=server_update_send_p99_upper verdict=%s%s%s\n",
           role, run_id, total, first, count, duration, result->connect_seconds, result->start_seconds,
           result->traffic_seconds, result->drain_seconds, result->barrier_seconds, result->startup_utc_s,
           result->traffic_start_utc_s, result->traffic_end_utc_s, result->connected,
           result->startup_complete, result->traffic_complete, result->drain_complete, result->barrier_complete,
           result->expected_offers, result->missed_due_slots, result->load_complete,
           result->late_application_deliveries,
           result->c2s_offered, result->c2s_accepted, result->c2s_received,
           result->s2c_offered, result->s2c_accepted, result->s2c_received,
           result->identity_errors, result->payload_errors, result->order_errors, result->duplicate_errors,
           result->unexpected_disconnects, result->offer_refusals, result->protocol_errors,
           result->wire_valid, result->wire_up, result->wire_down,
           result->max_client_up_kbps, result->max_client_down_kbps,
           result->aggregate_up_kbps, result->aggregate_down_kbps, aggregate_limit_kbps, duration,
           transport ? transport : "none", split_payload_label(), split_rate_hz(), split_load_name(), fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
           threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
           census ? "census_v1" : "phase_counts_v1", operational.threads, operational.handles,
           whole.threads, whole.handles, whole.os_pool_threads,
           (int) term_status, resources ? "PASS" : "FAIL", correctness ? "PASS" : "FAIL",
            result->load_complete ? "PASS" : "FAIL", bandwidth ? "PASS" : "FAIL",
            strcmp(role, "server") == 0 ? (timing ? "PASS" : "FAIL") : "NA",
            pass ? "PASS" : "FAIL",
            /* The proof's transport pin: the server's, in the receipt as well
             * as in the readiness line, so the merger can bind the two. The
             * default and full receipts are unchanged. */
            split_proof_load ? " transport_pin=" : "", split_proof_load ? split_transport_pin_field(role) : "");
    if ( split_proof_load )
        split_proof_role_line(role, run_id, transport, result, result->proof_disconnected_clients,
                              correctness && result->load_complete && bandwidth && resources);
    return pass;
}

/* A start that does not complete says where and why: the phase, the verdict,
 * and how many clients it still waits for. Up to SPLIT_START_NAMED_CLIENTS of
 * them follow with their transport counters, so a client whose path went
 * silent is told apart from a shard that was slow to start. */
static void split_print_start_failure( const char * role, uint64_t run_id, int first, int count,
                                       const char * phase, const char * reason, int waiting, int disconnected )
{
    printf("split_start_failure: role=%s run_id=%016" PRIx64 " first=%d count=%d phase=%s reason=%s "
           "waiting=%d disconnected=%d named_max=%d\n", role, run_id, first, count, phase, reason,
           waiting, disconnected, SPLIT_START_NAMED_CLIENTS);
}

static int split_server_client_live( const struct rocketnet_server_t * server, const struct split_server_client_t * clients, int i )
{
    return clients[i].slot >= 0 && rocketnet_server_client_connected(server, clients[i].slot) &&
        rocketnet_server_client_id(server, clients[i].slot) == (uint64_t) i + 1;
}

/* Proof: a disconnect between the population window and the planned close
 * fails the run (unexpected_disconnects); each client's first one is named
 * once with its reason. */
static void split_server_note_disconnect( struct split_result_t * result, const struct rocketnet_server_t * server,
                                          struct split_server_client_t * clients, int i, uint64_t run_id, const char * phase )
{
    if ( !split_proof_load || clients[i].disconnect_reported ) return;
    clients[i].disconnect_reported = 1;
    result->proof_disconnected_clients++;
    const int slot = clients[i].slot;
    const char * reason = slot < 0 ? "no_slot" :
        rocketnet_server_client_connected(server, slot) ? "slot_reused" :
        rocketnet_disconnect_reason_name(rocketnet_server_disconnect_reason(server, slot));
    printf("split_disconnect: role=server run_id=%016" PRIx64 " client=%d phase=%s reason=%s\n",
           run_id, i, phase, reason ? reason : "unknown");
}

static void split_client_note_disconnect( struct split_result_t * result, struct split_client_t * client, int global,
                                          uint64_t run_id, const char * phase )
{
    if ( !split_proof_load || client->disconnect_reported ) return;
    client->disconnect_reported = 1;
    result->proof_disconnected_clients++;
    const char * reason = rocketnet_client_state_name(rocketnet_client_state(client->api));
    printf("split_disconnect: role=client-shard run_id=%016" PRIx64 " client=%d phase=%s reason=%s\n",
           run_id, global, phase, reason ? reason : "unknown");
}

static void split_proof_server_receipts( struct rocketnet_server_t * server, struct split_server_client_t * clients,
                                         int total, struct split_result_t * result )
{
    static struct rocketnet_receipt_t receipts[SPLIT_PROOF_RECEIPT_BATCH];
    if ( !split_proof_load ) return;
    for ( int i = 0; i < total; ++i )
    {
        if ( !clients[i].proof || !split_server_client_live(server, clients, i) ) continue;
        const int available = rocketnet_server_receipts(server, clients[i].slot, receipts, SPLIT_PROOF_RECEIPT_BATCH);
        split_proof_take_receipts(result, clients[i].proof, receipts, available);
    }
}

/* The delivery capture's SDK bindings (tools/load_split_capture.h). The
 * server keeps one counter block for every connection and no per-connection
 * resend count; each client keeps its own block and its own resends. */
struct split_server_capture_context_t
{
    struct rocketnet_server_t * server;
    const struct split_server_client_t * clients;
};

static int split_server_capture_sample( void * context, int index, double now, struct split_capture_sample_t * out )
{
    const struct split_server_capture_context_t * bound = (const struct split_server_capture_context_t *) context;
    out->time = now;
    if ( !split_server_client_live(bound->server, bound->clients, index) ) return 0;
    const int slot = bound->clients[index].slot;
    struct rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats(bound->server, slot, &stats);
    out->packets_sent = stats.packets_sent;
    out->packets_received = stats.packets_received;
    rocketnet_server_delivery_capture(bound->server, slot, &out->delivery);
    out->state = out->delivery.connected;
    return 1;
}

static uint64_t split_capture_block_value( const struct rocketnet_counters_t * block, int counter )
{
    return block ? rocketnet_counter_value(block, counter) : 0;
}

static uint64_t split_server_capture_counter( void * context, int index, int counter )
{
    const struct split_server_capture_context_t * bound = (const struct split_server_capture_context_t *) context;
    if ( index >= 0 ) return 0;
    return split_capture_block_value(rocketnet_server_transport_counter_block(bound->server), counter) +
           split_capture_block_value(rocketnet_server_message_counter_block(bound->server), counter);
}

struct split_client_capture_context_t
{
    struct split_client_t * clients;
    int count;
};

static int split_client_capture_sample( void * context, int index, double now, struct split_capture_sample_t * out )
{
    const struct split_client_capture_context_t * bound = (const struct split_client_capture_context_t *) context;
    out->time = now;
    struct rocketnet_client_t * api = bound->clients[index].api;
    if ( !api ) return 0;
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats(api, &stats);
    out->state = (int) rocketnet_client_state(api);
    out->packets_sent = stats.packets_sent;
    out->packets_received = stats.packets_received;
    out->messages_resent = stats.messages_resent;
    out->resends_known = 1;
    rocketnet_client_delivery_capture(api, &out->delivery);
    return 1;
}

static uint64_t split_client_capture_one( struct rocketnet_client_t * api, int counter )
{
    if ( !api ) return 0;
    return split_capture_block_value(rocketnet_client_transport_counter_block(api), counter) +
           split_capture_block_value(rocketnet_client_message_counter_block(api), counter);
}

static uint64_t split_client_capture_counter( void * context, int index, int counter )
{
    const struct split_client_capture_context_t * bound = (const struct split_client_capture_context_t *) context;
    if ( index >= 0 ) return split_client_capture_one(bound->clients[index].api, counter);
    uint64_t sum = 0;
    for ( int i = 0; i < bound->count; ++i ) sum += split_client_capture_one(bound->clients[i].api, counter);
    return sum;
}

static void split_capture_print_line( void * context, const char * line )
{
    (void) context;
    printf("%s\n", line);
}

static void split_print_server_waiting( const struct rocketnet_server_t * server, const struct split_server_client_t * clients,
                                        int total, uint64_t run_id, const char * phase, const char * reason,
                                        int waiting, int disconnected, int (*done)( const struct split_server_client_t * client ) )
{
    split_print_start_failure("server", run_id, 0, total, phase, reason, waiting, disconnected);
    int named = 0;
    for ( int i = 0; i < total && named < SPLIT_START_NAMED_CLIENTS; ++i )
    {
        if ( done(&clients[i]) ) continue;
        named++;
        struct rocketnet_server_client_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        const int live = split_server_client_live(server, clients, i);
        if ( live ) rocketnet_server_client_stats(server, clients[i].slot, &stats);
        printf("split_start_waiting: role=server run_id=%016" PRIx64 " client=%d phase=%s connected=%d "
               "packets_sent=%" PRIu64 " packets_received=%" PRIu64 " rtt_ms=%.3f\n", run_id, i, phase, live,
               stats.packets_sent, stats.packets_received, stats.rtt_ms);
    }
}

static int split_server_connected_done( const struct split_server_client_t * client ) { return client->connected; }
static int split_server_arm_done( const struct split_server_client_t * client ) { return client->arm_acked; }
static int split_server_go_done( const struct split_server_client_t * client ) { return client->go_acked; }

/* Send ARM or GO to every client and wait one server window for every
 * acknowledgement. A client that disconnects before it answers ends the wait
 * at once; either failure names the clients still owed. tla/SplitStart.tla
 * ServerArm and ServerGo: ARM or GO sent to every client; ServerAck and
 * ServerGoAck: each acknowledgement recorded; ServerFail: the window expiring
 * (split_server_start_update) fails the start, and cleanup ends every
 * connection. */
static int split_server_start_phase( struct rocketnet_server_t * server, struct split_server_client_t * clients,
                                     int total, uint64_t run_id, enum split_start_step_t phase,
                                     struct split_result_t * result, struct split_capture_t * capture )
{
    const uint32_t send_magic = phase == SPLIT_START_STEP_ARM ? SPLIT_ARM_MAGIC : SPLIT_GO_MAGIC;
    const uint32_t ack_magic = phase == SPLIT_START_STEP_ARM ? SPLIT_ACK_MAGIC : SPLIT_GO_ACK_MAGIC;
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, send_magic, (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL, NULL) )
        { result->offer_refusals++; return 0; }
    rocketnet_server_send_packets(server);
    const double phase_start = rocketnet_time();
    enum split_start_verdict_t verdict = SPLIT_START_WAITING;
    int acked = 0, disconnected = 0;
    while ( !quit && verdict == SPLIT_START_WAITING )
    {
        rocketnet_server_update(server, rocketnet_time());
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            for ( int j = 0; j < batch->num_messages; ++j )
            {
                struct split_message_t message;
                if ( !split_verify_message(batch->data[j], batch->bytes[j], ack_magic, run_id, &message) ||
                     message.combo != 0 || message.seq != 1 || message.client >= (uint32_t) total ||
                     batch->channel[j] != SPLIT_CONTROL_CHANNEL ||
                     batch->client_index[j] < 0 || batch->client_index[j] >= total ||
                     clients[message.client].slot != batch->client_index[j] ||
                     rocketnet_server_client_id(server, batch->client_index[j]) != (uint64_t) message.client + 1 )
                { result->protocol_errors++; continue; }
                int * flag = phase == SPLIT_START_STEP_ARM ? &clients[message.client].arm_acked : &clients[message.client].go_acked;
                if ( *flag ) { result->protocol_errors++; continue; }
                *flag = 1;
            }
        acked = 0;
        disconnected = 0;
        for ( int i = 0; i < total; ++i )
        {
            const int done = phase == SPLIT_START_STEP_ARM ? clients[i].arm_acked : clients[i].go_acked;
            acked += done;
            if ( !done && !split_server_client_live(server, clients, i) )
            {
                disconnected++;
                split_server_note_disconnect(result, server, clients, i, run_id, split_start_step_name(phase));
            }
        }
        rocketnet_server_send_packets(server);
        split_capture_tick(capture, rocketnet_time());
        verdict = split_server_start_update(rocketnet_time(), phase_start, total, acked, disconnected);
        if ( verdict == SPLIT_START_WAITING ) rocketnet_sleep(0.001);
    }
    if ( verdict != SPLIT_START_COMPLETE && !quit )
    {
        result->unexpected_disconnects += disconnected;
        split_print_server_waiting(server, clients, total, run_id, split_start_step_name(phase),
                                   split_start_verdict_name(verdict), total - acked, disconnected,
                                   phase == SPLIT_START_STEP_ARM ? split_server_arm_done : split_server_go_done);
    }
    return verdict == SPLIT_START_COMPLETE && !result->protocol_errors && !quit;
}

static int split_server_run( int total, int duration, uint64_t run_id, const char * server_endpoint,
                             const char * ready_ipv4, double aggregate_limit_kbps )
{
    struct split_result_t result = { 0 };
    result.duration_target = duration;
    struct split_server_client_t * clients = NULL;
    struct split_proof_t * proofs = NULL;
    int transport_refused = 0;
    struct rocketnet_server_t * server = NULL;
    struct load_timing_t timing = { 0 };
    char transport[32] = "none";
    int api_started = 0, timing_started = 0, pass = 0;
    struct split_capture_t capture;
    struct split_capture_entry_t * capture_entries = NULL;
    struct split_server_capture_context_t capture_context;
    memset(&capture, 0, sizeof(capture));
    memset(&capture_context, 0, sizeof(capture_context));
    load_warm_cycle();
    const long fds_pre_init = count_open_fds(), threads_pre_init = count_threads();
    LOAD_CENSUS(LOAD_PRE_INIT);
    long fds_post_init = -1, threads_post_init = -1, fds_pre_term = -1, threads_pre_term = -1;
    long fds_post_term = -1, threads_post_term = -1;
    enum rocketnet_status_t term_status = (enum rocketnet_status_t) -1;
    if ( !load_timing_init(&timing) ) goto cleanup;
    timing_started = 1;
    for ( int i = 0; i < 10000; ++i )
    {
        const uint64_t start_ns = load_clock_ns();
        load_timing_record(&timing, LOAD_TIMING_CLOCK_PAIR, start_ns);
    }
    if ( rocketnet_init(NULL, NULL, NULL) != ROCKETNET_STATUS_OK ) goto cleanup;
    api_started = 1;
    fds_post_init = count_open_fds();
    threads_post_init = count_threads();
    LOAD_CENSUS(LOAD_POST_INIT);
    struct rocketnet_server_config_t config;
    split_server_config(&config, total);
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create(NULL, server_endpoint, NULL, &config, &create_status);
    if ( !server || rocketnet_server_start(server) != ROCKETNET_STATUS_OK ) goto cleanup;
    snprintf(transport, sizeof(transport), "%s", rocketnet_server_transport_name(server));
    {
        /* Pinned transport: a grant other than the one transport
         * ROCKETNET_SERVER_TRANSPORT names is a refusal (exit 2) before any
         * client connects, never a silent fallback. The proof runs pinned. */
        const char * pin = split_transport_pin();
        if ( (pin && strcmp(pin, transport) != 0) || (split_proof_load && !pin) )
        {
            printf("split_transport_refused: role=server run_id=%016" PRIx64 " pin=%s granted=%s\n",
                   run_id, pin ? pin : "none", transport);
            transport_refused = 1;
            goto cleanup;
        }
    }
    clients = (struct split_server_client_t *) calloc((size_t) total, sizeof(*clients));
    capture_entries = (struct split_capture_entry_t *) calloc((size_t) total, sizeof(*capture_entries));
    if ( !clients || !capture_entries ) goto cleanup;
    for ( int i = 0; i < total; ++i ) clients[i].slot = -1;
    if ( split_proof_load )
    {
        proofs = (struct split_proof_t *) calloc((size_t) total, sizeof(*proofs));
        if ( !proofs ) goto cleanup;
        for ( int i = 0; i < total; ++i )
        {
            split_proof_init(&proofs[i], (uint32_t) i, SPLIT_VERIFY_SERVER_TO_CLIENT);
            clients[i].proof = &proofs[i];
        }
    }
    {
        struct split_capture_source_t source;
        struct split_capture_emit_t emit;
        capture_context.server = server;
        capture_context.clients = clients;
        source.context = &capture_context;
        source.sample = split_server_capture_sample;
        source.counter = split_server_capture_counter;
        source.counters_per_connection = 0;
        emit.context = NULL;
        emit.line = split_capture_print_line;
        split_capture_init(&capture, "server", run_id, 0, total, capture_entries, &source, &emit);
    }

    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char key_hex[2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 1];
    rocketnet_server_public_key(server, key);
    split_encode_key(key, key_hex);
    result.startup_utc_s = (uint64_t) time(NULL);
    printf("split_ready: schema=split_v1 role=server run_id=%016" PRIx64 " total=%d "
           "duration_s=%d profile=default payload_bytes=%s rate_target_hz=%d load=%s "
           "address=%s:%u key=%s transport=%s startup_utc_s=%" PRIu64 "%s%s\n",
           run_id, total, duration, split_payload_label(), split_rate_hz(), split_load_name(), ready_ipv4,
           (unsigned) rocketnet_server_port(server), key_hex, transport, result.startup_utc_s,
           split_proof_load ? " transport_pin=" : "", split_proof_load ? split_transport_pin() : "");
    fflush(stdout);

    /* The launcher has a 60-second readiness lease. Once the first client
     * connects, the established 10-second population deadline applies.
     * tla/SplitStart.tla Connect: each client connecting, from the first
     * connect time; ServerFail: the population window expiring fails the
     * start, and cleanup ends every connection. */
    const double ready_time = rocketnet_time();
    double first_connected = -1.0;
    int all_connected = 0;
    while ( !quit )
    {
        const double now = rocketnet_time();
        rocketnet_server_update(server, now);
        rocketnet_server_send_packets(server);
        split_capture_tick(&capture, now);
        result.connected = 0;
        for ( int slot = 0; slot < total; ++slot )
        {
            if ( !rocketnet_server_client_connected(server, slot) ) continue;
            const uint64_t id = rocketnet_server_client_id(server, slot);
            if ( id < 1 || id > (uint64_t) total ) { result.identity_errors++; continue; }
            struct split_server_client_t * client = &clients[id - 1];
            if ( client->connected && client->slot != slot ) { result.identity_errors++; continue; }
            client->slot = slot;
            client->connected = 1;
            result.connected++;
        }
        if ( first_connected < 0.0 && result.connected ) first_connected = now;
        if ( result.connected == total && !result.identity_errors )
        {
            all_connected = 1;
            result.connect_seconds = first_connected < 0.0 ? 0.0 : now - first_connected;
            break;
        }
        if ( now - ready_time >= 60.0 || (first_connected >= 0.0 && now - first_connected >= LOAD_CONNECT_DEADLINE_S) )
        {
            result.connect_seconds = first_connected < 0.0 ? now - ready_time : now - first_connected;
            for ( int i = 0; i < total; ++i ) clients[i].connected = split_server_client_live(server, clients, i);
            split_print_server_waiting(server, clients, total, run_id, "connect", "timed_out",
                                       total - result.connected, 0, split_server_connected_done);
            goto cleanup;
        }
        rocketnet_sleep(0.001);
    }
    if ( !all_connected || quit ) goto cleanup;

    const double arm_start = rocketnet_time();
    if ( !split_server_start_phase(server, clients, total, run_id, SPLIT_START_STEP_ARM, &result, &capture) ||
         !split_server_start_phase(server, clients, total, run_id, SPLIT_START_STEP_GO, &result, &capture) )
        goto cleanup;
    for ( int i = 0; i < total; ++i )
    {
        struct rocketnet_server_client_stats_t stats;
        rocketnet_server_client_stats(server, clients[i].slot, &stats);
        clients[i].wire_start.sent = stats.bytes_sent;
        clients[i].wire_start.received = stats.bytes_received;
    }
    {
        /* RUN's mark: the first sequence any packet sent to a client after RUN
         * is queued can carry, so it holds whether RUN leaves on this flush or
         * later. An acknowledgement at or past it is of a packet sent after RUN
         * was queued: RUN's own, or a later one. The total reliable_unacked
         * window cannot identify whether RUN itself remains outstanding. */
        const double run_queued = rocketnet_time();
        for ( int i = 0; i < total; ++i )
        {
            struct rocketnet_delivery_capture_t delivery;
            rocketnet_server_delivery_capture(server, clients[i].slot, &delivery);
            split_capture_mark_run(&capture, i, run_queued, delivery.highest_sent_sequence + 1);
        }
    }
    /* RUN and all subsequent S2C traffic share an ordered channel. A client
     * cannot consume traffic before its RUN even if clocks or network lag.
     * tla/SplitStart.tla ServerRun: every GO acknowledged, RUN sent to every
     * client. */
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, SPLIT_RUN_MAGIC,
                                (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, NULL) )
        { result.offer_refusals++; goto cleanup; }
    rocketnet_server_send_packets(server);
    result.startup_complete = 1;
    result.start_seconds = rocketnet_time() - arm_start;
    const double traffic_start = rocketnet_time();
    result.traffic_start_utc_s = (uint64_t) time(NULL);
    for ( int i = 0; i < total; ++i )
        clients[i].next_due = traffic_start + (split_proof_load ? (double) (i % 5) * 0.010 : 0.0);
    result.expected_offers = (uint64_t) total * (uint64_t) duration * (uint64_t) split_rate_hz();
    timing.enabled = 1;
    double next_tick = traffic_start;
    int traffic_sampled = 0;
    const double end_time = traffic_start + (double) duration;
    const double drain_end = end_time + 3.0;
    result.delivery_deadline = drain_end;
    while ( !quit && rocketnet_time() < drain_end )
    {
        const double now = rocketnet_time();
        const int in_traffic = now < end_time;
        if ( !in_traffic && !traffic_sampled )
        {
            traffic_sampled = 1;
            result.traffic_seconds = now - traffic_start;
            result.traffic_end_utc_s = (uint64_t) time(NULL);
            result.wire_valid = result.traffic_seconds > 0.0;
            for ( int i = 0; i < total; ++i )
            {
                if ( !rocketnet_server_client_connected(server, clients[i].slot) ||
                     rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1 )
                { result.wire_valid = 0; continue; }
                struct rocketnet_server_client_stats_t stats;
                rocketnet_server_client_stats(server, clients[i].slot, &stats);
                clients[i].wire_end.sent = stats.bytes_sent;
                clients[i].wire_end.received = stats.bytes_received;
                if ( stats.bytes_sent < clients[i].wire_start.sent ||
                     stats.bytes_received < clients[i].wire_start.received )
                { result.wire_valid = 0; continue; }
                const uint64_t up = stats.bytes_sent - clients[i].wire_start.sent;
                const uint64_t down = stats.bytes_received - clients[i].wire_start.received;
                result.wire_up += up;
                result.wire_down += down;
                const double up_kbps = (double) up * 8.0 / ((double) duration * 1000.0);
                const double down_kbps = (double) down * 8.0 / ((double) duration * 1000.0);
                if ( up_kbps > result.max_client_up_kbps ) result.max_client_up_kbps = up_kbps;
                if ( down_kbps > result.max_client_down_kbps ) result.max_client_down_kbps = down_kbps;
                if ( !result.wire_sampled || up_kbps < result.min_client_up_kbps ) result.min_client_up_kbps = up_kbps;
                if ( !result.wire_sampled || down_kbps < result.min_client_down_kbps ) result.min_client_down_kbps = down_kbps;
                result.wire_sampled++;
            }
            result.aggregate_up_kbps = (double) result.wire_up * 8.0 / ((double) duration * 1000.0);
            result.aggregate_down_kbps = (double) result.wire_down * 8.0 / ((double) duration * 1000.0);
            result.traffic_complete = result.traffic_seconds >= (double) duration;
        }
        LOAD_TIMED_VOID(&timing, LOAD_TIMING_SERVER_UPDATE, rocketnet_server_update(server, now));
        /* An update may itself cross the drain deadline. Do not let its
         * newly published batch repair delivery after the fixed window. */
        if ( rocketnet_time() >= drain_end ) result.barrier_active = 1;
        split_proof_server_receipts(server, clients, total, &result);
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = split_server_receive(server, &timing)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        for ( int i = 0; in_traffic && i < total; ++i )
        {
            if ( !rocketnet_server_client_connected(server, clients[i].slot) ||
                 rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1 )
            {
                result.unexpected_disconnects++;
                split_server_note_disconnect(&result, server, clients, i, run_id, "traffic");
                continue;
            }
            /* Use the offer-time clock for both the deadline and due phase.
             * An update may have crossed one or more due slots since now. */
            const double offer_now = rocketnet_time();
            if ( offer_now >= end_time ) break;
            if ( !split_offer_due(offer_now, end_time, split_offer_period_s(), &clients[i].next_due,
                                  &result.missed_due_slots) ) continue;
            if ( split_proof_load )
            {
                result.s2c_offered++;
                if ( !split_proof_offer(clients[i].proof, server, clients[i].slot, NULL, (uint32_t) i,
                                        SPLIT_VERIFY_SERVER_TO_CLIENT, run_id, &timing) )
                { result.offer_refusals++; continue; }
                result.s2c_accepted++;
                continue;
            }
            if ( clients[i].s2c.sent_count + 1 >= LOAD_MAX_SEQ_TRACKED )
            { result.protocol_errors++; continue; }
            result.s2c_offered++;
            const uint32_t seq = clients[i].s2c.sent_count + 1;
            if ( !split_server_send(server, clients[i].slot, LOAD_MAGIC_SV_TO_CLIENT,
                                    (uint32_t) i, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                    seq, run_id, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                    ROCKETNET_RELIABILITY_RELIABLE_ORDERED, &timing) )
            { result.offer_refusals++; continue; }
            clients[i].s2c.sent_count = seq;
            result.s2c_accepted++;
        }
        LOAD_TIMED_VOID(&timing, LOAD_TIMING_SERVER_SEND_PACKETS, rocketnet_server_send_packets(server));
        split_capture_tick(&capture, rocketnet_time());
        next_tick += split_server_tick_s();
        const double left = next_tick - rocketnet_time();
        if ( left > 0.0 ) rocketnet_sleep(left);
    }
    result.drain_seconds = rocketnet_time() - end_time;
    result.drain_complete = result.drain_seconds >= 3.0;
    result.load_complete = result.s2c_offered == result.expected_offers && !result.missed_due_slots;
    timing.enabled = 0;
    if ( !traffic_sampled ) goto cleanup;
    result.barrier_active = 1;
    /* The shard starts its full local duration only after all local RUNs.
     * Wait for its own drain to complete; this bounded teardown phase is not
     * part of the server's measured three-second drain. */
    const double barrier_start = rocketnet_time();
    int drained = 0;
    while ( !quit && drained < total && rocketnet_time() - barrier_start < SPLIT_BARRIER_DEADLINE_S )
    {
        rocketnet_server_update(server, rocketnet_time());
        split_proof_server_receipts(server, clients, total, &result);
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        drained = 0;
        for ( int i = 0; i < total; ++i )
        {
            drained += clients[i].drained;
            if ( !rocketnet_server_client_connected(server, clients[i].slot) ||
                 rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1 )
            {
                result.unexpected_disconnects++;
                split_server_note_disconnect(&result, server, clients, i, run_id, "drain_barrier");
            }
        }
        rocketnet_server_send_packets(server);
        split_capture_tick(&capture, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    if ( drained != total || result.protocol_errors || result.unexpected_disconnects || quit ) goto cleanup;
    for ( int i = 0; i < total; ++i )
    {
        if ( !split_server_send(server, clients[i].slot, SPLIT_FINISH_MAGIC,
                                (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL, &timing) )
        { result.offer_refusals++; goto cleanup; }
        clients[i].finish_sent = 1;
    }
    rocketnet_server_send_packets(server);
    const double ack_start = rocketnet_time();
    int finish_acked = 0;
    while ( !quit && finish_acked < total && rocketnet_time() - ack_start < SPLIT_BARRIER_DEADLINE_S )
    {
        rocketnet_server_update(server, rocketnet_time());
        split_proof_server_receipts(server, clients, total, &result);
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        finish_acked = 0;
        for ( int i = 0; i < total; ++i )
        {
            finish_acked += clients[i].finish_acked;
            if ( !clients[i].finish_acked &&
                 (!rocketnet_server_client_connected(server, clients[i].slot) ||
                  rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1) )
            {
                result.unexpected_disconnects++;
                split_server_note_disconnect(&result, server, clients, i, run_id, "finish_barrier");
            }
        }
        rocketnet_server_send_packets(server);
        split_capture_tick(&capture, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    if ( finish_acked != total || result.protocol_errors || result.unexpected_disconnects || quit ) goto cleanup;
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, SPLIT_CONFIRM_MAGIC,
                                (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL, &timing) )
        { result.offer_refusals++; goto cleanup; }
    /* Keep retransmission running after confirmation. Shards record their own
     * confirmation; the merger rejects any shard that missed it. */
    const double confirm_start = rocketnet_time();
    while ( !quit && rocketnet_time() - confirm_start < SPLIT_CONFIRM_GRACE_S )
    {
        rocketnet_server_update(server, rocketnet_time());
        split_proof_server_receipts(server, clients, total, &result);
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        rocketnet_server_send_packets(server);
        /* no capture tick: confirmed shards disconnect on purpose here, and
         * those are not kicks */
        rocketnet_sleep(0.001);
    }
    result.barrier_seconds = rocketnet_time() - barrier_start;
    result.barrier_complete = !quit && !result.protocol_errors;

cleanup:
    timing.enabled = 0;
    if ( server )
    {
        if ( clients )
        {
            split_print_streams("server", run_id, 0, total, clients, NULL);
            split_print_wire("server", run_id, 0, total, clients, NULL);
        }
        if ( proofs )
        {
            struct rocketnet_server_stats_t stats;
            rocketnet_server_stats(server, &stats);
            result.proof_receipts_dropped = stats.receipts_dropped;
            for ( int i = 0; i < total; ++i )
                split_proof_print(&result, "server", run_id, (uint32_t) i, &proofs[i], SPLIT_VERIFY_SERVER_TO_CLIENT);
        }
        if ( capture.entries ) split_capture_end(&capture, rocketnet_time());
        rn_cli_server_stop_and_destroy(server);
    }
    free(clients);
    free(proofs);
    free(capture_entries);
    if ( api_started )
    {
        fds_pre_term = count_open_fds();
        threads_pre_term = count_threads();
        LOAD_CENSUS(LOAD_PRE_TERM);
        term_status = rocketnet_term();
        fds_post_term = count_open_fds();
        threads_post_term = count_threads();
        LOAD_CENSUS(LOAD_POST_TERM);
    }
    if ( timing_started )
    {
        result.timing_valid = timing.count[LOAD_TIMING_SERVER_UPDATE] > 0 &&
            timing.count[LOAD_TIMING_SERVER_SEND_PACKETS] > 0 &&
            timing.count[LOAD_TIMING_CLOCK_PAIR] == 10000;
        result.timing_budget_met = result.timing_valid &&
            load_timing_quantile_upper_us(&timing, LOAD_TIMING_SERVER_UPDATE, 99) <= 100 &&
            load_timing_quantile_upper_us(&timing, LOAD_TIMING_SERVER_SEND_PACKETS, 99) <= 100;
        printf("split_server_timing: run_id=%016" PRIx64 " scope=traffic_plus_drain "
               "server_update_count=%" PRIu64 " server_update_p50_upper_us=%" PRIu64 " "
               "server_update_p99_upper_us=%" PRIu64 " server_update_max_us=%.3f "
               "server_send_count=%" PRIu64 " server_send_p99_upper_us=%" PRIu64 " "
               "clock_pair_count=%" PRIu64 "\n", run_id,
               timing.count[LOAD_TIMING_SERVER_UPDATE],
               load_timing_quantile_upper_us(&timing, LOAD_TIMING_SERVER_UPDATE, 50),
               load_timing_quantile_upper_us(&timing, LOAD_TIMING_SERVER_UPDATE, 99),
               (double) timing.max_ns[LOAD_TIMING_SERVER_UPDATE] / 1000.0,
               timing.count[LOAD_TIMING_SERVER_SEND_PACKETS],
               load_timing_quantile_upper_us(&timing, LOAD_TIMING_SERVER_SEND_PACKETS, 99),
               timing.count[LOAD_TIMING_CLOCK_PAIR]);
        split_print_server_calls(&timing, run_id, total, transport);
        free(timing.buckets);
    }
    pass = split_print_receipt("server", run_id, total, 0, total, duration, aggregate_limit_kbps,
                               &result, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
                               threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
                               term_status, transport);
    if ( transport_refused ) return 2;
    return pass ? 0 : 1;
}

/* connecting names the clients not yet connected; otherwise the start phase
 * is the earliest step any local client has not passed. */
static void split_print_shard_waiting( const struct split_client_t * clients, int first, int count, uint64_t run_id,
                                       int connecting, const char * reason, int waiting, int disconnected )
{
    enum split_start_step_t phase = SPLIT_START_STEP_RUNNING;
    for ( int i = 0; i < count; ++i )
    {
        const enum split_start_step_t step = split_start_client_step(&clients[i].startup);
        if ( step < phase ) phase = step;
    }
    split_print_start_failure("client-shard", run_id, first, count,
                              connecting ? "connect" : split_start_step_name(phase), reason, waiting, disconnected);
    int named = 0;
    for ( int i = 0; i < count && named < SPLIT_START_NAMED_CLIENTS; ++i )
    {
        if ( connecting ? clients[i].connected : clients[i].startup.running ) continue;
        named++;
        struct rocketnet_client_stats_t stats;
        rocketnet_client_stats(clients[i].api, &stats);
        printf("split_start_waiting: role=client-shard run_id=%016" PRIx64 " client=%d phase=%s state=%s "
               "packets_sent=%" PRIu64 " packets_received=%" PRIu64 " messages_resent=%" PRIu64 "\n", run_id, first + i,
               split_start_step_name(split_start_client_step(&clients[i].startup)),
               rocketnet_client_state_name(rocketnet_client_state(clients[i].api)),
               stats.packets_sent, stats.packets_received, stats.messages_resent);
    }
}

static int split_client_run( int total, int duration, uint64_t run_id, int first, int count,
                             const char * address, const uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES],
                             double aggregate_limit_kbps )
{
    struct split_result_t result = { 0 };
    result.duration_target = duration;
    struct split_client_t * clients = NULL;
    struct split_proof_t * proofs = NULL;
    char transport[32] = "none";
    int api_started = 0, traffic_sampled = 0, pass = 0;
    struct split_capture_t capture;
    struct split_capture_entry_t * capture_entries = NULL;
    struct split_client_capture_context_t capture_context;
    memset(&capture, 0, sizeof(capture));
    memset(&capture_context, 0, sizeof(capture_context));
    load_warm_cycle();
    const long fds_pre_init = count_open_fds(), threads_pre_init = count_threads();
    LOAD_CENSUS(LOAD_PRE_INIT);
    long fds_post_init = -1, threads_post_init = -1, fds_pre_term = -1, threads_pre_term = -1;
    long fds_post_term = -1, threads_post_term = -1;
    enum rocketnet_status_t term_status = (enum rocketnet_status_t) -1;
#if !defined(_WIN32)
    struct load_fd_limit_t fd_limit;
    if ( !ensure_fd_limit(count, &fd_limit) ) goto cleanup;
#endif
    if ( rocketnet_init(NULL, NULL, NULL) != ROCKETNET_STATUS_OK ) goto cleanup;
    api_started = 1;
    fds_post_init = count_open_fds();
    threads_post_init = count_threads();
    LOAD_CENSUS(LOAD_POST_INIT);
    struct rocketnet_client_config_t config;
    split_client_config(&config);
    clients = (struct split_client_t *) calloc((size_t) count, sizeof(*clients));
    capture_entries = (struct split_capture_entry_t *) calloc((size_t) count, sizeof(*capture_entries));
    if ( !clients || !capture_entries ) goto cleanup;
    if ( split_proof_load )
    {
        proofs = (struct split_proof_t *) calloc((size_t) count, sizeof(*proofs));
        if ( !proofs ) goto cleanup;
        for ( int i = 0; i < count; ++i )
        {
            split_proof_init(&proofs[i], (uint32_t) (first + i), SPLIT_VERIFY_CLIENT_TO_SERVER);
            clients[i].proof = &proofs[i];
        }
    }
    {
        struct split_capture_source_t source;
        struct split_capture_emit_t emit;
        capture_context.clients = clients;
        capture_context.count = count;
        source.context = &capture_context;
        source.sample = split_client_capture_sample;
        source.counter = split_client_capture_counter;
        source.counters_per_connection = 1;
        emit.context = NULL;
        emit.line = split_capture_print_line;
        split_capture_init(&capture, "client-shard", run_id, first, count, capture_entries, &source, &emit);
    }
    const uint64_t token_now = (uint64_t) time(NULL);
    for ( int i = 0; i < count; ++i )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        const uint32_t global = (uint32_t) (first + i);
        if ( rocketnet_create_dev_connect_token(address, NULL, key, 30, 10,
              (uint64_t) global + 1, LOAD_PROTOCOL_ID, token_now, NULL, token) != ROCKETNET_STATUS_OK )
            goto cleanup;
        enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
        clients[i].api = rocketnet_client_create(NULL, &config, &create_status);
        if ( !clients[i].api ) goto cleanup;
        /* Capture before any connect/update can receive ARM, GO, RUN or
         * application traffic. Startup bytes conservatively count toward the
         * requested-duration wire cap. */
        load_client_wire_sample(clients[i].api, &clients[i].wire_start);
        if ( rocketnet_client_connect(clients[i].api, token) != ROCKETNET_STATUS_OK )
            goto cleanup;
        clients[i].next_combo = global % LOAD_NUM_COMBOS;
    }
    snprintf(transport, sizeof(transport), "%s", rocketnet_client_transport_name(clients[0].api));
    result.startup_utc_s = (uint64_t) time(NULL);
    printf("split_shard_ready: schema=split_v1 role=client-shard run_id=%016" PRIx64
           " total=%d first=%d count=%d duration_s=%d profile=default "
           "payload_bytes=%s rate_target_hz=%d load=%s startup_utc_s=%" PRIu64 "\n",
           run_id, total, first, count, duration, split_payload_label(), split_rate_hz(), split_load_name(),
           result.startup_utc_s);
    fflush(stdout);
    const double connect_start = rocketnet_time();
    while ( !quit && rocketnet_time() - connect_start < LOAD_CONNECT_DEADLINE_S )
    {
        result.connected = 0;
        for ( int i = 0; i < count; ++i )
        {
            rocketnet_client_update(clients[i].api, rocketnet_time());
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_startup_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            split_client_startup_answer(&result, &clients[i], (uint32_t) (first + i), run_id);
            rocketnet_client_send_packets(clients[i].api);
            const enum rocketnet_client_state_t state = rocketnet_client_state(clients[i].api);
            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                clients[i].connected = 1;
                result.connected++;
            }
            else if ( clients[i].connected || (int) state < 0 ) result.unexpected_disconnects++;
        }
        split_capture_tick(&capture, rocketnet_time());
        if ( result.connected == count ) break;
        rocketnet_sleep(0.001);
    }
    result.connect_seconds = rocketnet_time() - connect_start;
    if ( result.connected != count && !quit )
    {
        int refused = 0;
        for ( int i = 0; i < count; ++i )
            if ( (int) rocketnet_client_state(clients[i].api) < 0 ) refused++;
        split_print_shard_waiting(clients, first, count, run_id, 1, "timed_out", count - result.connected, refused);
    }
    if ( result.connected != count || result.connect_seconds > LOAD_CONNECT_DEADLINE_S || quit ) goto cleanup;

    /* One start wait for ARM, GO and RUN, bounded from the shard's own
     * connection by every server window (tools/load_split_barrier.h,
     * modelled in tla/SplitStart.tla): the shard that connects first waits
     * for the last, and the server decides a start that fails. RUN is emitted only after every shard has
     * acknowledged GO, so machine clock skew never shortens the traffic
     * interval. split_shard_start_update is ShardEnded (a client the
     * server's failed start disconnected) and ShardTimeout (the shard's own
     * deadline passing). */
    struct split_shard_start_t start;
    split_shard_start_begin(&start, rocketnet_time(), LOAD_CONNECT_DEADLINE_S,
                            (double) config.reliable_message_timeout_ms / 1000.0);
    const double arm_start = rocketnet_time();
    enum split_start_verdict_t start_verdict = SPLIT_START_WAITING;
    int running = 0, start_disconnected = 0;
    while ( !quit && !result.protocol_errors && !result.offer_refusals && start_verdict == SPLIT_START_WAITING )
    {
        running = 0;
        start_disconnected = 0;
        for ( int i = 0; i < count; ++i )
        {
            rocketnet_client_update(clients[i].api, rocketnet_time());
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_startup_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            split_client_startup_answer(&result, &clients[i], (uint32_t) (first + i), run_id);
            running += clients[i].startup.running;
            if ( clients[i].startup.running && capture_entries[i].run_time <= 0.0 )
                split_capture_mark_run(&capture, i, rocketnet_time(), 0);
            if ( rocketnet_client_state(clients[i].api) != ROCKETNET_CLIENT_STATE_CONNECTED ) start_disconnected++;
            rocketnet_client_send_packets(clients[i].api);
        }
        split_capture_tick(&capture, rocketnet_time());
        start_verdict = split_shard_start_update(&start, rocketnet_time(), count, running, start_disconnected);
        if ( start_verdict == SPLIT_START_WAITING ) rocketnet_sleep(0.001);
    }
    if ( start_verdict != SPLIT_START_COMPLETE && start_verdict != SPLIT_START_WAITING && !quit )
    {
        result.unexpected_disconnects += start_disconnected;
        split_print_shard_waiting(clients, first, count, run_id, 0, split_start_verdict_name(start_verdict),
                                  count - running, start_disconnected);
    }
    if ( start_verdict != SPLIT_START_COMPLETE || result.protocol_errors || result.offer_refusals || quit ) goto cleanup;
    result.startup_complete = 1;
    result.start_seconds = rocketnet_time() - arm_start;
    const double traffic_start = rocketnet_time();
    result.traffic_start_utc_s = (uint64_t) time(NULL);
    for ( int i = 0; i < count; ++i ) clients[i].next_due = traffic_start;
    result.expected_offers = (uint64_t) count * (uint64_t) duration * (uint64_t) split_rate_hz();
    const double traffic_end = traffic_start + (double) duration;
    const double drain_end = traffic_end + 3.0;
    result.delivery_deadline = drain_end;
    double next_tick = traffic_start;
    int cursor = 0;
    while ( !quit && rocketnet_time() < drain_end )
    {
        const double now = rocketnet_time();
        const int in_traffic = now < traffic_end;
        if ( !in_traffic && !traffic_sampled )
        {
            traffic_sampled = 1;
            result.traffic_seconds = now - traffic_start;
            result.traffic_end_utc_s = (uint64_t) time(NULL);
            result.wire_valid = result.traffic_seconds > 0.0;
            for ( int i = 0; i < count; ++i )
            {
                load_client_wire_sample(clients[i].api, &clients[i].wire_end);
                if ( clients[i].wire_end.sent < clients[i].wire_start.sent ||
                     clients[i].wire_end.received < clients[i].wire_start.received )
                { result.wire_valid = 0; continue; }
                const uint64_t up = clients[i].wire_end.sent - clients[i].wire_start.sent;
                const uint64_t down = clients[i].wire_end.received - clients[i].wire_start.received;
                result.wire_up += up;
                result.wire_down += down;
                const double up_kbps = (double) up * 8.0 / ((double) duration * 1000.0);
                const double down_kbps = (double) down * 8.0 / ((double) duration * 1000.0);
                if ( up_kbps > result.max_client_up_kbps ) result.max_client_up_kbps = up_kbps;
                if ( down_kbps > result.max_client_down_kbps ) result.max_client_down_kbps = down_kbps;
                if ( !result.wire_sampled || up_kbps < result.min_client_up_kbps ) result.min_client_up_kbps = up_kbps;
                if ( !result.wire_sampled || down_kbps < result.min_client_down_kbps ) result.min_client_down_kbps = down_kbps;
                result.wire_sampled++;
            }
            result.aggregate_up_kbps = (double) result.wire_up * 8.0 / ((double) duration * 1000.0);
            result.aggregate_down_kbps = (double) result.wire_down * 8.0 / ((double) duration * 1000.0);
            result.traffic_complete = result.traffic_seconds >= (double) duration;
            for ( int i = 0; i < count; ++i )
                if ( !split_client_send(clients[i].api, SPLIT_DONE_MAGIC,
                                        (uint32_t) (first + i), 0, 1, run_id,
                                        ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
                    result.offer_refusals++;
        }
        const int slice = in_traffic ? (count < 100 ? count : 100) : count;
        for ( int k = 0; k < slice; ++k )
        {
            if ( !in_traffic && rocketnet_time() >= drain_end ) break;
            const int local = cursor;
            cursor = (cursor + 1) % count;
            struct split_client_t * client = &clients[local];
            rocketnet_client_update(client->api, rocketnet_time());
            if ( rocketnet_time() >= drain_end ) result.barrier_active = 1;
            split_proof_client_receipts(&result, client);
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(client->api)) != NULL )
                split_client_runtime_batch(&result, client, (uint32_t) (first + local), run_id, batch);
            const enum rocketnet_client_state_t state = rocketnet_client_state(client->api);
            if ( state != ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                result.unexpected_disconnects++;
                split_client_note_disconnect(&result, client, first + local, run_id, "traffic");
            }
            /* A client update can cross a due slot. The same fresh clock
             * decides traffic eligibility and counts every skipped slot. */
            const double offer_now = rocketnet_time();
            if ( split_proof_load && in_traffic && state == ROCKETNET_CLIENT_STATE_CONNECTED &&
                 split_offer_due(offer_now, traffic_end, split_offer_period_s(), &client->next_due,
                                 &result.missed_due_slots) )
            {
                result.c2s_offered++;
                if ( !split_proof_offer(client->proof, NULL, 0, client->api, (uint32_t) (first + local),
                                        SPLIT_VERIFY_CLIENT_TO_SERVER, run_id, NULL) )
                    result.offer_refusals++;
                else
                    result.c2s_accepted++;
            }
            else if ( in_traffic && state == ROCKETNET_CLIENT_STATE_CONNECTED && !split_proof_load &&
                 split_offer_due(offer_now, traffic_end, split_offer_period_s(), &client->next_due,
                                 &result.missed_due_slots) )
            {
                const uint32_t combo = client->next_combo % LOAD_NUM_COMBOS;
                client->next_combo++;
                struct load_stream_state_t * stream = &client->c2s[combo];
                if ( stream->sent_count + 1 >= LOAD_MAX_SEQ_TRACKED ) result.protocol_errors++;
                else
                {
                    const uint32_t seq = stream->sent_count + 1;
                    result.c2s_offered++;
                    if ( !split_client_send(client->api, LOAD_MAGIC_CLIENT_TO_SV,
                                            (uint32_t) (first + local), combo, seq, run_id,
                                            (enum rocketnet_reliability_t) combo, (int) combo) )
                        result.offer_refusals++;
                    else
                    {
                        stream->sent_count = seq;
                        result.c2s_accepted++;
                    }
                }
            }
            rocketnet_client_send_packets(client->api);
        }
        split_capture_tick(&capture, rocketnet_time());
        next_tick += 0.010;
        const double left = next_tick - rocketnet_time();
        if ( left > 0.0 ) rocketnet_sleep(left);
    }
    result.drain_seconds = rocketnet_time() - traffic_end;
    result.drain_complete = result.drain_seconds >= 3.0;
    result.load_complete = result.c2s_offered == result.expected_offers && !result.missed_due_slots;
    if ( !traffic_sampled ) goto cleanup;
    result.barrier_active = 1;
    const double barrier_start = rocketnet_time();
    for ( int i = 0; i < count; ++i )
    {
        if ( !split_client_send(clients[i].api, SPLIT_DRAINED_MAGIC,
                                (uint32_t) (first + i), 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        { result.offer_refusals++; goto cleanup; }
        rocketnet_client_send_packets(clients[i].api);
    }
    int finished = 0;
    while ( !quit && finished < count && rocketnet_time() - barrier_start < SPLIT_BARRIER_DEADLINE_S )
    {
        finished = 0;
        for ( int i = 0; i < count; ++i )
        {
            rocketnet_client_update(clients[i].api, rocketnet_time());
            split_proof_client_receipts(&result, &clients[i]);
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_runtime_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            finished += clients[i].finish;
            if ( rocketnet_client_state(clients[i].api) != ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                result.unexpected_disconnects++;
                split_client_note_disconnect(&result, &clients[i], first + i, run_id, "drain_barrier");
            }
            rocketnet_client_send_packets(clients[i].api);
        }
        split_capture_tick(&capture, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    if ( finished != count || result.unexpected_disconnects || result.protocol_errors || quit ) goto cleanup;
    for ( int i = 0; i < count; ++i )
    {
        if ( !split_client_send(clients[i].api, SPLIT_FINISH_ACK_MAGIC,
                                (uint32_t) (first + i), 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        { result.offer_refusals++; goto cleanup; }
        rocketnet_client_send_packets(clients[i].api);
    }
    const double confirm_start = rocketnet_time();
    int confirmed = 0;
    while ( !quit && confirmed < count && rocketnet_time() - confirm_start < SPLIT_BARRIER_DEADLINE_S )
    {
        confirmed = 0;
        for ( int i = 0; i < count; ++i )
        {
            rocketnet_client_update(clients[i].api, rocketnet_time());
            split_proof_client_receipts(&result, &clients[i]);
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_runtime_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            confirmed += clients[i].confirmed;
            if ( !clients[i].confirmed && rocketnet_client_state(clients[i].api) != ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                result.unexpected_disconnects++;
                split_client_note_disconnect(&result, &clients[i], first + i, run_id, "finish_barrier");
            }
            rocketnet_client_send_packets(clients[i].api);
        }
        split_capture_tick(&capture, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    result.barrier_seconds = rocketnet_time() - barrier_start;
    result.barrier_complete = confirmed == count && !quit && !result.protocol_errors;

cleanup:
    if ( clients )
    {
        split_print_streams("client-shard", run_id, first, count, NULL, clients);
        split_print_wire("client-shard", run_id, first, count, NULL, clients);
        for ( int i = 0; proofs && i < count; ++i )
        {
            if ( clients[i].api )
            {
                struct rocketnet_client_stats_t stats;
                rocketnet_client_stats(clients[i].api, &stats);
                result.proof_receipts_dropped += stats.receipts_dropped;
            }
            split_proof_print(&result, "client-shard", run_id, (uint32_t) (first + i), &proofs[i],
                              SPLIT_VERIFY_CLIENT_TO_SERVER);
        }
        if ( capture.entries ) split_capture_end(&capture, rocketnet_time());
        for ( int i = 0; i < count; ++i )
            if ( clients[i].api ) rn_cli_client_stop_and_destroy(clients[i].api);
        free(clients);
    }
    free(proofs);
    free(capture_entries);
    if ( api_started )
    {
        fds_pre_term = count_open_fds();
        threads_pre_term = count_threads();
        LOAD_CENSUS(LOAD_PRE_TERM);
        term_status = rocketnet_term();
        fds_post_term = count_open_fds();
        threads_post_term = count_threads();
        LOAD_CENSUS(LOAD_POST_TERM);
    }
    pass = split_print_receipt("client-shard", run_id, total, first, count, duration,
                               aggregate_limit_kbps, &result, fds_pre_init, fds_post_init,
                               fds_pre_term, fds_post_term, threads_pre_init, threads_post_init,
                               threads_pre_term, threads_post_term, term_status, transport);
    return pass ? 0 : 1;
}

static void split_usage( void )
{
    fprintf(stderr, "usage:\n"
            "  load_split plan-server <total>\n"
            "  load_split plan-client-shard <total> <local-count>\n"
            "  load_split server <total> <seconds> <run-id-16hex> <server-ipv4:port> <ready-ipv4> <aggregate-limit-kbps> [full|proof]\n"
            "  load_split client-shard <total> <seconds> <run-id-16hex> <first> <count> <server-ipv4:port> <server-key-64hex> <aggregate-limit-kbps> [full|proof]\n");
}

int main( int argc, char ** argv )
{
    if ( argc < 3 ) { split_usage(); return 2; }
    if ( !split_stock_profile_environment() ) return 2;
    int total = 0, duration = 0, first = 0, count = 0;
    uint64_t run_id = 0;
    double aggregate_limit_kbps = 0.0;
    if ( !split_parse_int(argv[2], 1, ROCKETNET_MAX_CLIENTS, &total) )
    { split_usage(); return 2; }
    if ( strcmp(argv[1], "plan-server") == 0 && argc == 3 )
        return split_plan("server", total, 0);
    if ( strcmp(argv[1], "plan-client-shard") == 0 && argc == 4 &&
         split_parse_int(argv[3], 1, total, &count) )
        return split_plan("client-shard", total, count);
    if ( argc < 8 || !split_parse_int(argv[3], 1, SPLIT_MAX_DURATION_S, &duration) ||
         !split_parse_run_id(argv[4], &run_id) )
    { split_usage(); return 2; }
    signal(SIGINT, interrupt_handler);
    const int server_args = strcmp(argv[1], "server") == 0 ? 8 : 10;
    if ( argc == server_args + 1 && strcmp(argv[argc - 1], "full") == 0 )
    {
        split_full_load = 1;
        argc--;
    }
    else if ( argc == server_args + 1 && strcmp(argv[argc - 1], "proof") == 0 )
    {
        split_proof_load = 1;
        argc--;
    }
    if ( strcmp(argv[1], "server") == 0 && argc == 8 &&
         strlen(argv[5]) < ROCKETNET_MAX_ADDRESS_STRING_BYTES && strchr(argv[5], ':') &&
         strlen(argv[6]) <= 45 && !strchr(argv[6], ':') &&
         split_parse_limit(argv[7], &aggregate_limit_kbps) )
        return split_server_run(total, duration, run_id, argv[5], argv[6], aggregate_limit_kbps);
    if ( strcmp(argv[1], "client-shard") == 0 && argc == 10 &&
         split_parse_int(argv[6], 1, total, &count) &&
         split_parse_int(argv[5], 0, total - count, &first) &&
         strlen(argv[7]) < ROCKETNET_MAX_ADDRESS_STRING_BYTES && strchr(argv[7], ':') &&
         split_parse_limit(argv[9], &aggregate_limit_kbps) )
    {
        uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
        if ( split_decode_key(argv[8], key) )
            return split_client_run(total, duration, run_id, first, count, argv[7], key,
                                    aggregate_limit_kbps);
    }
    split_usage();
    return 2;
}
