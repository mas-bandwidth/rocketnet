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
#define SPLIT_HEADER_BYTES 24
/* A space stream is one message a frame at most, and its sequence numbers
 * are tracked in a LOAD_MAX_SEQ_TRACKED bitmap: 68 seconds at 60 Hz. */
#define SPLIT_SPACE_MAX_DURATION_S ( (int) ( ( LOAD_MAX_SEQ_TRACKED - 1u ) / SPLIT_SPACE_FRAME_HZ ) )

#if SPLIT_SPACE_DELTA_BYTES > ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES
#error the space delta must fit one packet
#endif
#if SPLIT_SPACE_PLAYER_BYTES < SPLIT_HEADER_BYTES || SPLIT_SPACE_C2S_BYTES < SPLIT_HEADER_BYTES
#error a space message carries the 24-byte split header
#endif

struct split_message_t
{
    uint32_t client, combo, seq;
};

struct split_client_t
{
    struct rocketnet_client_t * api;
    struct load_stream_state_t c2s[LOAD_NUM_COMBOS];
    struct load_stream_state_t s2c;
    struct load_stream_state_t s2c_delta;           /* space: the UNRELIABLE_WITH_ACK_RECEIPT deltas */
    struct load_wire_sample_t wire_start, wire_end;
    double next_due;
    uint32_t next_combo;
    struct split_startup_t startup;
    int connected, finish, confirmed;
};

struct split_server_client_t
{
    struct load_stream_state_t c2s[LOAD_NUM_COMBOS];
    struct load_stream_state_t s2c;
    struct load_stream_state_t s2c_delta;           /* space: the UNRELIABLE_WITH_ACK_RECEIPT deltas */
    struct load_wire_sample_t wire_start, wire_end;
    int slot, connected, arm_acked, go_acked, done, drained, finish_sent, finish_acked;
    double next_due;
};

