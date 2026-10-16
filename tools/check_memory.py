#!/usr/bin/env python3
"""Compare pre-allocation plans with independent allocator observations."""
import json
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/transport_memory').resolve()
profiles = [(1, 1416, 1), (2, 1416, 1), (1000, 1416, 1), (1000, 1416, 16), (1000, 4096, 16)]
drivers = ['portable']
if sys.platform == 'darwin':
    drivers.append('msgx')
elif sys.platform == 'win32':
    drivers.append('iocp')
elif sys.platform.startswith('linux'):
    drivers.append('gso')


def run(*arguments):
    result = subprocess.run([str(binary), *map(str, arguments)],
                            capture_output=True, text=True, timeout=120)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    result.check_returncode()
    return [json.loads(line) for line in result.stdout.splitlines()]


for driver in drivers:
    for profile in profiles if driver == 'portable' else [profiles[0]]:
        plan, observed = run(*profile, driver, 1)
        assert plan['umem_bytes'] == 0
        assert observed['peak_allocator_bytes'] <= plan['planned_bytes']
        if plan['worker_threads'] == 0:
            assert observed['live_allocator_bytes'] == plan['planned_bytes']
        assert plan['os_and_application_bytes'] is None
# This legal field combination is deliberately too large for the default
# reassembly budget. The plan-only path must not try to create it.
large, = run(1000, 305408, 256, 'portable', 0, '--plan-only')
assert large['planned_bytes'] > 100 * 1024**3
if sys.platform.startswith('linux'):
    xdp, = run(1000, 1416, 1, 'xdp', 7, '--plan-only')
    # UMEM is rocketnet_xdp_umem_bytes in the SDK. Do not restate the product here.
    assert xdp['umem_bytes'] > 0
    assert xdp['worker_threads'] == 7
    assert xdp['sockets'] == 7
refused = subprocess.run([str(binary), '1001', '1416', '1', 'portable', '0', '--plan-only'],
                         capture_output=True, text=True, timeout=10)
assert refused.returncode != 0 and 'memory plan unavailable' in refused.stderr, (
    refused.returncode, refused.stdout, refused.stderr)
print('test_transport_memory_plans_match_allocations')
