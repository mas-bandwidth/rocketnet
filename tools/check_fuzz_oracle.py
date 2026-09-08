#!/usr/bin/env python3
"""Require the exact-field fuzz oracle to reject memory-safe base64 truncation."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1] if len(sys.argv) > 1 else 'build-fuzz').resolve()
cache = (BUILD / 'CMakeCache.txt').read_text()
compiler = next(line.split('=', 1)[1] for line in cache.splitlines()
                if line.startswith('CMAKE_CXX_COMPILER:'))
flags = [compiler, '-g', '-O1', '-std=c++17',
         '-fsanitize=fuzzer-no-link,address,undefined',
         '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I' + str(ROOT / 'sdk')]
libs = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'libsodium'], text=True))
libs += ['-pthread']
if sys.platform == 'darwin':
    libs += ['-framework', 'CoreFoundation', '-framework', 'SystemConfiguration']
else:
    libs += ['-lbpf', '-lxdp']
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')

with tempfile.TemporaryDirectory(prefix='fuzz-oracle-', dir=BUILD) as directory:
    work = Path(directory)
    replay = work / 'replay.cpp'
    replay.write_text('''#include <fstream>
#include <iterator>
#include <vector>
#include <stdint.h>
extern "C" int LLVMFuzzerInitialize(int*, char***);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t);
int main(int argc, char** argv) {
    LLVMFuzzerInitialize(&argc, &argv);
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        if (!file) return 2;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
        LLVMFuzzerTestOneInput(data.data(), data.size());
    }
}
''')
    sources = [str(ROOT / 'fuzz/rocketnet_fuzz.cpp'), str(replay)]
    baseline = work / 'baseline'
    subprocess.run(flags + sources + [str(BUILD / 'librocketnet.a')] + libs + ['-o', str(baseline)], check=True)
    subprocess.run([str(baseline), *map(str, sorted((ROOT / 'fuzz/corpus').iterdir()))], env=env, check=True)

    original = (ROOT / 'sdk/rocketnet_base64.cpp').read_text()
    guard = '    if ( output_length > output_size )\n        return 0;'
    if original.count(guard) != 1:
        raise SystemExit('decode capacity guard changed; review the negative control')
    mutant = work / 'base64-truncate.cpp'
    mutant.write_text(original.replace(guard, '''    if ( output_length > output_size )
    {
        uint8_t wide[4097];
        if ( output_length > sizeof(wide) ) return 0;
        const int length = rocketnet_base64_decode_data( input, wide, sizeof(wide) );
        if ( length <= 0 ) return 0;
        memcpy( output, wide, output_size );
        return (int) output_size;
    }'''))
    control = work / 'control'
    # Supplying the complete replacement object before the static library
    # prevents its original base64 object from being extracted by the linker.
    subprocess.run(flags + sources + [str(mutant), str(BUILD / 'librocketnet.a')] + libs + ['-o', str(control)], check=True)
    result = subprocess.run([str(control), str(ROOT / 'fuzz/corpus/base64_over_key_field')],
                            env=env, capture_output=True, text=True)
    (BUILD / 'fuzz-oracle-control.log').write_text(result.stdout + result.stderr)
    if result.returncode == 0 or 'fuzz oracle failed: base64 overlong field must refuse' not in result.stderr:
        raise SystemExit('memory-safe truncation did not fail the exact-field oracle: ' + result.stderr)
    if 'ERROR: AddressSanitizer' in result.stderr or 'runtime error:' in result.stderr:
        raise SystemExit('negative control failed memory safety rather than the property oracle')
print('test_fuzz_exact_field_oracle')
