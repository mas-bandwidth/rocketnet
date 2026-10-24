/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The manual worker consumes production ACK deltas with an explicit step.
// Transport updates and publication still run through the public server.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_layers.h"
#include "../sdk/rocketnet_ack.h"
#include "../sdk/rocketnet_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define STATS_PROTOCOL 0x715A7A7512345678ULL

// The transport env pin intentionally outranks the struct field. Keep this
// fixture on the in-memory backend even in a CI matrix pinned to a native one.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
static int stats_setenv( const char * name, const char * value ) { return _putenv_s(name, value); }
static int stats_unsetenv( const char * name ) { return _putenv_s(name, ""); }
#else
static int stats_setenv( const char * name, const char * value ) { return setenv(name, value, 1); }
static int stats_unsetenv( const char * name ) { return unsetenv(name); }
#endif

struct stats_env_pin_t
{
    bool set;
    char value[256];
    stats_env_pin_t()
    {
        set = rocketnet_test_getenv("ROCKETNET_SERVER_TRANSPORT", value, sizeof(value)) != 0;
        check(stats_unsetenv("ROCKETNET_SERVER_TRANSPORT") == 0);
    }
    bool restore()
    {
        if ( !set ) return true;
        if ( stats_setenv("ROCKETNET_SERVER_TRANSPORT", value) != 0 ) return false;
        set = false;
        return true;
    }
    ~stats_env_pin_t() noexcept
    {
        // Failure unwinding must not throw a second exception. Successful
        // paths call restore() and check its result explicitly below.
        (void)restore();
    }
};

static double stats_now = 1000.0;
static double stats_clock( void * ) { return stats_now; }

struct stats_world_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * clients[2];
    int slots[2];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    int count;
    double time;
};

static rocketnet_server_transport_t * stats_transport( stats_world_t * world )
{
    return rocketnet_message_server_transport( rocketnet_server_message_server( world->server ) );
}

static void stats_pump( stats_world_t * world )
{
    stats_now = world->time;
    for ( int i = 0; i < world->count; ++i )
        rocketnet_client_update( world->clients[i], world->time );
    for ( int i = 0; i < world->count; ++i )
        rocketnet_client_send_packets( world->clients[i] );
    rocketnet_server_update( world->server, world->time );
    rocketnet_server_send_packets( world->server );
    world->time += 0.1;
}

static void stats_world_create( stats_world_t * world, int capacity, int count )
{
    memset( world, 0, sizeof(*world) );
    world->time = stats_now = 1000.0;
    world->count = count;
    uint8_t seed[32];
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof(rocketnet_dev_server_secret_key)];
    for ( int i = 0; i < 32; ++i ) seed[i] = (uint8_t)(0x5A ^ i);
    check( rocketnet_generate_backend_keypair_from_seed(seed, backend_public, world->backend_secret) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair(world->server_public, server_secret) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t config;
    check( rocketnet_server_default_config(&config) == ROCKETNET_STATUS_OK );
    config.protocol_id = STATS_PROTOCOL;
    config.max_clients = capacity;
    config.transport = "loopback";
    memcpy(config.backend_public_key, backend_public, sizeof(backend_public));
    rocketnet_server_transport_set_stats_worker_manual_for_test(true);
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create(NULL, "127.0.0.1:0", NULL, &config, &status);
    check(world->server && status == ROCKETNET_STATUS_OK);
    rocketnet_server_public_key(world->server, world->server_public);
    check(rocketnet_server_start(world->server) == ROCKETNET_STATUS_OK);
    for ( int i = 0; i < count; ++i )
    {
        rocketnet_client_test_config_t client_config;
        check(rocketnet_client_default_config(&client_config) == ROCKETNET_STATUS_OK);
        client_config.transport = "loopback";
        client_config.step_driven = true;
        client_config.time_callback = stats_clock;
        world->clients[i] = rocketnet_client_create(NULL, &client_config, &status);
        check(world->clients[i] && status == ROCKETNET_STATUS_OK);
    }
}