struct split_result_t
{
    uint64_t c2s_offered, c2s_accepted, c2s_received;
    uint64_t s2c_offered, s2c_accepted, s2c_received;
    uint64_t expected_offers, missed_due_slots;
    uint64_t late_application_deliveries;
    uint64_t wire_up, wire_down;
    double max_peer_up_kbps, max_peer_down_kbps;
    int connected, identity_errors, payload_errors, order_errors, duplicate_errors;
    int duration_target;
    int unexpected_disconnects, offer_refusals, protocol_errors;
    int startup_complete, traffic_complete, drain_complete, barrier_complete, load_complete, wire_valid;
    int barrier_active;
    int timing_valid, timing_budget_met;
    double connect_seconds, start_seconds, traffic_seconds, drain_seconds, barrier_seconds;
    double delivery_deadline;
    double aggregate_up_kbps, aggregate_down_kbps;
    uint64_t startup_utc_s, traffic_start_utc_s, traffic_end_utc_s;
    /* space only: server-to-client counts per mode (the player messages are
     * UNRELIABLE, the deltas UNRELIABLE_WITH_ACK_RECEIPT), and the SDK's own
     * loss evidence sampled at the end of the drain. Every client-to-server
     * message is UNRELIABLE, so the c2s counts above are that mode's. */
    int profile;
    uint64_t s2c_player_offered, s2c_player_accepted, s2c_player_received;
    uint64_t s2c_delta_offered, s2c_delta_accepted, s2c_delta_received;
    uint64_t receipts_acked, receipts_lost, messages_dropped_over_budget;
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

/* The space messages carry the same 24-byte header as the default ones; the
 * rest is a splitmix64 stream seeded from the header, eight bytes a step, so
 * a 1184-byte delta costs 148 steps to build or check. */
static uint64_t split_space_seed( uint32_t magic, uint32_t client, uint32_t combo, uint32_t seq, uint64_t run_id )
{
    return run_id ^ ((uint64_t) magic * 0x9E3779B97F4A7C15ull) ^ ((uint64_t) client * 0xC2B2AE3D27D4EB4Full) ^
           ((uint64_t) combo * 0x165667B19E3779F9ull) ^ ((uint64_t) seq * 0xD6E8FEB86659FD93ull);
}

static uint64_t split_space_next( uint64_t * state )
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* The size of a space message by direction and mode; 0 for none. */
static int split_space_message_bytes( uint32_t magic, uint32_t combo )
{
    if ( magic == LOAD_MAGIC_CLIENT_TO_SV )
        return combo == ROCKETNET_RELIABILITY_UNRELIABLE ? SPLIT_SPACE_C2S_BYTES : 0;
    if ( magic == LOAD_MAGIC_SV_TO_CLIENT )
        return combo == ROCKETNET_RELIABILITY_UNRELIABLE ? SPLIT_SPACE_PLAYER_BYTES :
               combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ? SPLIT_SPACE_DELTA_BYTES : 0;
    return 0;
}

static void split_build_space_message( uint8_t * out, int bytes, uint32_t magic,
                                       uint32_t client, uint32_t combo, uint32_t seq, uint64_t run_id )
{
    write_u32_le(out, magic);
    write_u32_le(out + 4, client);
    write_u32_le(out + 8, combo);
    write_u32_le(out + 12, seq);
    split_write_u64_le(out + 16, run_id);
    uint64_t state = split_space_seed(magic, client, combo, seq, run_id);
    for ( int i = SPLIT_HEADER_BYTES; i < bytes; i += 8 )
    {
        const uint64_t word = split_space_next(&state);
        for ( int k = 0; k < 8 && i + k < bytes; ++k ) out[i + k] = (uint8_t) (word >> (8 * k));
    }
}

static int split_verify_space_message( const uint8_t * data, int bytes, uint32_t magic,
                                       uint64_t run_id, struct split_message_t * message )
{
    if ( !data || bytes < SPLIT_HEADER_BYTES || read_u32_le(data) != magic ||
         split_read_u64_le(data + 16) != run_id )
        return 0;
    message->client = read_u32_le(data + 4);
    message->combo = read_u32_le(data + 8);
    message->seq = read_u32_le(data + 12);
    if ( bytes != split_space_message_bytes(magic, message->combo) ) return 0;
    uint64_t state = split_space_seed(magic, message->client, message->combo, message->seq, run_id);
    for ( int i = SPLIT_HEADER_BYTES; i < bytes; i += 8 )
    {
        const uint64_t word = split_space_next(&state);
        for ( int k = 0; k < 8 && i + k < bytes; ++k )
            if ( data[i + k] != (uint8_t) (word >> (8 * k)) ) return 0;
    }
    return 1;
}

/* The SDK reports its canonical receive channel 0 for these four modes, even
 * though their send API channel argument is the combo. */
static int split_receive_channel( uint32_t combo )
{
    return (combo == ROCKETNET_RELIABILITY_UNRELIABLE ||
            combo == ROCKETNET_RELIABILITY_RELIABLE ||
            combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ||
            combo == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT) ? 0 : (int) combo;
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
        if ( (length >= 17 && strncmp(variable, "ROCKETNET_SERVER_", 17) == 0 &&
              !(length == 26 && strncmp(variable, "ROCKETNET_SERVER_TRANSPORT", 26) == 0)) ||
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

static int split_server_send( struct rocketnet_server_t * server, int slot, uint32_t magic,
                              uint32_t client, uint32_t combo, uint32_t seq, uint64_t run_id,
                              enum rocketnet_reliability_t reliability, int channel )
{
    uint8_t * out = rocketnet_server_start_message(server, (uint64_t) slot, channel, reliability, LOAD_PAYLOAD_BYTES);
    if ( !out ) return 0;
    split_build_message(out, magic, client, combo, seq, run_id);
    return rocketnet_server_finish_message(server, LOAD_PAYLOAD_BYTES) != 0;
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

/* A space message sends on the channel numbered by its mode, as the default
 * client-to-server combos do. */
static int split_server_send_space( struct rocketnet_server_t * server, int slot, uint32_t client,
                                    uint32_t combo, uint32_t seq, uint64_t run_id )
{
    const int bytes = split_space_message_bytes(LOAD_MAGIC_SV_TO_CLIENT, combo);
    uint8_t * out = rocketnet_server_start_message(server, (uint64_t) slot, (int) combo,
                                                   (enum rocketnet_reliability_t) combo, bytes);
    if ( !out ) return 0;
    split_build_space_message(out, bytes, LOAD_MAGIC_SV_TO_CLIENT, client, combo, seq, run_id);
    return rocketnet_server_finish_message(server, bytes) != 0;
}

static int split_client_send_space( struct rocketnet_client_t * client, uint32_t global_client,
                                    uint32_t seq, uint64_t run_id )
{
    const uint32_t combo = ROCKETNET_RELIABILITY_UNRELIABLE;
    uint8_t * out = rocketnet_client_start_message(client, (int) combo, (enum rocketnet_reliability_t) combo,
                                                   SPLIT_SPACE_C2S_BYTES);
    if ( !out ) return 0;
    split_build_space_message(out, SPLIT_SPACE_C2S_BYTES, LOAD_MAGIC_CLIENT_TO_SV, global_client, combo, seq, run_id);
    return rocketnet_client_finish_message(client, SPLIT_SPACE_C2S_BYTES) != 0;
}

/* An unreliable space message may be lost, so a gap is loss, never an
 * order error; receiving one twice, or one never sent, is still an error. */
static void split_receive_c2s_space( struct split_result_t * result, struct split_server_client_t * clients,
                                     int total, uint64_t run_id, const struct rocketnet_message_batch_t * batch,
                                     int j, const struct rocketnet_server_t * server )
{
    struct split_message_t message;
    if ( !split_verify_space_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_CLIENT_TO_SV, run_id, &message) )
    {
        result->payload_errors++;
        return;
    }
    if ( message.client >= (uint32_t) total || message.combo != ROCKETNET_RELIABILITY_UNRELIABLE ||
         message.seq > split_space_frames(result->duration_target) ||
         batch->client_index[j] < 0 || batch->client_index[j] >= total ||
         clients[message.client].slot != batch->client_index[j] ||
         rocketnet_server_client_id(server, batch->client_index[j]) != (uint64_t) message.client + 1 )
    {
        result->identity_errors++;
        return;
    }
    if ( batch->channel[j] != split_receive_channel(message.combo) )
    {
        result->payload_errors++;
        return;
    }
    if ( result->barrier_active ||
         (result->delivery_deadline > 0.0 && rocketnet_time() >= result->delivery_deadline) )
    {
        result->late_application_deliveries++;
        return;
    }
    struct load_stream_state_t * stream = &clients[message.client].c2s[message.combo];
    if ( stream_seen_test_and_set(stream, message.seq) != 0 ) result->duplicate_errors++;
    if ( message.seq > stream->max_seq_seen ) stream->max_seq_seen = message.seq;
    stream->recv_count++;
    result->c2s_received++;
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
            struct split_server_client_t * peer = &clients[message.client];
            if ( magic == SPLIT_DONE_MAGIC && !peer->done ) peer->done = 1;
            else if ( magic == SPLIT_DRAINED_MAGIC && peer->done && !peer->drained ) peer->drained = 1;
            else if ( magic == SPLIT_FINISH_ACK_MAGIC && peer->finish_sent && !peer->finish_acked ) peer->finish_acked = 1;
            else result->protocol_errors++;
            continue;
        }
        if ( result->profile == SPLIT_PROFILE_SPACE )
        {
            split_receive_c2s_space(result, clients, total, run_id, batch, j, server);
            continue;
        }
        if ( !split_verify_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_CLIENT_TO_SV, run_id, &message) )
        {
            result->payload_errors++;
            continue;
        }
        if ( message.client >= (uint32_t) total || message.combo >= LOAD_NUM_COMBOS ||
             message.seq > split_expected_combo_offers(message.client, message.combo, result->duration_target) ||
             batch->client_index[j] < 0 || batch->client_index[j] >= total ||
             clients[message.client].slot != batch->client_index[j] ||
             rocketnet_server_client_id(server, batch->client_index[j]) != (uint64_t) message.client + 1 )
        {
            result->identity_errors++;
            continue;
        }
        if ( batch->channel[j] != split_receive_channel(message.combo) )
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

static void split_receive_s2c_space( struct split_result_t * result, struct split_client_t * client,
                                     uint32_t global_client, uint64_t run_id,
                                     const struct rocketnet_message_batch_t * batch, int j )
{
    struct split_message_t message;
    const uint64_t frames = split_space_frames(result->duration_target);
    if ( !split_verify_space_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_SV_TO_CLIENT, run_id, &message) ||
         message.client != global_client ||
         (message.combo == ROCKETNET_RELIABILITY_UNRELIABLE && message.seq > frames) ||
         (message.combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT &&
          message.seq > split_space_delta_offers(global_client, frames)) ||
         batch->channel[j] != split_receive_channel(message.combo) ||
         batch->client_index[j] != 0 )
    {
        result->payload_errors++;
        return;
    }
    if ( result->barrier_active ||
         (result->delivery_deadline > 0.0 && rocketnet_time() >= result->delivery_deadline) )
    {
        result->late_application_deliveries++;
        return;
    }
    const int delta = message.combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
    struct load_stream_state_t * stream = delta ? &client->s2c_delta : &client->s2c;
    if ( stream_seen_test_and_set(stream, message.seq) != 0 ) result->duplicate_errors++;
    if ( message.seq > stream->max_seq_seen ) stream->max_seq_seen = message.seq;
    stream->recv_count++;
    result->s2c_received++;
    if ( delta ) result->s2c_delta_received++;
    else result->s2c_player_received++;
}

static void split_receive_s2c( struct split_result_t * result, struct split_client_t * client,
                               uint32_t global_client, uint64_t run_id,
                               const struct rocketnet_message_batch_t * batch )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        struct split_message_t message;
        if ( result->profile == SPLIT_PROFILE_SPACE )
        {
            split_receive_s2c_space(result, client, global_client, run_id, batch, j);
            continue;
        }
        if ( !split_verify_message(batch->data[j], batch->bytes[j], LOAD_MAGIC_SV_TO_CLIENT, run_id, &message) ||
             message.client != global_client || message.combo != ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
             message.seq > (uint32_t) result->duration_target ||
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
static void split_client_startup_batch( struct split_result_t * result, struct split_client_t * peer,
                                        uint32_t global, uint64_t run_id,
                                        const struct rocketnet_message_batch_t * batch )
{
    for ( int j = 0; j < batch->num_messages; ++j )
    {
        struct split_message_t message;
        const uint32_t magic = batch->data[j] && batch->bytes[j] >= 4 ? read_u32_le(batch->data[j]) : 0;
        /* A space message is unreliable and does not share RUN's ordered
         * channel, so it can overtake a RUN that was lost and resent. The
         * server sends traffic only after every GO ACK. */
        if ( magic == LOAD_MAGIC_SV_TO_CLIENT &&
             (peer->startup.running || (result->profile == SPLIT_PROFILE_SPACE && peer->startup.go)) )
        {
            split_receive_s2c(result, peer, global, run_id, split_batch_entry(batch, j));
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
        if ( !split_startup_receive(&peer->startup, kind) ) result->protocol_errors++;
    }
}

/* ARM and GO are answered once the client is connected: its transport hands
 * payloads from before it reports CONNECTED, and a send before then is
 * dropped (tools/load_split_startup.h). */
static void split_client_startup_answer( struct split_result_t * result, struct split_client_t * peer,
                                         uint32_t global, uint64_t run_id )
{
    const int connected = rocketnet_client_state(peer->api) == ROCKETNET_CLIENT_STATE_CONNECTED;
    const int send = split_startup_due(&peer->startup, connected);
    if ( send == SPLIT_STARTUP_SEND_NONE ) return;
    if ( !split_client_send(peer->api, send == SPLIT_STARTUP_SEND_ACK ? SPLIT_ACK_MAGIC : SPLIT_GO_ACK_MAGIC,
                            global, 0, 1, run_id, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        result->offer_refusals++;
    split_startup_sent(&peer->startup, send);
}

static void split_client_runtime_batch( struct split_result_t * result, struct split_client_t * peer,
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
            if ( magic == SPLIT_FINISH_MAGIC && !peer->finish ) peer->finish = 1;
            else if ( magic == SPLIT_CONFIRM_MAGIC && peer->finish && !peer->confirmed ) peer->confirmed = 1;
            else result->protocol_errors++;
            continue;
        }
        split_receive_s2c(result, peer, global, run_id, split_batch_entry(batch, j));
    }
}

/* The fields that name a profile in readiness lines and receipts. The
 * default text is the established receipt's, unchanged. */
static void split_profile_fields( int profile, char * out, size_t bytes )
{
    if ( profile != SPLIT_PROFILE_SPACE )
    {
        snprintf(out, bytes, "profile=default payload_bytes=32 rate_target_hz=1");
        return;
    }
    snprintf(out, bytes, "profile=space payload_bytes=%d rate_target_hz=%d c2s_payload_bytes=%d c2s_rate_hz=%d "
             "s2c_player_bytes=%d s2c_player_rate_hz=%d s2c_delta_bytes=%d s2c_delta_rate_hz=%d "
             "s2c_delta_stagger=client_mod_%d server_frame_hz=%d client_up_cap_kbps=%d client_down_cap_kbps=%d",
             SPLIT_SPACE_C2S_BYTES, SPLIT_SPACE_FRAME_HZ, SPLIT_SPACE_C2S_BYTES, SPLIT_SPACE_FRAME_HZ,
             SPLIT_SPACE_PLAYER_BYTES, SPLIT_SPACE_FRAME_HZ, SPLIT_SPACE_DELTA_BYTES, SPLIT_SPACE_DELTA_HZ,
             SPLIT_SPACE_DELTA_STRIDE, SPLIT_SPACE_FRAME_HZ, SPLIT_SPACE_CLIENT_UP_CAP_KBPS,
             SPLIT_SPACE_CLIENT_DOWN_CAP_KBPS);
}

static void split_print_streams( const char * role, uint64_t run_id, int first, int count, int profile,
                                 const struct split_server_client_t * server_clients,
                                 const struct split_client_t * shard_clients )
{
    for ( int local = 0; local < count; ++local )
    {
        const int client = first + local;
        const struct load_stream_state_t * c2s = server_clients ? server_clients[client].c2s : shard_clients[local].c2s;
        const struct load_stream_state_t * s2c = server_clients ? &server_clients[client].s2c : &shard_clients[local].s2c;
        if ( profile == SPLIT_PROFILE_SPACE )
        {
            /* one UNRELIABLE stream up; UNRELIABLE players and
             * UNRELIABLE_WITH_ACK_RECEIPT deltas down */
            const struct load_stream_state_t * delta = server_clients ?
                &server_clients[client].s2c_delta : &shard_clients[local].s2c_delta;
            printf("split_stream: role=%s run_id=%016" PRIx64 " client=%d direction=c2s combo=%d sent=%u received=%u\n",
                   role, run_id, client, ROCKETNET_RELIABILITY_UNRELIABLE,
                   c2s[ROCKETNET_RELIABILITY_UNRELIABLE].sent_count, c2s[ROCKETNET_RELIABILITY_UNRELIABLE].recv_count);
            printf("split_stream: role=%s run_id=%016" PRIx64 " client=%d direction=s2c combo=%d sent=%u received=%u\n",
                   role, run_id, client, ROCKETNET_RELIABILITY_UNRELIABLE, s2c->sent_count, s2c->recv_count);
            printf("split_stream: role=%s run_id=%016" PRIx64 " client=%d direction=s2c combo=%d sent=%u received=%u\n",
                   role, run_id, client, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT,
                   delta->sent_count, delta->recv_count);
            continue;
        }
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
    /* A role's up is what it sends: client to server on a shard, server to
     * client on the server. The caps are a client's, per direction. */
    const int space = result->profile == SPLIT_PROFILE_SPACE;
    const int server_role = strcmp(role, "server") == 0;
    const double client_up_cap = space ? SPLIT_SPACE_CLIENT_UP_CAP_KBPS : SPLIT_DEFAULT_CLIENT_CAP_KBPS;
    const double client_down_cap = space ? SPLIT_SPACE_CLIENT_DOWN_CAP_KBPS : SPLIT_DEFAULT_CLIENT_CAP_KBPS;
    const double sent_cap = server_role ? client_down_cap : client_up_cap;
    const double received_cap = server_role ? client_up_cap : client_down_cap;
    char profile_text[1024];
    split_profile_fields(result->profile, profile_text, sizeof(profile_text));
    if ( space )
    {
        const size_t used = strlen(profile_text);
        snprintf(profile_text + used, sizeof(profile_text) - used,
                 " c2s_unreliable_offered=%" PRIu64 " c2s_unreliable_accepted=%" PRIu64
                 " c2s_unreliable_received=%" PRIu64 " s2c_player_offered=%" PRIu64
                 " s2c_player_accepted=%" PRIu64 " s2c_player_received=%" PRIu64
                 " s2c_delta_offered=%" PRIu64 " s2c_delta_accepted=%" PRIu64 " s2c_delta_received=%" PRIu64
                 " s2c_delta_receipts_acked=%" PRIu64 " s2c_delta_receipts_lost=%" PRIu64
                 " messages_dropped_over_budget=%" PRIu64,
                 result->c2s_offered, result->c2s_accepted, result->c2s_received,
                 result->s2c_player_offered, result->s2c_player_accepted, result->s2c_player_received,
                 result->s2c_delta_offered, result->s2c_delta_accepted, result->s2c_delta_received,
                 result->receipts_acked, result->receipts_lost, result->messages_dropped_over_budget);
    }
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
        !result->protocol_errors && !result->late_application_deliveries && result->wire_valid;
    const int bandwidth = result->max_peer_up_kbps < sent_cap && result->max_peer_down_kbps < received_cap &&
        result->aggregate_up_kbps < aggregate_limit_kbps && result->aggregate_down_kbps < aggregate_limit_kbps;
    const int timing = !server_role ||
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
           "max_peer_up_kbps=%.6f max_peer_down_kbps=%.6f aggregate_up_kbps=%.6f "
           "aggregate_down_kbps=%.6f aggregate_limit_kbps=%.6f wire_scope=pre_run_to_traffic_end_conservative "
           "wire_rate_denominator_s=%d "
           "workload_scope=traffic_plus_drain transport=%s %s "
           "fds_pre_init=%ld fds_post_init=%ld fds_pre_term=%ld fds_post_term=%ld "
           "threads_pre_init=%ld threads_post_init=%ld threads_pre_term=%ld threads_post_term=%ld "
           "resource_baseline_schema=%s warm_cycle=1 survivor_threads_operational=%ld "
           "survivor_handles_operational=%ld survivor_threads_total=%ld survivor_handles_total=%ld "
           "os_pool_threads=%ld "
           "term_status_code=%d resource_baseline=%s delivery_verdict=%s load_verdict=%s "
           "bandwidth_verdict=%s timing_verdict=%s timing_budget_us=100 "
           "timing_budget_basis=server_update_send_p99_upper verdict=%s\n",
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
           result->max_peer_up_kbps, result->max_peer_down_kbps,
           result->aggregate_up_kbps, result->aggregate_down_kbps, aggregate_limit_kbps, duration,
           transport ? transport : "none", profile_text, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
           threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
           census ? "census_v1" : "phase_counts_v1", operational.threads, operational.handles,
           whole.threads, whole.handles, whole.os_pool_threads,
           (int) term_status, resources ? "PASS" : "FAIL", correctness ? "PASS" : "FAIL",
           result->load_complete ? "PASS" : "FAIL", bandwidth ? "PASS" : "FAIL",
           strcmp(role, "server") == 0 ? (timing ? "PASS" : "FAIL") : "NA",
           pass ? "PASS" : "FAIL");
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
 * at once; either failure names the clients still owed. */
static int split_server_start_phase( struct rocketnet_server_t * server, struct split_server_client_t * clients,
                                     int total, uint64_t run_id, enum split_start_step_t phase,
                                     struct split_result_t * result, struct split_capture_t * capture )
{
    const uint32_t send_magic = phase == SPLIT_START_STEP_ARM ? SPLIT_ARM_MAGIC : SPLIT_GO_MAGIC;
    const uint32_t ack_magic = phase == SPLIT_START_STEP_ARM ? SPLIT_ACK_MAGIC : SPLIT_GO_ACK_MAGIC;
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, send_magic, (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
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
            if ( !done && !split_server_client_live(server, clients, i) ) disconnected++;
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

/* One server offer on one space stream, counted under its mode. */
static void split_server_offer_space( struct split_result_t * result, struct rocketnet_server_t * server,
                                      int slot, uint32_t client, uint32_t combo, uint64_t run_id,
                                      struct load_stream_state_t * stream, uint64_t * offered, uint64_t * accepted )
{
    if ( stream->sent_count + 1 >= LOAD_MAX_SEQ_TRACKED ) { result->protocol_errors++; return; }
    const uint32_t seq = stream->sent_count + 1;
    result->s2c_offered++;
    (*offered)++;
    if ( !split_server_send_space(server, slot, client, combo, seq, run_id) ) { result->offer_refusals++; return; }
    stream->sent_count = seq;
    result->s2c_accepted++;
    (*accepted)++;
}

/* One space server frame: a player message to every client, and a delta to
 * the clients whose index mod 3 matches the frame's. */
static void split_server_space_frame( struct split_result_t * result, struct rocketnet_server_t * server,
                                      struct split_server_client_t * clients, int total, uint64_t run_id,
                                      uint64_t frame )
{
    for ( int i = 0; i < total; ++i )
    {
        if ( !split_server_client_live(server, clients, i) ) { result->unexpected_disconnects++; continue; }
        split_server_offer_space(result, server, clients[i].slot, (uint32_t) i, ROCKETNET_RELIABILITY_UNRELIABLE,
                                 run_id, &clients[i].s2c, &result->s2c_player_offered, &result->s2c_player_accepted);
        if ( split_space_delta_due(frame, (uint32_t) i) )
            split_server_offer_space(result, server, clients[i].slot, (uint32_t) i,
                                     ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, run_id, &clients[i].s2c_delta,
                                     &result->s2c_delta_offered, &result->s2c_delta_accepted);
    }
}

/* Sleep to the next frame of the traffic phase, again after an early wake. */
static void split_sleep_to_next_frame( double traffic_start )
{
    const double next = split_frame_next_time(rocketnet_time(), traffic_start, SPLIT_SPACE_FRAME_HZ);
    for ( double left = next - rocketnet_time(); left > 0.0 && !quit; left = next - rocketnet_time() )
        rocketnet_sleep(left);
}

static int split_server_run( int total, int duration, uint64_t run_id, const char * server_endpoint,
                             const char * ready_ipv4, double aggregate_limit_kbps, int profile )
{
    struct split_result_t result = { 0 };
    result.duration_target = duration;
    result.profile = profile;
    char profile_fields[512];
    split_profile_fields(profile, profile_fields, sizeof(profile_fields));
    struct split_server_client_t * clients = NULL;
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
    clients = (struct split_server_client_t *) calloc((size_t) total, sizeof(*clients));
    capture_entries = (struct split_capture_entry_t *) calloc((size_t) total, sizeof(*capture_entries));
    if ( !clients || !capture_entries ) goto cleanup;
    for ( int i = 0; i < total; ++i ) clients[i].slot = -1;
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
           "duration_s=%d %s "
           "address=%s:%u key=%s transport=%s startup_utc_s=%" PRIu64 "\n",
           run_id, total, duration, profile_fields, ready_ipv4, (unsigned) rocketnet_server_port(server),
           key_hex, transport, result.startup_utc_s);
    fflush(stdout);

    /* The launcher has a 60-second readiness lease. Once the first client
     * connects, the established 10-second population deadline applies. */
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
            struct split_server_client_t * peer = &clients[id - 1];
            if ( peer->connected && peer->slot != slot ) { result.identity_errors++; continue; }
            peer->slot = slot;
            peer->connected = 1;
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
     * cannot consume traffic before its RUN even if clocks or network lag. */
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, SPLIT_RUN_MAGIC,
                                (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED) )
        { result.offer_refusals++; goto cleanup; }
    rocketnet_server_send_packets(server);
    result.startup_complete = 1;
    result.start_seconds = rocketnet_time() - arm_start;
    const double traffic_start = rocketnet_time();
    result.traffic_start_utc_s = (uint64_t) time(NULL);
    for ( int i = 0; i < total; ++i )
        clients[i].next_due = traffic_start;
    result.expected_offers = split_expected_offers(profile, 1, 0, total, duration);
    timing.enabled = 1;
    double next_tick = traffic_start;
    uint64_t next_frame = 0;
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
                if ( up_kbps > result.max_peer_up_kbps ) result.max_peer_up_kbps = up_kbps;
                if ( down_kbps > result.max_peer_down_kbps ) result.max_peer_down_kbps = down_kbps;
            }
            result.aggregate_up_kbps = (double) result.wire_up * 8.0 / ((double) duration * 1000.0);
            result.aggregate_down_kbps = (double) result.wire_down * 8.0 / ((double) duration * 1000.0);
            result.traffic_complete = result.traffic_seconds >= (double) duration;
        }
        LOAD_TIMED_VOID(&timing, LOAD_TIMING_SERVER_UPDATE, rocketnet_server_update(server, now));
        /* An update may itself cross the drain deadline. Do not let its
         * newly published batch repair delivery after the fixed window. */
        if ( rocketnet_time() >= drain_end ) result.barrier_active = 1;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        if ( profile == SPLIT_PROFILE_SPACE )
        {
            /* One frame per wake. A late wake runs the latest due frame and
             * counts every client's share of the frames it skipped. */
            uint64_t frame = 0, missed_frames = 0;
            if ( in_traffic &&
                 split_frame_due(rocketnet_time(), traffic_start, end_time,
                                 SPLIT_SPACE_FRAME_HZ, split_space_frames(duration), &next_frame, &missed_frames, &frame) )
            {
                result.missed_due_slots += missed_frames * (uint64_t) total;
                split_server_space_frame(&result, server, clients, total, run_id, frame);
            }
        }
        for ( int i = 0; profile == SPLIT_PROFILE_DEFAULT && in_traffic && i < total; ++i )
        {
            if ( !rocketnet_server_client_connected(server, clients[i].slot) ||
                 rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1 )
            { result.unexpected_disconnects++; continue; }
            /* Use the offer-time clock for both the deadline and due phase.
             * An update may have crossed one or more due slots since now. */
            const double offer_now = rocketnet_time();
            if ( offer_now >= end_time ) break;
            if ( !split_offer_due(offer_now, end_time, &clients[i].next_due,
                                  &result.missed_due_slots) ) continue;
            if ( clients[i].s2c.sent_count + 1 >= LOAD_MAX_SEQ_TRACKED )
            { result.protocol_errors++; continue; }
            result.s2c_offered++;
            const uint32_t seq = clients[i].s2c.sent_count + 1;
            if ( !split_server_send(server, clients[i].slot, LOAD_MAGIC_SV_TO_CLIENT,
                                    (uint32_t) i, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                    seq, run_id, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                    ROCKETNET_RELIABILITY_RELIABLE_ORDERED) )
            { result.offer_refusals++; continue; }
            clients[i].s2c.sent_count = seq;
            result.s2c_accepted++;
        }
        LOAD_TIMED_VOID(&timing, LOAD_TIMING_SERVER_SEND_PACKETS, rocketnet_server_send_packets(server));
        split_capture_tick(&capture, rocketnet_time());
        if ( profile == SPLIT_PROFILE_SPACE )
        {
            split_sleep_to_next_frame(traffic_start);
            continue;
        }
        next_tick += 0.010;
        const double left = next_tick - rocketnet_time();
        if ( left > 0.0 ) rocketnet_sleep(left);
    }
    result.drain_seconds = rocketnet_time() - end_time;
    if ( profile == SPLIT_PROFILE_SPACE )
    {
        /* Only the deltas ask for receipts, so the server's receipt counts
         * are theirs: the SDK's loss evidence at the end of the drain. */
        struct rocketnet_server_stats_t stats;
        rocketnet_server_stats(server, &stats);
        result.receipts_acked = stats.receipts_acked;
        result.receipts_lost = stats.receipts_lost;
        result.messages_dropped_over_budget = stats.messages_dropped_over_budget;
    }
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
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( (batch = rocketnet_server_receive_messages(server)) != NULL )
            split_receive_c2s(&result, clients, total, run_id, batch, server);
        drained = 0;
        for ( int i = 0; i < total; ++i )
        {
            drained += clients[i].drained;
            if ( !rocketnet_server_client_connected(server, clients[i].slot) ||
                 rocketnet_server_client_id(server, clients[i].slot) != (uint64_t) i + 1 )
                result.unexpected_disconnects++;
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
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        { result.offer_refusals++; goto cleanup; }
        clients[i].finish_sent = 1;
    }
    rocketnet_server_send_packets(server);
    const double ack_start = rocketnet_time();
    int finish_acked = 0;
    while ( !quit && finish_acked < total && rocketnet_time() - ack_start < SPLIT_BARRIER_DEADLINE_S )
    {
        rocketnet_server_update(server, rocketnet_time());
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
                result.unexpected_disconnects++;
        }
        rocketnet_server_send_packets(server);
        split_capture_tick(&capture, rocketnet_time());
        rocketnet_sleep(0.001);
    }
    if ( finish_acked != total || result.protocol_errors || result.unexpected_disconnects || quit ) goto cleanup;
    for ( int i = 0; i < total; ++i )
        if ( !split_server_send(server, clients[i].slot, SPLIT_CONFIRM_MAGIC,
                                (uint32_t) i, 0, 1, run_id,
                                ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SPLIT_CONTROL_CHANNEL) )
        { result.offer_refusals++; goto cleanup; }
    /* Keep retransmission running after confirmation. Shards record their own
     * confirmation; the merger rejects any shard that missed it. */
    const double confirm_start = rocketnet_time();
    while ( !quit && rocketnet_time() - confirm_start < SPLIT_CONFIRM_GRACE_S )
    {
        rocketnet_server_update(server, rocketnet_time());
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
            split_print_streams("server", run_id, 0, total, profile, clients, NULL);
            split_print_wire("server", run_id, 0, total, clients, NULL);
        }
        if ( capture.entries ) split_capture_end(&capture, rocketnet_time());
        rn_cli_server_stop_and_destroy(server);
    }
    free(clients);
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
        free(timing.buckets);
    }
    pass = split_print_receipt("server", run_id, total, 0, total, duration, aggregate_limit_kbps,
                               &result, fds_pre_init, fds_post_init, fds_pre_term, fds_post_term,
                               threads_pre_init, threads_post_init, threads_pre_term, threads_post_term,
                               term_status, transport);
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
                             double aggregate_limit_kbps, int profile )
{
    struct split_result_t result = { 0 };
    result.duration_target = duration;
    result.profile = profile;
    char profile_fields[512];
    split_profile_fields(profile, profile_fields, sizeof(profile_fields));
    struct split_client_t * clients = NULL;
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
           " total=%d first=%d count=%d duration_s=%d %s "
           "startup_utc_s=%" PRIu64 "\n",
           run_id, total, first, count, duration, profile_fields, result.startup_utc_s);
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
     * interval. */
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
    result.expected_offers = split_expected_offers(profile, 0, first, count, duration);
    const double traffic_end = traffic_start + (double) duration;
    const double drain_end = traffic_end + 3.0;
    result.delivery_deadline = drain_end;
    double next_tick = traffic_start;
    uint64_t next_frame = 0;
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
                if ( up_kbps > result.max_peer_up_kbps ) result.max_peer_up_kbps = up_kbps;
                if ( down_kbps > result.max_peer_down_kbps ) result.max_peer_down_kbps = down_kbps;
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
        /* A space frame visits every local client once: each updates,
         * offers its input when the shard's frame is due, and sends. */
        uint64_t frame = 0, missed_frames = 0;
        const int space_frame = profile == SPLIT_PROFILE_SPACE && in_traffic &&
            split_frame_due(rocketnet_time(), traffic_start, traffic_end,
                            SPLIT_SPACE_FRAME_HZ, split_space_frames(duration), &next_frame, &missed_frames, &frame);
        result.missed_due_slots += missed_frames * (uint64_t) count;
        const int slice = in_traffic && profile == SPLIT_PROFILE_DEFAULT ? (count < 100 ? count : 100) : count;
        for ( int k = 0; k < slice; ++k )
        {
            if ( !in_traffic && rocketnet_time() >= drain_end ) break;
            const int local = cursor;
            cursor = (cursor + 1) % count;
            struct split_client_t * peer = &clients[local];
            rocketnet_client_update(peer->api, rocketnet_time());
            if ( rocketnet_time() >= drain_end ) result.barrier_active = 1;
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(peer->api)) != NULL )
                split_client_runtime_batch(&result, peer, (uint32_t) (first + local), run_id, batch);
            const enum rocketnet_client_state_t state = rocketnet_client_state(peer->api);
            if ( state != ROCKETNET_CLIENT_STATE_CONNECTED )
                result.unexpected_disconnects++;
            if ( profile == SPLIT_PROFILE_SPACE )
            {
                struct load_stream_state_t * stream = &peer->c2s[ROCKETNET_RELIABILITY_UNRELIABLE];
                if ( space_frame && state == ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    const uint32_t seq = stream->sent_count + 1;
                    if ( seq >= LOAD_MAX_SEQ_TRACKED ) result.protocol_errors++;
                    else
                    {
                        result.c2s_offered++;
                        if ( !split_client_send_space(peer->api, (uint32_t) (first + local), seq, run_id) )
                            result.offer_refusals++;
                        else
                        {
                            stream->sent_count = seq;
                            result.c2s_accepted++;
                        }
                    }
                }
                rocketnet_client_send_packets(peer->api);
                continue;
            }
            /* A client update can cross a due slot. The same fresh clock
             * decides traffic eligibility and counts every skipped slot. */
            const double offer_now = rocketnet_time();
            if ( in_traffic && state == ROCKETNET_CLIENT_STATE_CONNECTED &&
                 split_offer_due(offer_now, traffic_end, &peer->next_due,
                                 &result.missed_due_slots) )
            {
                const uint32_t combo = peer->next_combo % LOAD_NUM_COMBOS;
                peer->next_combo++;
                struct load_stream_state_t * stream = &peer->c2s[combo];
                if ( stream->sent_count + 1 >= LOAD_MAX_SEQ_TRACKED ) result.protocol_errors++;
                else
                {
                    const uint32_t seq = stream->sent_count + 1;
                    result.c2s_offered++;
                    if ( !split_client_send(peer->api, LOAD_MAGIC_CLIENT_TO_SV,
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
            rocketnet_client_send_packets(peer->api);
        }
        split_capture_tick(&capture, rocketnet_time());
        if ( profile == SPLIT_PROFILE_SPACE )
        {
            split_sleep_to_next_frame(traffic_start);
            continue;
        }
        next_tick += 0.010;
        const double left = next_tick - rocketnet_time();
        if ( left > 0.0 ) rocketnet_sleep(left);
    }
    result.drain_seconds = rocketnet_time() - traffic_end;
    if ( profile == SPLIT_PROFILE_SPACE )
        for ( int i = 0; i < count; ++i )
        {
            struct rocketnet_client_stats_t stats;
            rocketnet_client_stats(clients[i].api, &stats);
            result.receipts_acked += stats.receipts_acked;
            result.receipts_lost += stats.receipts_lost;
            result.messages_dropped_over_budget += stats.messages_dropped_over_budget;
        }
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
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_runtime_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            finished += clients[i].finish;
            if ( rocketnet_client_state(clients[i].api) != ROCKETNET_CLIENT_STATE_CONNECTED )
                result.unexpected_disconnects++;
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
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( (batch = rocketnet_client_receive_messages(clients[i].api)) != NULL )
                split_client_runtime_batch(&result, &clients[i], (uint32_t) (first + i), run_id, batch);
            confirmed += clients[i].confirmed;
            if ( !clients[i].confirmed && rocketnet_client_state(clients[i].api) != ROCKETNET_CLIENT_STATE_CONNECTED )
                result.unexpected_disconnects++;
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
        split_print_streams("client-shard", run_id, first, count, profile, NULL, clients);
        split_print_wire("client-shard", run_id, first, count, NULL, clients);
        if ( capture.entries ) split_capture_end(&capture, rocketnet_time());
        for ( int i = 0; i < count; ++i )
            if ( clients[i].api ) rn_cli_client_stop_and_destroy(clients[i].api);
        free(clients);
    }
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
            "  load_split server <total> <seconds> <run-id-16hex> <server-ipv4:port> <ready-ipv4> <aggregate-limit-kbps> [default|space]\n"
            "  load_split client-shard <total> <seconds> <run-id-16hex> <first> <count> <server-ipv4:port> <server-key-64hex> <aggregate-limit-kbps> [default|space]\n"
            "The optional last argument selects the traffic profile; default when absent. space runs at most %d seconds.\n",
            SPLIT_SPACE_MAX_DURATION_S);
}

