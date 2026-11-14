#!/usr/bin/env python3
"""Compare pre-allocation plans with independent allocator observations."""
import json
import os
from pathlib import Path
import subprocess
import sys

from nightly_timeout import execution_timeout

binary = Path(sys.argv[1] if len(sys.argv) > 1 else 'bin/transport_memory').resolve()
# (clients, payload column). The payload column is ignored: the payload is
# fixed at ROCKETNET_MAX_PACKET_PAYLOAD_BYTES (1416). The 1000-client row is
# the default config, which must plan and create.
profiles = [(1, 1416), (2, 1416), (1000, 1416)]
transports = ['portable']
# The worker count is never copied here: it moves with the configuration and
# the transports' defaults (IOCP's send threads with the client count's lanes,
# GSO's workers, RIO's lanes), and a copy fails the day the library changes
# it. Each row is held to the count the library's planner prints for the same
# profile and transport (--plan-only; tests/test_unit_network_threads.cpp pins
# the planner to the named defaults), every row of one profile must agree, and
# where the probe can count the process's threads (Linux) the threads create
# started must be exactly that count: the native planner includes every
# worker and no worker is unplanned.
if sys.platform == 'darwin':
    transports.append('msgx')
elif sys.platform == 'win32':
    transports.append('iocp')
elif sys.platform.startswith('linux'):
    transports.append('gso')


def run(*arguments):
    result = subprocess.run([str(binary), *map(str, arguments)],
                            capture_output=True, text=True, timeout=execution_timeout(120))
    print(result.stdout, end='')
    print(result.stderr, end='', file=sys.stderr)
    if result.returncode != 0:
        raise AssertionError(
            f"{binary.name} {' '.join(map(str, arguments))} exited {result.returncode}; "
            f"stdout={result.stdout!r} stderr={result.stderr!r}")
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    if not rows:
        raise SystemExit(f"{binary.name} {' '.join(map(str, arguments))} printed 0 JSON lines: nothing was checked")
    return rows


planned = {}


def planned_workers(profile, transport):
    """The worker threads the library's planner gives this profile and transport
    at one queue: the probe's --plan-only row, which starts nothing."""
    key = (profile, transport)
    if key not in planned:
        row, = run(*profile, transport, 1, '--plan-only')
        workers = row['worker_threads']
        assert isinstance(workers, int) and workers >= 1, (transport, profile, 'worker_threads', workers)
        # every worker has a stack in the plan
        assert row['thread_stack_bytes'] > 0 and row['thread_stack_bytes'] % workers == 0, (transport, profile, row)
        planned[key] = workers
    return planned[key]