static void stats_connect( stats_world_t * world, int client, uint64_t client_id )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char address[64];
    snprintf(address, sizeof(address), "127.0.0.1:%u", (unsigned)rocketnet_server_port(world->server));
    const char * addresses[] = {address};
    check(rocketnet_write_production_connect_token(world->backend_secret, world->server_public,
        STATS_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, 300, NULL,
        token, sizeof(token)) == ROCKETNET_STATUS_OK);
    check(rocketnet_base64_encode_data(token, sizeof(token), token_string, sizeof(token_string)) ==
          ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1);
    check(rocketnet_client_connect(world->clients[client], token_string) == ROCKETNET_STATUS_OK);
    for ( int n = 0; n < 400 && rocketnet_client_state(world->clients[client]) != ROCKETNET_CLIENT_STATE_CONNECTED; ++n )
        stats_pump(world);
    check(rocketnet_client_state(world->clients[client]) == ROCKETNET_CLIENT_STATE_CONNECTED);
    world->slots[client] = rocketnet_client_index(world->clients[client]);
    for ( int n = 0; n < 400 && !rocketnet_server_client_connected(world->server, world->slots[client]); ++n )
        stats_pump(world);
    check(rocketnet_server_client_connected(world->server, world->slots[client]) != 0);
    check(rocketnet_server_transport_stats_endpoint_for_test(stats_transport(world), world->slots[client]) != NULL);
}

static void stats_world_destroy( stats_world_t * world )
{
    for ( int i = 0; i < world->count; ++i )
        rn_test_client_stop_and_destroy(world->clients[i]);
    rn_test_server_stop_and_destroy(world->server);
}

static void stats_check_six( const rocketnet_server_client_stats_t & actual,
                             const rocketnet_ack_endpoint_t & expected )
{
    check(fabs(actual.rtt_ms - (double) expected.rtt) < 0.001);
    check(fabs(actual.jitter_ms - (double) expected.jitter) < 0.001);
    check(fabs(actual.packet_loss_percent - (double) expected.packet_loss) < 0.001);
    check(fabs(actual.bandwidth_sent_kbps - (double) expected.bandwidth_sent_kbps) < 0.001);
    check(fabs(actual.bandwidth_received_kbps - (double) expected.bandwidth_received_kbps) < 0.001);
    check(fabs(actual.bandwidth_acked_kbps - (double) expected.bandwidth_acked_kbps) < 0.001);
}

static void stats_check_unchanged( const rocketnet_server_client_stats_t & actual,
                                   const rocketnet_server_client_stats_t & prior )
{
    check(actual.rtt_ms == prior.rtt_ms);
    check(actual.jitter_ms == prior.jitter_ms);
    check(actual.packet_loss_percent == prior.packet_loss_percent);
    check(actual.bandwidth_sent_kbps == prior.bandwidth_sent_kbps);
    check(actual.bandwidth_received_kbps == prior.bandwidth_received_kbps);
    check(actual.bandwidth_acked_kbps == prior.bandwidth_acked_kbps);
}

// Exercise the same producer calls used by accepted protocol sends, receives
// and ACKs. Direct writes to live history would bypass the worker's deltas.
static int stats_ack_one( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_ack_block_t block = {};
    block.largest = sequence;
    block.first_range_length = 1;
    uint64_t ids[1] = {}, unknown = 0, full = 0;
    int count = 0;
    const int newly_acked = rocketnet_ack_endpoint_process_block(endpoint, &block, time,
        ids, 1, &count, &unknown, &full);
    check(unknown == 0 && full == 0);
    return newly_acked;
}

static uint64_t stats_add_sample( rocketnet_ack_endpoint_t * endpoint, double time, double rtt_ms )
{
    // Keep synthetic producer records ahead of the real loopback connection's
    // wire sequence so its later maintenance cannot overwrite this cohort.
    const uint64_t sequence = endpoint->highest_sequence + ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;
    check(rocketnet_ack_endpoint_record_sent_classified(endpoint, sequence, 120, 0, 0, 0, time)
          != ROCKETNET_ACK_SENT_REFUSED_STALE);
    check(stats_ack_one(endpoint, sequence, time + rtt_ms / 1000.0) == 1);
    return sequence;
}

static rocketnet_ack_endpoint_t * stats_reference( const rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_ack_endpoint_t * expected = new rocketnet_ack_endpoint_t(*endpoint);
    expected->stats_delta = NULL;
    rocketnet_ack_endpoint_update_stats(expected);
    return expected;
}

