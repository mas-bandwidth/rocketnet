/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CONFIG_H
#define ROCKETNET_SDK_ROCKETNET_CONFIG_H

#include "rocketnet_internal.h"
#include "rocketnet_driver.h"
#include "rocketnet_test_seams.h"

// The config engine (#106): the machinery behind the per-transport config
// tables declared in rocketnet.h. Each table declares every field once;
// this engine derives the rest at runtime from field descriptors expanded
// from the same table -- the env var name (mechanically: ROCKETNET_ plus
// the field name uppercased), the parse, the validation, and the walkable
// metadata the completeness test uses. Precedence is DEFAULT < STRUCT <
// ENV; every env override logs loudly with the variable and the displaced
// value; validation refuses loudly and never clamps. Env reads go through
// rocketnet_platform_getenv.

#define ROCKETNET_CONFIG_KIND_INT       0
#define ROCKETNET_CONFIG_KIND_UINT64    1
#define ROCKETNET_CONFIG_KIND_BYTES     2       // env encoding: base64, exactly the field's length
#define ROCKETNET_CONFIG_KIND_STRING    3

struct rocketnet_config_field_t
{
    const char * name;                          // the field name, exactly as declared in the table
    int kind;                                   // ROCKETNET_CONFIG_KIND_*
    int offset;                                 // byte offset of the field inside the config struct
    int size;                                   // BYTES: exact length; STRING: capacity including the terminator; numerics: sizeof
    int64_t min_value;                          // numeric fields only
    int64_t max_value;                          // numeric fields only
    int flags;                                  // ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT, ROCKETNET_CONFIG_SECRET
};

// the two tables' descriptors, one entry per declared field, in
// declaration order -- what the round-trip test walks

const struct rocketnet_config_field_t * rocketnet_client_transport_config_fields( int * num_fields );

const struct rocketnet_config_field_t * rocketnet_server_transport_config_fields( int * num_fields );

// the mechanical environment-name derivation used by source utilities and
// tests. Returns false when the buffer cannot hold the derived name.
// only a unit case calls it.

bool rocketnet_config_env_var_name( const char * field_name, char * buffer, int buffer_size );

// the public configs' send budget fields (sdk/rocketnet_api.cpp), one int
// each: send_kbps for the client, per_client_send_kbps for the server. with
// the transport tables and the message table they are every env-backed field
// of rocketnet_client_config_t and rocketnet_server_config_t.

#if ROCKETNET_DEV
const struct rocketnet_config_field_t * rocketnet_client_budget_config_fields( int * num_fields );

const struct rocketnet_config_field_t * rocketnet_server_budget_config_fields( int * num_fields );
#endif // #if ROCKETNET_DEV

// the split create makes (sdk/rocketnet_api.cpp): a public config, NULL for
// the defaults, into the transport config, the message config and the send
// budget the layers take. read_env applies every environment override, the
// transport table's, the message fields', the budget's and the development
// simulator's, as create does: the result is the endpoint's one resolved
// config, which start and connect use without reading the environment again.
// false, with a log line, for a string too long for its layer field, a
// variable that does not parse or a budget out of range.

struct rocketnet_client_transport_config_t;
struct rocketnet_server_transport_config_t;
struct rocketnet_message_config_t;
struct rocketnet_client_config_t;
struct rocketnet_server_config_t;

#if ROCKETNET_DEV
bool rocketnet_client_config_resolve( const struct rocketnet_client_config_t * config, bool read_env,
                                      struct rocketnet_client_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps );

bool rocketnet_server_config_resolve( const struct rocketnet_server_config_t * config, bool read_env,
                                      struct rocketnet_server_transport_config_t * transport, struct rocketnet_message_config_t * message, uint32_t * kbps );
#endif // #if ROCKETNET_DEV

// the per-endpoint derivation (ruling 11): "server" + "max_fragmented_messages" ->
// "ROCKETNET_SERVER_MAX_FRAGMENTED_MESSAGES" and "client" + "max_fragmented_messages" ->
// "ROCKETNET_CLIENT_MAX_FRAGMENTED_MESSAGES". each transport table's field is read
// under its own endpoint's name, so a server and a client in one process
// never share a variable. returns false when the buffer cannot hold it.

bool rocketnet_config_env_var_name_for( const char * transport_name, const char * field_name, char * buffer, int buffer_size );

// the strict integer parse: whole-string, digits only after an optional
// leading minus, or false.

bool rocketnet_config_parse_int_value( const char * value, int * out );