/* The optional profile argument: absent is default. space is bounded by
 * its sequence tracking. */
static int split_parse_profile( int argc, int last, char ** argv, int duration, int * profile )
{
    *profile = SPLIT_PROFILE_DEFAULT;
    if ( argc == last ) return 1;
    if ( argc != last + 1 ) return 0;
    if ( strcmp(argv[last], "default") == 0 ) return 1;
    if ( strcmp(argv[last], "space") != 0 || duration > SPLIT_SPACE_MAX_DURATION_S ) return 0;
    *profile = SPLIT_PROFILE_SPACE;
    return 1;
}

int main( int argc, char ** argv )
{
    if ( argc < 3 ) { split_usage(); return 2; }
    if ( !split_stock_profile_environment() ) return 2;
    int total = 0, duration = 0, first = 0, count = 0;
    uint64_t run_id = 0;
    double aggregate_limit_kbps = 0.0;
    int profile = SPLIT_PROFILE_DEFAULT;
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
    if ( strcmp(argv[1], "server") == 0 && split_parse_profile(argc, 8, argv, duration, &profile) &&
         strlen(argv[5]) < ROCKETNET_MAX_ADDRESS_STRING_BYTES && strchr(argv[5], ':') &&
         strlen(argv[6]) <= 45 && !strchr(argv[6], ':') &&
         split_parse_limit(argv[7], &aggregate_limit_kbps) )
        return split_server_run(total, duration, run_id, argv[5], argv[6], aggregate_limit_kbps, profile);
    if ( strcmp(argv[1], "client-shard") == 0 && split_parse_profile(argc, 10, argv, duration, &profile) &&
         split_parse_int(argv[6], 1, total, &count) &&
         split_parse_int(argv[5], 0, total - count, &first) &&
         strlen(argv[7]) < ROCKETNET_MAX_ADDRESS_STRING_BYTES && strchr(argv[7], ':') &&
         split_parse_limit(argv[9], &aggregate_limit_kbps) )
    {
        uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
        if ( split_decode_key(argv[8], key) )
            return split_client_run(total, duration, run_id, first, count, argv[7], key,
                                    aggregate_limit_kbps, profile);
    }
    split_usage();
    return 2;
}