static void stats_step_and_update( stats_world_t * world )
{
    check(rocketnet_server_transport_stats_worker_step_for_test(stats_transport(world)));
    world->time += 0.001;
    rocketnet_server_update(world->server, world->time);
}

static void test_server_stats_worker_publishes_only_on_next_update()
{
    stats_env_pin_t pin;
    stats_world_t world;
    stats_world_create(&world, 1, 1);
    stats_connect(&world, 0, 0xAC01);
    rocketnet_server_transport_t * transport = stats_transport(&world);
    rocketnet_ack_endpoint_t * endpoint = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[0]);
    check(endpoint);
    for ( int i = 0; i < 4; ++i ) stats_step_and_update(&world);

    stats_add_sample(endpoint, world.time, 7.0);
    stats_add_sample(endpoint, world.time, 17.0);
    rocketnet_ack_endpoint_record_received(endpoint, 200, 100, world.time);
    rocketnet_ack_endpoint_record_received(endpoint, 201, 100, world.time + 0.01);
    const rocketnet_server_client_stats_t before = rn_server_client_stats_of(world.server, world.slots[0]);
    uint64_t submitted = 0, completed = 0, busy = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &submitted, &completed, NULL, &busy));
    rocketnet_server_update(world.server, world.time + 0.02);
    uint64_t submitted_after = 0, completed_after = 0, busy_after = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &submitted_after, &completed_after, NULL, &busy_after));
    check(submitted_after == submitted && completed_after == completed && busy_after == busy + 1);
    stats_check_unchanged(rn_server_client_stats_of(world.server, world.slots[0]), before);

    // Finish the older sealed delta, then swap in the producer mutations.
    world.time += 0.02;
    stats_step_and_update(&world);
    rocketnet_ack_endpoint_t * expected = stats_reference(endpoint);
    check(fabsf(expected->rtt - 7.0f) < 0.001f && expected->bandwidth_received_kbps > 0.0f);
    const rocketnet_server_client_stats_t prior = rn_server_client_stats_of(world.server, world.slots[0]);
    check(rocketnet_server_transport_stats_worker_step_for_test(transport));
    stats_check_unchanged(rn_server_client_stats_of(world.server, world.slots[0]), prior);
    world.time += 0.001;
    rocketnet_server_update(world.server, world.time);
    stats_check_six(rn_server_client_stats_of(world.server, world.slots[0]), *expected);
    delete expected;
    stats_world_destroy(&world);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_publishes_only_on_next_update);

static void test_server_stats_worker_late_batch_keeps_modulo_fairness()
{
    stats_env_pin_t pin;
    stats_world_t world;
    stats_world_create(&world, 21, 2);
    stats_connect(&world, 0, 0xAC11);
    stats_connect(&world, 1, 0xAC12);
    check(world.slots[0] != world.slots[1]);
    rocketnet_server_transport_t * transport = stats_transport(&world);
    rocketnet_ack_endpoint_t * first = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[0]);
    rocketnet_ack_endpoint_t * second = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[1]);
    check(first && second);
    stats_add_sample(first, world.time, 11.0);
    stats_add_sample(second, world.time, 29.0);

    uint64_t submitted = 0, completed = 0, published = 0, busy = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &submitted, &completed, &published, &busy));
    for ( int i = 0; i < 8; ++i )
    {
        world.time += 0.01;
        rocketnet_server_update(world.server, world.time);
    }
    uint64_t held_submitted = 0, held_busy = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &held_submitted, NULL, NULL, &held_busy));
    check(held_submitted == submitted && held_busy >= busy + 8);
    check(rocketnet_server_client_connected(world.server, world.slots[0]));
    check(rocketnet_server_client_connected(world.server, world.slots[1]));

    // The pending banks are drained in ordinal order. A later phase must
    // still visit both slots despite the earlier worker delay.
    for ( int i = 0; i < 12; ++i )
    {
        check(rocketnet_server_transport_stats_worker_step_for_test(transport));
        world.time += 0.01;
        rocketnet_server_update(world.server, world.time);
    }
    uint64_t resumed_submitted = 0, resumed_published = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &resumed_submitted, NULL, &resumed_published, NULL));
    check(resumed_submitted >= held_submitted + 12);
    check(resumed_published >= published + 2);
    check(fabs(rn_server_client_stats_of(world.server, world.slots[0]).rtt_ms - 11.0) < 0.001);
    check(fabs(rn_server_client_stats_of(world.server, world.slots[1]).rtt_ms - 29.0) < 0.001);
    stats_world_destroy(&world);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_late_batch_keeps_modulo_fairness);

