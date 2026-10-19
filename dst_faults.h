/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Deterministic DST fault schedules from docs/DST.md section 3. Every
    injector owns one named substream. Disabled kinds make no draws.
*/

#ifndef ROCKETNET_DST_FAULTS_H
#define ROCKETNET_DST_FAULTS_H

#define ROCKETNET_DST_FAULT_PEERS 17
#define ROCKETNET_DST_CLIENT_CRASHES 2

struct rocketnet_dst_crash_event_t
{
    int peer;
    uint64_t crash_step;
    uint64_t restart_step;
    int rebind;
    int enabled;
    int crashed;
    int restarted;
};

struct rocketnet_dst_fault_state_t
{
    struct rocketnet_dst_substream_t reorder;
    struct rocketnet_dst_substream_t delay;
    struct rocketnet_dst_substream_t burst;
    struct rocketnet_dst_substream_t skew;
    struct rocketnet_dst_substream_t crash_client;
    struct rocketnet_dst_substream_t crash_server;
    double clock_offset[ROCKETNET_DST_FAULT_PEERS];
    double clock_rate[ROCKETNET_DST_FAULT_PEERS];
    uint64_t fault_steps;
    int reorder_percent;
    int delay_enabled;
    uint64_t delay_start;
    uint64_t delay_end;
    double delay_seconds;
    int burst_enabled[2];
    uint64_t burst_start[2];
    uint64_t burst_end[2];
    struct rocketnet_dst_crash_event_t client[ROCKETNET_DST_CLIENT_CRASHES];
    struct rocketnet_dst_crash_event_t server;
};

static inline uint64_t rocketnet_dst_fault_duration( struct rocketnet_dst_substream_t * stream,
                                                     uint64_t lo, uint64_t hi, uint64_t fault_steps )
{
    uint64_t cap = fault_steps / 5;
    if ( cap < 1 )
        cap = 1;
    if ( hi > cap )
        hi = cap;
    if ( lo > hi )
        lo = hi;
    return rocketnet_dst_substream_uniform( stream, lo, hi );
}

static inline uint64_t rocketnet_dst_fault_start( struct rocketnet_dst_substream_t * stream,
                                                  uint64_t fault_steps, uint64_t duration,
                                                  uint64_t floor_step )
{
    if ( fault_steps <= duration + floor_step )
        return floor_step < fault_steps ? floor_step : 0;
    return rocketnet_dst_substream_uniform( stream, floor_step, fault_steps - duration - 1 );
}