// strictly apply one per-endpoint uint64 env overlay -- the operator memory
// budget reads ROCKETNET_<TRANSPORT>_<FIELD> with the table parser, so an
// unset or empty variable leaves *out unchanged, a non-number refuses loudly
// exactly as the table overlays refuse, and a parseable number assigns.
// returns false on a value that does not parse.

bool rocketnet_config_apply_env_uint64( const char * transport_name, const char * field_name, uint64_t * out );

// overlay the environment onto a config struct, field by field from the
// descriptors: unset vars leave the struct value; set vars parse under the
// field's kind, log the override loudly with the displaced value (secret
// fields redacted), and assign. each field is read under its PER-ENDPOINT
// name, ROCKETNET_<TRANSPORT>_<FIELD> (ruling 11); an old process-wide
// ROCKETNET_<FIELD> is not read and nothing is logged for it. an env var set
// to the empty string is ignored as unset, with a
// log line. returns false -- loudly -- on any value that does not parse.
// transport_name flavors the log lines and the derived names ("client",
// "server").

bool rocketnet_config_apply_env( const char * transport_name, void * config, const struct rocketnet_config_field_t * fields, int num_fields );

void rocketnet_client_transport_default_config_current( struct rocketnet_client_transport_config_t * config );
struct rocketnet_client_transport_t * rocketnet_client_transport_create_current( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time );

// the current-size creates with the section-2.1 code written to *status (NULL skips it)
struct rocketnet_client_transport_t * rocketnet_client_transport_create_current_ex( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, int * status );
struct rocketnet_server_transport_t * rocketnet_server_transport_create_current_ex( void * context, const char * address, const struct rocketnet_server_transport_config_t * config, int * status );
#if ROCKETNET_DEV
struct rocketnet_client_transport_t * rocketnet_client_transport_create_step_driven_current( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, const rocketnet_client_transport_io_operations_t * operations, void * transport_context );
#endif // #if ROCKETNET_DEV
struct rocketnet_server_transport_t * rocketnet_server_transport_create_current( void * context, const char * address, const struct rocketnet_server_transport_config_t * config );

// the creates behind public connect and start: config is the one the public
// create resolved (DEFAULT < STRUCT < ENV, rn_client_split and
// rn_server_split in sdk/rocketnet_api.cpp) and memory_budget_bytes the
// operator budget it read then, so neither reads the environment again
struct rocketnet_client_transport_t * rocketnet_client_transport_create_resolved( void * context, const uint8_t * connect_token, const struct rocketnet_client_transport_config_t * config, double time, uint64_t memory_budget_bytes, int * status );
struct rocketnet_server_transport_t * rocketnet_server_transport_create_resolved( void * context, const char * address, const struct rocketnet_server_transport_config_t * config, uint64_t memory_budget_bytes, int * status );
struct rocketnet_message_client_t * rocketnet_message_client_create_resolved( void * context, const uint8_t * connect_token,
                                                                              const struct rocketnet_client_transport_config_t * transport_config,
                                                                              const struct rocketnet_message_config_t * message_config,
                                                                              void * memory, uint64_t memory_bytes, uint32_t send_kbps, int * status, int reserve_unlimited,
                                                                              uint64_t memory_budget_bytes );
struct rocketnet_message_server_t * rocketnet_message_server_create_resolved( void * context, const char * address,
                                                                              const struct rocketnet_server_transport_config_t * transport_config,
                                                                              const struct rocketnet_message_config_t * message_config,
                                                                              void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps, int * status, int reserve_unlimited,
                                                                              uint64_t memory_budget_bytes );
void rocketnet_server_transport_default_config_current( struct rocketnet_server_transport_config_t * config );

// validate a config struct against the descriptors: numeric ranges (zero
// admitted where the field declares ZERO_MEANS_DEFAULT), string
// termination. refuses loudly, never clamps. transport list content is
// validated by the ladder parse below, at create.

bool rocketnet_config_validate( const char * transport_name, const void * config, const struct rocketnet_config_field_t * fields, int num_fields );

// The public link simulator fields (simulated_loss_percent,
// simulated_latency_ms, simulated_jitter_ms, simulated_duplicate_percent) are
// a development and test instrument. A production library refuses a nonzero
// value at create with CONFIG_OUT_OF_RANGE and one error line naming the
// field; the same walk is exposed here so a unit suite that links only the
// development library can pin the refusal. development is the caller's build
// posture, normally ROCKETNET_DEV.

bool rocketnet_config_validate_public_link_simulator( const char * transport_name, const void * config, const struct rocketnet_config_field_t * fields, int num_fields, bool development );