static void test_server_stats_worker_reused_slot_rejects_old_publication()
{
    stats_env_pin_t pin;
    stats_world_t world;
    stats_world_create(&world, 1, 2);
    stats_connect(&world, 0, 0xAC21);
    rocketnet_server_transport_t * transport = stats_transport(&world);
    rocketnet_ack_endpoint_t * old_endpoint = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[0]);
    check(old_endpoint);
    for ( int n = 0; n < 4; ++n ) stats_step_and_update(&world);
    stats_add_sample(old_endpoint, world.time, 71.0);
    // Make room for a sealed delta that definitely names A, then hold that bank.
    check(rocketnet_server_transport_stats_worker_step_for_test(transport));
    world.time += 0.01;
    rocketnet_server_update(world.server, world.time);
    rocketnet_server_disconnect_client(world.server, world.slots[0]);
    for ( int n = 0; n < 200 && rocketnet_server_client_connected(world.server, world.slots[0]); ++n )
        stats_pump(&world);
    check(!rocketnet_server_client_connected(world.server, world.slots[0]));
    stats_connect(&world, 1, 0xAC22);
    check(world.slots[1] == world.slots[0]);
    rocketnet_ack_endpoint_t * new_endpoint = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[1]);
    check(new_endpoint);
    stats_add_sample(new_endpoint, world.time, 13.0);
    rocketnet_ack_endpoint_t * expected = stats_reference(new_endpoint);
    // A READY bank names the old connection. It must not apply to B even
    // when it finishes after the slot is reused.
    for ( int n = 0; n < 2; ++n )
    {
        check(rocketnet_server_transport_stats_worker_step_for_test(transport));
        world.time += 0.01;
        rocketnet_server_update(world.server, world.time);
        check(rn_server_client_stats_of(world.server, world.slots[1]).rtt_ms != 71.0);
    }
    for ( int n = 0; n < 6; ++n )
    {
        check(rocketnet_server_transport_stats_worker_step_for_test(transport));
        world.time += 0.01;
        rocketnet_server_update(world.server, world.time);
    }
    check(fabs(rn_server_client_stats_of(world.server, world.slots[1]).rtt_ms - 13.0) < 0.001);
    // Several exchanges revisit both physical banks. Old-generation RTTs
    // could leave the minimum unchanged while corrupting jitter or bandwidth.
    stats_check_six(rn_server_client_stats_of(world.server, world.slots[1]), *expected);
    delete expected;
    stats_world_destroy(&world);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_reused_slot_rejects_old_publication);