static inline void rocketnet_dst_faults_init( struct rocketnet_dst_fault_state_t * state,
                                              uint64_t seed, uint32_t faults,
                                              uint64_t fault_steps, int clients )
{
    memset( state, 0, sizeof(*state) );
    state->fault_steps = fault_steps;
    rocketnet_dst_substream_init( &state->reorder, seed, "fault.reorder" );
    rocketnet_dst_substream_init( &state->delay, seed, "fault.delay" );
    rocketnet_dst_substream_init( &state->burst, seed, "fault.burst" );
    rocketnet_dst_substream_init( &state->skew, seed, "fault.skew" );
    rocketnet_dst_substream_init( &state->crash_client, seed, "fault.crash.client" );
    rocketnet_dst_substream_init( &state->crash_server, seed, "fault.crash.server" );

    for ( int p = 0; p < ROCKETNET_DST_FAULT_PEERS; p++ )
    {
        state->clock_offset[p] = 0.0;
        state->clock_rate[p] = 1.0;
    }

    if ( faults & ROCKETNET_DST_FAULT_LOSS )
    {
        for ( int i = 0; i < 2; i++ )
        {
            state->burst_enabled[i] = rocketnet_dst_substream_real( &state->burst ) < 0.5;
            if ( state->burst_enabled[i] )
            {
                const uint64_t duration = rocketnet_dst_fault_duration( &state->burst, 20, 200, fault_steps );
                state->burst_start[i] = rocketnet_dst_fault_start( &state->burst, fault_steps, duration, 0 );
                state->burst_end[i] = state->burst_start[i] + duration;
            }
        }
    }

    if ( faults & ROCKETNET_DST_FAULT_REORDER )
    {
        static const int rates[] = { 0, 1, 5 };
        state->reorder_percent = rates[rocketnet_dst_substream_uniform( &state->reorder, 0, 2 )];
    }

    if ( faults & ROCKETNET_DST_FAULT_DELAY )
    {
        state->delay_enabled = rocketnet_dst_substream_real( &state->delay ) < 0.3;
        if ( state->delay_enabled )
        {
            const uint64_t duration = rocketnet_dst_fault_duration( &state->delay, 50, 300, fault_steps );
            state->delay_start = rocketnet_dst_fault_start( &state->delay, fault_steps, duration, 0 );
            state->delay_end = state->delay_start + duration;
            state->delay_seconds = (double) rocketnet_dst_substream_uniform( &state->delay, 500, 1500 ) / 1000.0;
        }
    }

    if ( faults & ROCKETNET_DST_FAULT_SKEW )
    {
        for ( int p = 0; p <= clients; p++ )
        {
            state->clock_offset[p] = -0.25 + rocketnet_dst_substream_real( &state->skew ) * 0.5;
            state->clock_rate[p] = 0.995 + rocketnet_dst_substream_real( &state->skew ) * 0.010;
        }
    }

    if ( faults & ROCKETNET_DST_FAULT_CRASH_CLIENT )
    {
        const int roll = (int) rocketnet_dst_substream_uniform( &state->crash_client, 0, 99 );
        const int count = roll < 50 ? 0 : ( roll < 85 ? 1 : 2 );
        for ( int i = 0; i < count; i++ )
        {
            struct rocketnet_dst_crash_event_t * event = &state->client[i];
            event->enabled = 1;
            event->peer = (int) rocketnet_dst_substream_uniform( &state->crash_client, 0, 15 ) + 1;
            const uint64_t duration = rocketnet_dst_fault_duration( &state->crash_client, 10, 300, fault_steps );
            event->crash_step = rocketnet_dst_fault_start( &state->crash_client, fault_steps, duration, 0 );
            event->restart_step = event->crash_step + duration;
            event->rebind = rocketnet_dst_substream_real( &state->crash_client ) >= 0.5;
        }
    }

    if ( faults & ROCKETNET_DST_FAULT_CRASH_SERVER )
    {
        struct rocketnet_dst_crash_event_t * event = &state->server;
        event->enabled = rocketnet_dst_substream_real( &state->crash_server ) < 0.2;
        if ( event->enabled )
        {
            event->peer = 0;
            const uint64_t duration = rocketnet_dst_fault_duration( &state->crash_server, 10, 200, fault_steps );
            event->crash_step = rocketnet_dst_fault_start( &state->crash_server, fault_steps, duration, fault_steps / 5 );
            event->restart_step = event->crash_step + duration;
        }
    }
}

// RNSimFaults ClockMonotone and SkewBounded.
static inline double rocketnet_dst_faults_clock( const struct rocketnet_dst_fault_state_t * state,
                                                 int peer, double true_time )
{
    return 1000.0 + state->clock_offset[peer] + state->clock_rate[peer] * ( true_time - 1000.0 );
}

// RNSimFaults Lose: delay/reorder alter only a packet's simulator send time.
static inline double rocketnet_dst_faults_send_time( struct rocketnet_dst_fault_state_t * state,
                                                     uint64_t step, double true_time, int * reordered )
{
    double result = true_time;
    *reordered = 0;
    if ( state->delay_enabled && step >= state->delay_start && step < state->delay_end )
        result += state->delay_seconds;
    if ( state->reorder_percent > 0 &&
         rocketnet_dst_substream_uniform( &state->reorder, 0, 99 ) < (uint64_t) state->reorder_percent )
    {
        result += (double) rocketnet_dst_substream_uniform( &state->reorder, 10, 100 ) / 1000.0;
        *reordered = 1;
    }
    return result;
}

#endif
