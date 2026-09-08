/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONFIG_H
#define ROCKETNET_CONFIG_H

#include "rocketnet.h"

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

// the mechanical env var derivation: "driver" -> "ROCKETNET_DRIVER".
// returns false when the buffer cannot hold the derived name.

bool rocketnet_config_env_var_name( const char * field_name, char * buffer, int buffer_size );

// the strict integer parse, exposed for the deprecated env alias reads
// (the pre-#106 spellings kept working under the ROCKETNET_PUMP pattern):
// whole-string, digits only after an optional leading minus, or false.

bool rocketnet_config_parse_int_value( const char * value, int * out );

// overlay the environment onto a config struct, field by field from the
// descriptors: unset vars leave the struct value; set vars parse under the
// field's kind, log the override loudly with the displaced value (secret
// fields redacted), and assign. an env var set to the empty string is
// ignored as unset, with a log line. returns false -- loudly -- on any
// value that does not parse. transport_name flavors the log lines
// ("client", "server").

bool rocketnet_config_apply_env( const char * transport_name, void * config, const struct rocketnet_config_field_t * fields, int num_fields );

// validate a config struct against the descriptors: numeric ranges (zero
// admitted where the field declares ZERO_MEANS_DEFAULT), string
// termination. refuses loudly, never clamps. driver list content is
// validated by the ladder parse below, at create.

bool rocketnet_config_validate( const char * transport_name, const void * config, const struct rocketnet_config_field_t * fields, int num_fields );

// ----------------------------------------------------------------
//                        the driver ladder
// ----------------------------------------------------------------

// The runtime driver ladder (#106): the driver config value is always an
// ordered candidate list, tried left to right at transport create, first
// grant wins, and a list exhausted by refusal fails loudly. "auto" (or the
// empty string) expands to the platform's recommended order for the
// endpoint. A name in the known transport set but not available to this
// endpoint on this platform is ignored with one soft log line (fleet
// configs stay portable), and a list whose EVERY name is ignored falls to
// the endpoint's own auto order, loudly, rather than bricking an endpoint
// the value was not addressing; a name in no transport set anywhere is a
// typo and fails loudly at parse listing the valid names (spelling errors
// stay catchable). An applicable one-element list is a pin: that driver or
// loud failure.

#define ROCKETNET_DRIVER_PORTABLE       0
#define ROCKETNET_DRIVER_GSO            1
#define ROCKETNET_DRIVER_XDP            2
#define ROCKETNET_DRIVER_MSGX           3
#define ROCKETNET_DRIVER_IOCP           4
#define ROCKETNET_NUM_DRIVERS           5

const char * rocketnet_driver_name( int driver );

// #109: does this driver's create discover the port the kernel granted and
// hand it back? a port-zero public address asks the kernel for an
// ephemeral port, so it is only meaningful to a driver that answers yes --
// the transport's ladder refuses a port-zero address on any rung that
// answers no, before that rung's create runs, and falls to one that
// answers yes. an unclassified driver reads false, which fails safe.

bool rocketnet_driver_discovers_bound_port( int driver );

#define ROCKETNET_MAX_DRIVER_CANDIDATES 8

struct rocketnet_driver_ladder_t
{
    int num_candidates;                                     // available candidates, in list order
    int candidates[ROCKETNET_MAX_DRIVER_CANDIDATES];
    int num_skipped;                                        // known names ignored for this endpoint on this platform
    int skipped[ROCKETNET_MAX_DRIVER_CANDIDATES];
};

// parse a candidate list against an explicit availability order --
// explicit so tests can exercise any platform's table anywhere. value
// NULL, empty, or "auto" expands to the availability order itself (which
// IS the recommended order) -- as does, loudly, a list whose every name
// was skipped for this endpoint. whitespace around names is tolerated;
// names match exactly otherwise. returns false, loudly, on a name in no
// transport set, an empty list element, "auto" inside a longer list, or a
// list longer than ROCKETNET_MAX_DRIVER_CANDIDATES.

bool rocketnet_driver_ladder_parse( const char * value, const int * available, int num_available, struct rocketnet_driver_ladder_t * ladder );

// the per-platform, per-endpoint availability tables, in recommended
// order -- the single table "auto" expands from:
//
//               server                  client
//   linux       xdp, gso, portable      portable
//   macOS       msgx, portable          msgx, portable
//   iOS         portable                portable
//   windows     portable                portable
//   BSDs        portable                portable
//
// (the staged linux drivers are server-side machinery; the client is the
// platform socket everywhere, with the darwin batch pump where granted)

const int * rocketnet_server_driver_availability( int * num_available );

const int * rocketnet_client_driver_availability( int * num_available );

#endif // #ifndef ROCKETNET_CONFIG_H