// Whether the public link simulator is honoured: true in a development
// library (ROCKETNET_DEV=1). A production library never answers true.

bool rocketnet_public_link_simulator_development( void );

#if ROCKETNET_DEV
void rocketnet_public_link_simulator_test_development( int enabled );
#endif // #if ROCKETNET_DEV

// The development values: the published test packet filter
// (ROCKETNET_TEST_FILTER_VALUE) and a connect token sealed to the published
// development server key. A production library (ROCKETNET_DEV=0) refuses them:
// the server and the client refuse the test filter at create with
// ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION, and the client refuses
// a development token at connect with
// ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION. True when this library
// refuses them: a production library always, a development library never,
// always in a production library.

bool rocketnet_development_values_refused( void );

// True (and one error line naming side) when the library refuses the
// development values and filter is the published test filter.

bool rocketnet_config_packet_filter_refused( const char * side, const uint8_t * filter );

// ----------------------------------------------------------------
//                        the transport ladder
// ----------------------------------------------------------------

// The runtime transport ladder (#106): the transport config value is always an
// ordered candidate list, tried left to right at transport create, first
// grant wins, and a list exhausted by refusal fails loudly. "auto" (or the
// empty string) expands to the platform's recommended order for the
// endpoint. A name in the known transport set but not available to this
// endpoint on this platform is ignored with one soft log line (fleet
// configs stay portable), and a list whose EVERY name is ignored falls to
// the endpoint's own auto order, loudly, rather than bricking an endpoint
// the value was not addressing; a name in no transport set anywhere is a
// typo and fails loudly at parse listing the valid names (spelling errors
// stay catchable). An applicable one-element list is a pin: that transport or
// loud failure.

const char * rocketnet_transport_name( int transport );

// #109: does this transport's create discover the port the kernel granted and
// hand it back? a port-zero public address asks the kernel for an
// ephemeral port, so it is only meaningful to a transport that answers yes --
// the transport's ladder refuses a port-zero address on any rung that
// answers no, before that rung's create runs, and falls to one that
// answers yes. an unclassified transport reads false, which fails safe.

bool rocketnet_transport_discovers_bound_port( int transport );

// room for the built-in registry and the transports an application adds
// through its own registry (tests/driver_extension adds one)
#define ROCKETNET_MAX_TRANSPORT_CANDIDATES 16

struct rocketnet_transport_ladder_t
{
    int num_candidates;                                     // available candidates, in list order
    int candidates[ROCKETNET_MAX_TRANSPORT_CANDIDATES];
    int num_skipped;                                        // known names ignored for this endpoint on this platform
    int skipped[ROCKETNET_MAX_TRANSPORT_CANDIDATES];
};

// the ladder must hold every transport the registry can put in an auto order
static_assert( ROCKETNET_NUM_TRANSPORTS <= ROCKETNET_MAX_TRANSPORT_CANDIDATES, "ROCKETNET_MAX_TRANSPORT_CANDIDATES is smaller than the transport registry" );

// parse a candidate list against an explicit availability order --
// explicit so tests can exercise any platform's table anywhere. value
// NULL, empty, or "auto" expands to the availability order itself (which
// IS the recommended order) -- as does, loudly, a list whose every name
// was skipped for this endpoint. whitespace around names is tolerated;
// names match exactly otherwise. returns false, loudly, on a name in no
// transport set, an empty list element, "auto" inside a longer list, or a
// list longer than ROCKETNET_MAX_TRANSPORT_CANDIDATES.

// server: the endpoint whose registry column decides pin-only (auto skips it)
bool rocketnet_transport_ladder_parse( const char * value, const int * available, int num_available, struct rocketnet_transport_ladder_t * ladder, bool server = true );

// the per-platform, per-endpoint availability tables, in recommended
// order -- the single table "auto" expands from:
//
//               production server          development server             client
//   linux       xdp, gso, io_uring, portable  xdp, gso, io_uring, portable   portable
//   macOS       msgx, portable                msgx, portable                 msgx, portable
//   windows     iocp, rio, portable           portable, iocp, rio            portable
//   iOS/BSDs    portable                      portable                       portable
//
// (the staged linux transports are server-side machinery; the client is the
// platform socket everywhere, with the darwin batch pump where granted)

const int * rocketnet_server_transport_io_availability( int * num_available );

const int * rocketnet_client_transport_io_availability( int * num_available );

#endif // ROCKETNET_SDK_ROCKETNET_CONFIG_H
