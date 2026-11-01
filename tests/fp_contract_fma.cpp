/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The floating-point floor canary built with FMA code generation (-mfma on
// x86-64 GCC and Clang, /arch:AVX2 on x64 MSVC and clang-cl; aarch64 has FMA
// in its baseline). Without the floor, a*b+c compiles to one fused
// multiply-add and the canary fails. It runs only on a CPU with FMA.

#include "fp_contract_canary.h"

int main()
{
    if ( test_fp_contract_off() )
        return 1;
    printf( "test_fp_contract_off: two roundings with FMA code generation\n" );
    return 0;
}