for transport in transports:
    for profile in profiles if transport == 'portable' else [profiles[0]]:
        plan, observed = run(*profile, transport, 1)
        assert plan['umem_bytes'] == 0
        assert observed['peak_allocator_bytes'] <= plan['planned_bytes'], (
            transport, profile, 'planned_bytes', plan['planned_bytes'],
            'peak_allocator_bytes', observed['peak_allocator_bytes'])
        assert plan['worker_threads'] == planned_workers(profile, transport), (transport, profile, plan['worker_threads'])
        started = observed.get('started_threads')
        if sys.platform.startswith('linux'):
            # the probe counts /proc/self/task across create: an observation the
            # plan does not make, so a worker the planner leaves out fails here
            assert started is not None, (transport, profile, 'started_threads not observed on Linux')
        if started is not None:
            assert started == plan['worker_threads'], (
                transport, profile, 'planned worker_threads', plan['worker_threads'], 'started_threads', started)
        # msgx keeps a connected socket per client beside the shared one
        assert plan['sockets'] == (1 + profile[0] if transport == 'msgx' else 1), (transport, profile, plan['sockets'])
        # Startup shims can be freed before this observation, even for
        # portable. The native probe bounds that difference by its planned
        # worker allocations and refuses any allocations left after destroy.
        assert 0 < observed['live_allocator_bytes'] <= observed['peak_allocator_bytes']
        assert plan['os_and_application_bytes'] is None
        session, = run(*profile, transport, 1, '--session-plan-only')
        assert session['scope'] == 'one_public_server_one_pinned_transport'
        assert session['create_callback_request_bytes'] > session['create_payload_bytes']
        assert session['concurrent_sdk_generations'] == 1
        assert session['known_active_or_stopping_requests_bytes'] >= (
            session['create_callback_request_bytes'] + session['active_transport_sdk_callback_bytes'] +
            session['active_xdp_umem_bytes'] + session['active_rio_registered_region_bytes'])
        assert session['native_retained_on_failed_cancel_bytes'] is None
        assert session['config_copy_frozen_for_future_start'] is False
        assert session['one_block_sufficient'] is False
        assert plan['allocator_bytes'] == plan['planned_bytes'] - plan['umem_bytes']
        assert plan['os_bytes'] == plan['umem_bytes'] + plan['thread_stack_bytes'] + plan['mapped_ring_bytes']
        assert plan['thread_stack_bytes'] > 0
    # The lifecycle rows: the public server through start, stop, start, stop
    # and the public client through connect, disconnect, connect, disconnect.
    # required_memory bounds the first cycle's peak and the second cycle takes
    # no more than the first: one block sized from the plan never runs out.
    for profile in [profiles[0], profiles[1]]:
        _, server, client = run(*profile, transport, 1, '--lifecycle')
        assert server['lifecycle'] == 'server' and client['lifecycle'] == 'client'
        for row in (server, client):
            assert 0 < row['first_cycle_peak_bytes'] <= row['required_bytes'], (transport, profile, row)
            assert row['second_cycle_peak_bytes'] == row['first_cycle_peak_bytes'], (transport, profile, row)
        assert server['os_bytes'] == server['thread_stack_bytes'] + server['umem_bytes'] + server['mapped_ring_bytes']
        assert server['thread_stack_bytes'] > 0 and server['worker_threads'] == planned_workers(profile, transport), (
            transport, profile, server)
    # The light rows: the public light presets, each created with an
    # allocator exposing exactly its public required_memory bytes in one
    # guarded block. Sufficiency is the arena's own evidence (peak at or
    # under capacity, no refusal, no canary damage, nothing live after
    # destroy, second cycle peak equal to the first), never the process
    # heap behind it. The 1000-client row is the max supported light
    # server population; it runs on portable, the smaller rows on every
    # native transport this checker drives.
    light_profiles = [profiles[0], profiles[1]]
    if transport == 'portable':
        light_profiles.append(profiles[2])
    for profile in light_profiles:
        _, server, client = run(*profile, transport, 1, '--light-lifecycle')
        assert server['lifecycle'] == 'server' and client['lifecycle'] == 'client'
        assert server['profile'] == 'light' and client['profile'] == 'light'
        for row in (server, client):
            assert row['arena_capacity_bytes'] == row['required_bytes'], (transport, profile, row)
            assert 0 < row['first_cycle_peak_bytes'] <= row['required_bytes'], (transport, profile, row)
            assert row['second_cycle_peak_bytes'] == row['first_cycle_peak_bytes'], (transport, profile, row)
            assert row['final_live_bytes'] == 0, (transport, profile, row)
            assert row['refused_allocations'] == 0, (transport, profile, row)
            assert row['invalid_frees'] == 0, (transport, profile, row)
            assert row['canaries_ok'] is True, (transport, profile, row)
        assert server['os_bytes'] == server['thread_stack_bytes'] + server['umem_bytes'] + server['mapped_ring_bytes']
        assert server['thread_stack_bytes'] > 0 and server['worker_threads'] == planned_workers(profile, transport), (
            transport, profile, server)
# Each transport's plan at its DEFAULT configuration (queues 0: nothing asked),
# held to the defaults where the library defines them,
# sdk/rocketnet_network_threads.h, read here and never copied, so a change of a
# default (rio's lanes, gso's workers) is checked on the day it lands.
header = Path(__file__).resolve().parent.parent / 'sdk' / 'rocketnet_network_threads.h'
defaults = {}
for line in header.read_text().splitlines():
    fields = line.split()
    if len(fields) == 3 and fields[0] == '#define' and fields[2].isdigit():
        defaults[fields[1]] = int(fields[2])
for name in ('ROCKETNET_GSO_DEFAULT_WORKERS', 'ROCKETNET_IO_URING_DEFAULT_WORKERS', 'ROCKETNET_XDP_DEFAULT_THREADS',
             'ROCKETNET_RIO_DEFAULT_LANES', 'ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS', 'ROCKETNET_IOCP_DEFAULT_SEND_THREADS',
             'ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS'):
    assert defaults.get(name, 0) >= 1, (str(header), name, 'not defined as a positive number')


def default_plan(transport, flag='--plan-only'):
    row, = run(1, 1416, transport, 0, flag)
    return row


if sys.platform.startswith('linux'):
    gso = default_plan('gso')
    assert gso['worker_threads'] == defaults['ROCKETNET_GSO_DEFAULT_WORKERS'] == gso['sockets'], ('gso default', gso)
    xdp_default = default_plan('xdp')
    assert xdp_default['worker_threads'] == defaults['ROCKETNET_XDP_DEFAULT_THREADS'], ('xdp default', xdp_default)
    uring_default = subprocess.run([str(binary), '1', '1416', 'io_uring', '0', '--plan-only'],
                                   capture_output=True, text=True, timeout=execution_timeout(10))
    if uring_default.returncode == 0:
        uring_row = json.loads(uring_default.stdout)
        assert uring_row['worker_threads'] == defaults['ROCKETNET_IO_URING_DEFAULT_WORKERS'], ('io_uring default', uring_row)
    else:
        assert 'memory plan unavailable' in uring_default.stderr, (uring_default.returncode, uring_default.stdout, uring_default.stderr)
