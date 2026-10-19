/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Maps the CBMC harness vocabulary onto ESBMC. The fragment readers
    include sdk/rocketnet_fragment.cpp, which only a full C++ front end
    parses, so these harnesses run under ESBMC, built as C++17. proof.mk
    passes this header with --include-file, ahead of the harness, so the
    harness keeps the __CPROVER_ names the other proofs use. ESBMC treats
    every undefined function whose name starts with nondet_ as returning a
    nondeterministic value, which is what the harness declarations ask for.
*/

#ifndef ROCKETNET_PROOF_ESBMC_CPROVER_H
#define ROCKETNET_PROOF_ESBMC_CPROVER_H

#define __CPROVER__ 1

#define __CPROVER_assert( condition, message ) __ESBMC_assert( ( condition ), ( message ) )
#define __CPROVER_assume( condition ) __ESBMC_assume( ( condition ) )
#define __CPROVER_same_object( a, b ) __ESBMC_same_object( ( a ), ( b ) )

#endif // #ifndef ROCKETNET_PROOF_ESBMC_CPROVER_H