static void test_server_stats_worker_coalesces_wrapped_history_and_late_ack()
{
    stats_env_pin_t pin;
    stats_world_t world;
    stats_world_create(&world, 1, 1);
    stats_connect(&world, 0, 0xAC41);
    rocketnet_server_transport_t * transport = stats_transport(&world);
    rocketnet_ack_endpoint_t * endpoint = rocketnet_server_transport_stats_endpoint_for_test(transport, world.slots[0]);
    check(endpoint && endpoint->stats_delta);
    for ( int n = 0; n < 4; ++n ) stats_step_and_update(&world);

    // Hold the sealed input while the producer laps each history ring twice.
    // Repeated writes to one index must retain the newest sequence and ACK bit.
    const uint64_t sent_base = endpoint->highest_sequence + ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    for ( int i = 0; i < 2 * ROCKETNET_SENT_PACKET_BUFFER_SIZE + 17; ++i )
    {
        const uint64_t sequence = sent_base + (uint64_t)i;
        const double sent_time = world.time + i * 0.00001;
        check(rocketnet_ack_endpoint_record_sent_classified(endpoint, sequence,
            100 + i % 23, 0, 0, 0, sent_time) != ROCKETNET_ACK_SENT_REFUSED_STALE);
        if ( i % 2 == 0 )
            check(stats_ack_one(endpoint, sequence, sent_time + 0.020 + (i % 3) * 0.001) == 1);
    }
    const uint64_t received_base = endpoint->highest_received_sequence + ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    for ( int i = 0; i < 2 * ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 17; ++i )
        rocketnet_ack_endpoint_record_received(endpoint, received_base + (uint64_t)i,
            90 + i % 17, world.time + i * 0.00002);

    uint64_t submitted = 0, completed = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &submitted, &completed, NULL, NULL));
    rocketnet_ack_stats_delta_t * active = endpoint->stats_delta;
    for ( int n = 0; n < 5; ++n )
    {
        world.time += 0.001;
        rocketnet_server_update(world.server, world.time);
        check(endpoint->stats_delta == active);
    }
    uint64_t held_submitted = 0, held_completed = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &held_submitted, &held_completed, NULL, NULL));
    check(held_submitted == submitted && held_completed == completed);

    // Swapping changes only ownership. Real ACK mutations after the swap
    // belong to the newly active bank; the sealed input remains immutable.
    stats_step_and_update(&world);
    check(endpoint->stats_delta != active);
    rocketnet_ack_stats_delta_t * sealed_copy = new rocketnet_ack_stats_delta_t;
    memcpy(sealed_copy, active, sizeof(*sealed_copy)); // test oracle, never a production handoff
    rocketnet_ack_endpoint_t * before_ack = stats_reference(endpoint);
    check(before_ack->rtt > 0.0f && before_ack->jitter > 0.0f);
    check(before_ack->packet_loss > 0.0f && before_ack->bandwidth_sent_kbps > 0.0f);
    check(before_ack->bandwidth_received_kbps > 0.0f && before_ack->bandwidth_acked_kbps > 0.0f);

    const uint64_t oldest = endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;
    uint64_t late_sequence = oldest;
    while ( late_sequence < oldest + ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 &&
            endpoint->sent[late_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked )
        ++late_sequence;
    check(late_sequence < oldest + ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2);
    const double late_time = endpoint->sent[late_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE].send_time + 0.011;
    check(stats_ack_one(endpoint, late_sequence, late_time) == 1);
    check(stats_ack_one(endpoint, late_sequence, late_time + 9.0) == 0);
    // The duplicate must neither resample RTT nor re-ACK another record.
    check(fabsf(endpoint->rtt_history[late_sequence % ROCKETNET_RTT_HISTORY_SIZE] - 11.0f) < 0.001f);
    check(memcmp(active, sealed_copy, sizeof(*active)) == 0);
    rocketnet_ack_endpoint_t * after_ack = stats_reference(endpoint);
    check(after_ack->packet_loss < before_ack->packet_loss);
    check(after_ack->rtt < before_ack->rtt);

    check(rocketnet_server_transport_stats_worker_step_for_test(transport));
    world.time += 0.001;
    rocketnet_server_update(world.server, world.time);
    stats_check_six(rn_server_client_stats_of(world.server, world.slots[0]), *before_ack);
    check(rocketnet_server_transport_stats_worker_step_for_test(transport));
    world.time += 0.001;
    rocketnet_server_update(world.server, world.time);
    stats_check_six(rn_server_client_stats_of(world.server, world.slots[0]), *after_ack);
    delete after_ack;
    delete before_ack;
    delete sealed_copy;
    stats_world_destroy(&world);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_coalesces_wrapped_history_and_late_ack);

static void test_server_stats_worker_stop_discards_ready_batch()
{
    stats_env_pin_t pin;
    stats_world_t world;
    stats_world_create(&world, 1, 1);
    stats_connect(&world, 0, 0xAC31);
    rocketnet_server_transport_t * transport = stats_transport(&world);
    uint64_t submitted = 0, completed = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, &submitted, &completed, NULL, NULL));
    check(submitted > completed);
    rocketnet_server_stop(world.server);
    check(rocketnet_server_stopped(world.server));
    check(!rocketnet_server_transport_stats_worker_step_for_test(transport));
    uint64_t after = 0;
    check(rocketnet_server_transport_stats_worker_status_for_test(transport, NULL, &after, NULL, NULL));
    check(after == completed);
    for ( int i = 0; i < world.count; ++i )
        rn_test_client_stop_and_destroy(world.clients[i]);
    rocketnet_server_destroy(world.server);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_stop_discards_ready_batch);

