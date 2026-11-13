/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_msgx_send_runs.h"

// The sort's phase test, out of line: the trace check's plant
// (ROCKETNET_MSGX_SEND_RUNS_PLANT) must be library code the plant binary
// compiles in place of the library's (proofs/tla/README-TraceCheck.md): only
// tests/tla_trace_msgx_send_runs.cpp in the plant build defines it, and its
// traces must fail TraceConsistent. The rest of the send path stays inline
// in its header.

// proofs/tla/RNMsgxSendRuns.tla Sort (RangeEnd): two packets of the bank are
// in one fill phase, so one counting sort orders them
bool rocketnet_msgx_send_runs_same_phase( uint32_t phase, uint32_t other )
{
#if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    // the trace check's plant: the sort ignores the fill phase and orders the
    // whole bank by slot alone, as before the phase (RNMsgxSendRuns
    // PhaseSort FALSE)
    (void) phase;
    (void) other;
    return true;
#else  // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
    return phase == other;
#endif // #if defined( ROCKETNET_MSGX_SEND_RUNS_PLANT )
}