if sys.platform == 'darwin':
    msgx = default_plan('msgx')
    # the transmit workers and the one receive worker
    assert msgx['worker_threads'] == defaults['ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS'] + 1, ('msgx default', msgx)
if sys.platform == 'win32':
    # on the wildcard the Windows transports take one lane and one completion
    # worker; on a specific address, their defaults
    rio_wildcard = default_plan('rio')
    assert rio_wildcard['worker_threads'] == 1, ('rio wildcard', rio_wildcard)
    rio_default = default_plan('rio', '--plan-only-at-address')
    assert rio_default['worker_threads'] == defaults['ROCKETNET_RIO_DEFAULT_LANES'] == rio_default['sockets'], ('rio default', rio_default)
    iocp_default = default_plan('iocp', '--plan-only-at-address')
    assert iocp_default['sockets'] == defaults['ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS'], ('iocp default', iocp_default)
    # the send threads are the default, or the send lanes when fewer (one
    # client's server is sized at its minimum lanes)
    send_threads = iocp_default['worker_threads'] - iocp_default['sockets']
    assert 1 <= send_threads <= defaults['ROCKETNET_IOCP_DEFAULT_SEND_THREADS'], ('iocp default send threads', iocp_default)
if sys.platform.startswith('linux'):
    xdp, = run(1000, 1416, 'xdp', 7, '--plan-only')
    # UMEM is rocketnet_xdp_umem_bytes in the SDK. Do not restate the product here.
    assert xdp['umem_bytes'] > 0
    assert xdp['worker_threads'] == 7  # seven queue workers
    assert xdp['sockets'] == 7
    assert xdp['os_bytes'] >= xdp['umem_bytes'] + xdp['thread_stack_bytes'] and xdp['thread_stack_bytes'] > 0
    # the provided buffer rings are kernel-mapped memory outside the allocator;
    # a build without liburing has no io_uring plan
    uring = subprocess.run([str(binary), '1', '1416', 'io_uring', '4', '--plan-only'],
                           capture_output=True, text=True, timeout=execution_timeout(10))
    if uring.returncode == 0:
        uring_plan = json.loads(uring.stdout)
        assert uring_plan['mapped_ring_bytes'] > 0 and uring_plan['os_bytes'] >= uring_plan['mapped_ring_bytes']
    else:
        assert 'memory plan unavailable' in uring.stderr, (uring.returncode, uring.stdout, uring.stderr)
if sys.platform == 'win32':
    rio_plan, = run(1, 1416, 'rio', 0, '--plan-only')
    assert rio_plan['worker_threads'] == 1  # one RIO I/O worker
    # the RIO registered region comes from the allocator; nothing but stacks is OS-owned
    assert rio_plan['allocator_bytes'] == rio_plan['planned_bytes']
    assert rio_plan['os_bytes'] == rio_plan['thread_stack_bytes']
    rio, = run(1, 1416, 'rio', 0, '--session-plan-only')
    assert rio['active_rio_registered_region_bytes'] > 0
    assert rio['active_xdp_umem_bytes'] == 0
    assert rio['active_transport_sdk_callback_bytes'] > 0
refused = subprocess.run([str(binary), '1001', '1416', 'portable', '0', '--plan-only'],
                         capture_output=True, text=True, timeout=execution_timeout(10))
assert refused.returncode != 0 and 'memory plan unavailable' in refused.stderr, (
    refused.returncode, refused.stdout, refused.stderr)
env = dict(os.environ)
env['ROCKETNET_SERVER_MAX_CLIENTS'] = '2'
refused_env = subprocess.run([str(binary), '1', '1416', 'portable', '0', '--session-plan-only'],
                             capture_output=True, text=True, timeout=execution_timeout(10), env=env)
assert refused_env.returncode != 0 and 'environment overrides' in refused_env.stderr
env.pop('ROCKETNET_SERVER_MAX_CLIENTS')
env['ROCKETNET_DEV_SERVER_LATENCY_MS'] = '1'
refused_impairment = subprocess.run([str(binary), '1', '1416', 'portable', '0', '--session-plan-only'],
                                    capture_output=True, text=True, timeout=execution_timeout(10), env=env)
assert refused_impairment.returncode != 0 and 'environment overrides' in refused_impairment.stderr
refused_candidate = subprocess.run([str(binary), '1', '1416', 'auto', '0', '--session-plan-only'],
                                   capture_output=True, text=True, timeout=execution_timeout(10))
assert refused_candidate.returncode != 0 and 'memory plan unavailable' in refused_candidate.stderr
print('test_transport_memory_plans_match_allocations')
