#!/usr/bin/env python3
"""Compare pre-allocation plans with independent allocator observations."""
import json
import os
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/transport_memory').resolve()
profiles = [(1, 1416, 1), (2, 1416, 1), (1000, 1416, 1), (1000, 1416, 16), (1000, 4096, 16)]
transports = ['portable']
if sys.platform == 'darwin':
    transports.append('msgx')
elif sys.platform == 'win32':
    transports.append('iocp')
elif sys.platform.startswith('linux'):
    transports.append('gso')


def run(*arguments):
    result = subprocess.run([str(binary), *map(str, arguments)],
                            capture_output=True, text=True, timeout=120)
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if result.returncode != 0:
        raise AssertionError(
            f"{binary.name} {' '.join(map(str, arguments))} exited {result.returncode}; "
            f"stdout={result.stdout!r} stderr={result.stderr!r}")
    return [json.loads(line) for line in result.stdout.splitlines()]


for transport in transports:
    for profile in profiles if transport == 'portable' else [profiles[0]]:
        plan, observed = run(*profile, transport, 1)
        assert plan['umem_bytes'] == 0
        assert observed['peak_allocator_bytes'] <= plan['planned_bytes'], (
            transport, profile, 'planned_bytes', plan['planned_bytes'],
            'peak_allocator_bytes', observed['peak_allocator_bytes'])
        if plan['worker_threads'] == 0:
            assert observed['live_allocator_bytes'] == plan['planned_bytes'], (
                transport, profile, 'planned_bytes', plan['planned_bytes'],
                'live_allocator_bytes', observed['live_allocator_bytes'])
        assert plan['os_and_application_bytes'] is None
        session, = run(*profile, transport, 1, '--session-plan-only')
        assert session['scope'] == 'one_public_server_one_pinned_transport'
        assert session['create_callback_request_bytes'] > session['create_payload_bytes']
        assert session['concurrent_sdk_generations'] == 1
        assert session['known_active_or_stopping_requests_bytes'] >= (
            session['create_callback_request_bytes'] + session['active_transport_sdk_callback_bytes'] +
            session['active_xdp_umem_bytes'] + session['active_rio_registered_region_bytes'])
        assert session['native_retained_on_failed_cancel_bytes'] is None
        assert session['config_snapshot_frozen_for_future_start'] is False
        assert session['one_block_sufficient'] is False
# This legal field combination is deliberately too large for the default
# reassembly budget. The plan-only path must not try to create it.
large, = run(1000, 305408, 256, 'portable', 0, '--plan-only')
assert large['reassembly_bytes'] > 512 * 1024**2
if sys.platform.startswith('linux'):
    xdp, = run(1000, 1416, 1, 'xdp', 7, '--plan-only')
    # UMEM is rocketnet_xdp_umem_bytes in the SDK. Do not restate the product here.
    assert xdp['umem_bytes'] > 0
    assert xdp['worker_threads'] == 7
    assert xdp['sockets'] == 7
if sys.platform == 'win32':
    rio, = run(1, 1416, 1, 'rio', 0, '--session-plan-only')
    assert rio['active_rio_registered_region_bytes'] > 0
    assert rio['active_xdp_umem_bytes'] == 0
    assert rio['active_transport_sdk_callback_bytes'] > 0
refused = subprocess.run([str(binary), '1001', '1416', '1', 'portable', '0', '--plan-only'],
                         capture_output=True, text=True, timeout=10)
assert refused.returncode != 0 and 'memory plan unavailable' in refused.stderr, (
    refused.returncode, refused.stdout, refused.stderr)
env = dict(os.environ)
env['ROCKETNET_SERVER_MAX_CLIENTS'] = '2'
refused_env = subprocess.run([str(binary), '1', '1416', '1', 'portable', '0', '--session-plan-only'],
                             capture_output=True, text=True, timeout=10, env=env)
assert refused_env.returncode != 0 and 'environment overrides' in refused_env.stderr
refused_candidate = subprocess.run([str(binary), '1', '1416', '1', 'auto', '0', '--session-plan-only'],
                                   capture_output=True, text=True, timeout=10)
assert refused_candidate.returncode != 0 and 'memory plan unavailable' in refused_candidate.stderr
print('test_transport_memory_plans_match_allocations')
