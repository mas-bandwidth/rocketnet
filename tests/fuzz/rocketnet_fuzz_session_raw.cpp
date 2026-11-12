/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// rocketnet_fuzz_session_raw: the session harness (rocketnet_fuzz_session.cpp,
// same input format and oracles) with the client created off its I/O-thread
// decrypt copy, so the client protocol's own replay check and open refusal
// (the RAW verdict in sdk/rocketnet_client_protocol.cpp) see every session
// datagram (tests/fuzz/rocketnet_fuzz_pair.h, ROCKETNET_FUZZ_CLIENT_RAW).
// Its corpus is tests/fuzz/corpus_session_raw.

#define ROCKETNET_FUZZ_CLIENT_RAW 1
#include "rocketnet_fuzz_session.cpp"
