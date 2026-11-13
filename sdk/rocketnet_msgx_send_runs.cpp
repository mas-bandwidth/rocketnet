/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_msgx_send_runs.h"

// The run builder's generation test, out of line: the trace check's plant
// (ROCKETNET_MSGX_SEND_RUNS_PLANT) must be library code the plant binary
// compiles in place of the library's (proofs/tla/README-TraceCheck.md): only
// tests/tla_trace_msgx_send_runs.cpp in the plant build defines it, and its
// traces must fail TraceConsistent. The rest of the send path stays inline in
// its header.

// proofs/tla/RNMsgxSendRuns.tla Build (GenBreak): a run ends at a change of
// generation, so two packets of one slot are in one run only at one
// generation
bool rocketnet_msgx_send_runs_same_generation( uint32_t generation, uint32_t other )
{
#if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    // the trace check's plant: a run crosses a generation (RNMsgxSendRuns
    // GenBreak FALSE), as mutation 3
    (void) generation;
    (void) other;
    return true;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    return generation == other;
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
}