struct stats_allocator_t
{
    int live;
    int calls;
    int fail_at;
};

static void * stats_allocate( void * context, size_t bytes )
{
    stats_allocator_t * state = (stats_allocator_t*)context;
    if ( ++state->calls == state->fail_at ) return NULL;
    void * base = malloc(bytes + 128);
    if ( !base ) return NULL;
    uintptr_t address = (((uintptr_t)base + sizeof(void*) + 63) & ~(uintptr_t)63) + 16;
    memcpy((void*)(address - sizeof(void*)), &base, sizeof(base));
    ++state->live;
    return (void*)address;
}

static void stats_release( void * context, void * pointer )
{
    stats_allocator_t * state = (stats_allocator_t*)context;
    void * base = NULL;
    memcpy(&base, (uint8_t*)pointer - sizeof(void*), sizeof(base));
    --state->live;
    free(base);
}

static void stats_transport_config( rocketnet_server_transport_config_t * config )
{
    rocketnet_server_transport_default_config(config);
    memset(config->backend_public_key, 0x42, 32);
    memset(config->server_public_key, 0x43, 32);
    memset(config->server_secret_key, 0x44, 32);
    config->protocol_id = STATS_PROTOCOL;
    config->max_clients = 1;
    snprintf(config->transport, sizeof(config->transport), "%s", "loopback");
}

static void test_server_stats_worker_start_failure_cleans_allocations()
{
    stats_env_pin_t pin;
    // The runner owns initial init; install a counted allocator for this case.
    check(rocketnet_term_ex() == ROCKETNET_STATUS_OK);
    stats_allocator_t state = {0, 0, 0};
    check(rocketnet_allocator_ex(stats_allocate, stats_release) == ROCKETNET_STATUS_OK);
    int status = -1;
    check(rocketnet_init_ex(&state, stats_allocate, stats_release, &status));
    check(status == ROCKETNET_STATUS_OK);
    rocketnet_server_transport_config_t config;
    stats_transport_config(&config);

    // The injected refusal occurs before native wake/thread creation, but
    // after worker, two banks, cold slots and spare pointers are allocated.
    rocketnet_server_transport_fail_stats_worker_thread_for_test();
    status = -1;
    check(rocketnet_server_transport_create_ex(&state, "127.0.0.1:0", &config, 1000.0, &status) == NULL);
    check(status == ROCKETNET_STATUS_THREAD_CREATE_FAILED);
    check(state.live == 0);

    // A successful manual create establishes the allocation count. The final
    // five allocations belong to worker, two banks, cold slots and spare pointers.
    rocketnet_server_transport_set_stats_worker_manual_for_test(true);
    const int start_calls = state.calls;
    status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex(
        &state, "127.0.0.1:0", &config, 1000.0, &status);
    check(server && status == ROCKETNET_STATUS_OK);
    const int create_calls = state.calls - start_calls;
    check(create_calls >= 5);
    rn_test_server_transport_stop_and_destroy(server);
    check(state.live == 0);

    for ( int last = 4; last >= 0; --last )
    {
        state.fail_at = state.calls + create_calls - last;
        rocketnet_server_transport_set_stats_worker_manual_for_test(true);
        status = -1;
        check(rocketnet_server_transport_create_ex(&state, "127.0.0.1:0", &config, 1000.0, &status) == NULL);
        check(status == ROCKETNET_STATUS_OUT_OF_MEMORY);
        check(state.live == 0);
    }
    state.fail_at = 0;
    rocketnet_server_transport_set_stats_worker_manual_for_test(true);
    status = -1;
    server = rocketnet_server_transport_create_ex(&state, "127.0.0.1:0", &config, 1000.0, &status);
    check(server && status == ROCKETNET_STATUS_OK);
    rn_test_server_transport_stop_and_destroy(server);
    check(state.live == 0);
    check(rocketnet_term_ex() == ROCKETNET_STATUS_OK);
    check(state.live == 0);
    check(rocketnet_allocator_ex(rocketnet_default_malloc_function, rocketnet_default_free_function) == ROCKETNET_STATUS_OK);
    check(pin.restore());
}

REGISTER_UNIT_TEST(test_server_stats_worker_start_failure_cleans_allocations);
