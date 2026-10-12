/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_H
#define ROCKETNET_SERVER_H

#include <stdint.h>
#include "rocketnet_address.h"

// The failed-signature-verify budget. The cookie gate proves a source can
// receive at the address it claims, but one valid cookie would otherwise buy
// unlimited Ed25519 verifies from that address: nothing between the gate and
// the verify bounds failed verifies. This budget, checked before the verify
// runs, closes that.
//
// It is a windowed global cap on failed verifies plus a per-source table that
// remembers which sources have already failed this window. Once the global cap
// is spent, a request is refused before the verify only if its source has
// already failed this window; a source with no failures -- a first-time or a
// previously-good source -- is still verified, so one source's failures never
// refuse a validly signed request from another source. The whole budget decays
// when the window rolls over, so a source that exhausted it is not refused
// forever, and because the table is a fixed part of the server struct (not
// file-scope), two servers in one process each have their own budget.

#define ROCKETNET_SIGNATURE_VERIFY_FAILURE_BUDGET        90
#define ROCKETNET_SIGNATURE_VERIFY_FAILURE_WINDOW_SECONDS 10
#define ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS          64

struct rocketnet_signature_verify_source_t
{
    struct rocketnet_address_t address;
    uint32_t fail_count;        // failed verifies from this source this window; 0 means the slot is empty
};

struct rocketnet_signature_verify_budget_t
{
    uint64_t window_start;      // unix seconds the current window began
    uint32_t failure_count;     // failed verifies this window, across all sources
    struct rocketnet_signature_verify_source_t sources[ROCKETNET_SIGNATURE_VERIFY_TABLE_SLOTS];
};

// Returns nonzero when this source may run one more signature verify. Zero
// means the source has already failed this window and the global budget is
// spent: refuse before the verify.
int rocketnet_signature_verify_budget_allow( struct rocketnet_signature_verify_budget_t * budget,
                                             const struct rocketnet_address_t * source,
                                             uint64_t current_timestamp );

// Records a failed verify from this source: the source's own count and the
// global count both rise. Call only after a verify actually failed.
void rocketnet_signature_verify_budget_record_failure( struct rocketnet_signature_verify_budget_t * budget,
                                                       const struct rocketnet_address_t * source,
                                                       uint64_t current_timestamp );

#endif // #ifndef ROCKETNET_SERVER_H
