# Test Scaffolding and the `ROCKETNET_DEV` Boundary

This document records the complete inventory of development and test-only scaffolding across the RocketNet SDK (`sdk/*`), build graph (`CMakeLists.txt`), and test suites (`tests/*`, `functional/*`), and defines the invariant that every test seam, hook macro, deterministic fixture helper, fault injector, and inspection witness is compiled and declared only when `ROCKETNET_DEV=1`.

## Core Invariant

1. **Single Boundary (`ROCKETNET_DEV`)**:
   - `ROCKETNET_DEV=1` (CMake `-DROCKETNET_DEV=ON`, the development default) enables development keys, development token minting, the in-memory link simulator, and every test seam: fault injection, probes, cost counters, oracles, witnesses, ledgers, step-driven and fake transports, and introspection. `ROCKETNET_DEV` is the one switch; there is no per-feature test macro. Every seam is armed by a test call (or, for three named environment variables, by the environment) and is inert until then.
   - `ROCKETNET_DEV=0` (CMake `-DROCKETNET_DEV=OFF`, production) compiles out all development and test scaffolding from `rocketnet` (static) and `rocketnet_shared` (shared/DLL + Windows import library), links `test` and `test_support` against the production library, and installs no development key, token minter or simulator.
2. **Public Header (`sdk/rocketnet.h`)**:
   - `sdk/rocketnet.h` never declares any internal test seam, `_for_test` witness, deterministic connect-token fixture helper, or reset-inventory walker.
   - Development-only public helpers (`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and `rocketnet_create_dev_connect_token`) are guarded by `#if ROCKETNET_DEV` in `sdk/rocketnet.h` and compiled only when `ROCKETNET_DEV=1`.

---

## 1. The One Switch and the Seams It Guards

`ROCKETNET_DEV` (sdk/rocketnet.h) is the only switch. The per-feature test macros that used to sit beside it (`ROCKETNET_TEST_SEAMS`, the `*_TEST_HOOKS` family, `ROCKETNET_IOCP_TESTS`, `ROCKETNET_RIO_TESTS`, `ROCKETNET_XDP_TESTS`, `ROCKETNET_WINDOWS_LIFECYCLE_TRACK`, `ROCKETNET_TEST_CLOCK_WRAP`) are gone, and `tools/check_test_scaffolding_boundary.py` fails if one comes back. Every seam is now plain `#if ROCKETNET_DEV`, with the platform condition beside it where the code is for one platform (the Windows lifecycle ledger is `ROCKETNET_DEV && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS`, the POSIX send-failure registry is `ROCKETNET_DEV && ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS`).

Seams, by the files that carry them:

| Seam | Files |
| --- | --- |
| ACK visit counter, span-cost, block-cost and pending-stats witnesses | `sdk/rocketnet_ack.cpp`, `sdk/rocketnet_allocator.cpp`, `sdk/rocketnet_allocator.h`, `sdk/rocketnet_window_ack.cpp`, `sdk/rocketnet_window_ack.h`, `sdk/rocketnet_channel.h`, `sdk/rocketnet_channel_reliable.cpp`, `sdk/rocketnet_client_transport.cpp`, `proofs/cbmc/proof_ack_apply_run.c` |
| Link simulator and its test switch | `sdk/rocketnet_api.cpp`, `sdk/rocketnet_config.cpp`, `sdk/rocketnet_config.h`, `sdk/rocketnet_network_simulator.cpp` |
| Payload-copy counters, secret-wipe witnesses | `sdk/rocketnet_message.h`, `sdk/rocketnet_message.cpp`, `sdk/rocketnet_config.cpp`, `sdk/rocketnet_connect_token.cpp`, `tests/test_unit_secrets_wiped.cpp` |
| io_uring fake CQE source and rearm counters | `sdk/rocketnet_server_uring.h`, `sdk/rocketnet_server_uring_linux.cpp` |
| Receive hook, in-memory environment table, send-failure registry, Darwin `sendmsg_x`/`recvmsg_x` hooks | `sdk/rocketnet_platform.h`, `sdk/rocketnet_platform.cpp`, `tests/test_msgx.cpp` |
| Portable transmit fault hooks, loopback fault hooks | `sdk/rocketnet_driver_builtin.cpp`, `sdk/rocketnet_driver_loopback.h`, `sdk/rocketnet_driver_loopback.cpp` |
| GSO send-error and worker-handoff seams | `sdk/rocketnet_client_gso.h`, `sdk/rocketnet_client_gso_linux.cpp`, `sdk/rocketnet_server_gso.h`, `sdk/rocketnet_server_gso_linux.cpp` |
| XDP staging checks and AF_XDP socket cases | `sdk/rocketnet_server_transport.cpp`, `sdk/rocketnet_server_socket_linux.cpp`, `sdk/rocketnet_server_socket.h` |
| IOCP and RIO fault injection, RIO completion overflow | `sdk/rocketnet_server_iocp.h`, `sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_rio.h`, `sdk/rocketnet_server_rio_windows.cpp` |
| Windows lifecycle ledger | `sdk/rocketnet_frame_sync.cpp`, `sdk/rocketnet_frame_sync.h`, `sdk/rocketnet_platform.cpp`, `sdk/rocketnet_platform.h` |
| Client and server transport seams, session sequence start, packet-builder and encryption-map probes | `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`, `sdk/rocketnet_client_protocol.cpp`, `sdk/rocketnet_client_slots.cpp`, `sdk/rocketnet_encryption_map.cpp`, `sdk/rocketnet_packet_builder.cpp`, `sdk/rocketnet_channel_sequenced.cpp`, `sdk/rocketnet_test_seams.h` |

Seams armed by the environment rather than by a test call, each inert unless its variable is set, and present in every `ROCKETNET_DEV=1` library: `ROCKETNET_DEV_SESSION_SEQUENCE_START` (`sdk/rocketnet_test_seams.h`), `ROCKETNET_TEST_GSO_PLAIN` and `ROCKETNET_TEST_XDP_FAIL_AT_QUEUE` (Linux).

One build shape remains that is not a development switch: `ROCKETNET_CONFIG_PARSE_ONLY`, defined by `test_xdp` and `test_iocp`, which compile `sdk/rocketnet_config.cpp` for `rocketnet_config_parse_int_value` alone. It removes code; it enables nothing.

---

## 2. Source-by-Source Inventory of `ROCKETNET_DEV` and Test Scaffolding

### 2.1 Portable SDK Core (`sdk/*`)

1. **Connect Token Minting and Deterministic Fixture Writer (`sdk/rocketnet_connect_token.h`, `sdk/rocketnet_connect_token.cpp`, `sdk/rocketnet.cpp`)**:
   - **Production (`ROCKETNET_DEV=0` and `ROCKETNET_DEV=1`)**:
     - `rocketnet_write_connect_token_sealed`: seals connect tokens using fresh ephemeral X25519 keypairs (`crypto_box_keypair`), with zero caller-supplied ephemeral secret key path in `DEV=0`.
     - `rocketnet_read_connect_token_sealed`: unseals and validates connect tokens.
   - **Development / Test Only (`#if ROCKETNET_DEV`)**:
     - `rocketnet_dev_server_secret_key[32]` and `rocketnet_dev_backend_secret_key[32]` (`sdk/rocketnet_connect_token.cpp`, `sdk/rocketnet.cpp`).
     - `rocketnet_box_seal_with_ephemeral` (`sdk/rocketnet_connect_token.cpp`): deterministic X25519 box seal helper with caller-supplied ephemeral secret key.
     - `ephemeral_secret_key` parameter/branch in `rocketnet_write_connect_token_sealed` (`sdk/rocketnet_connect_token.cpp`): compiled only under `#if ROCKETNET_DEV`.
     - `rocketnet_write_connect_token_deterministic` (`sdk/rocketnet_connect_token.h`, `sdk/rocketnet_connect_token.cpp`): deterministic connect-token writer for known-answer vectors and unit tests.
     - `rocketnet_create_dev_connect_token` (`sdk/rocketnet.h`, `sdk/rocketnet.cpp`): public development token minter.

2. **Connection Reset and Field-Coverage Inventory Walker (`sdk/rocketnet_connection_reset.h`, `sdk/rocketnet_connection_reset.cpp`)**:
   - **Production (`ROCKETNET_DEV=0` and `ROCKETNET_DEV=1`)**:
     - `rocketnet_client_connection_reset`: resets client protocol per-session state and wipes cryptographic secrets.
     - `rocketnet_client_protocol_wipe_secrets`: wipes client cryptographic secret material (`session_TokenData`, `rx_key`, `tx_key`) through `rocketnet_secret_wipe`.
     - `rocketnet_server_connection_reset`: resets server per-client slot protocol state and wipes session keys.
   - **Development / Test Only (`#if ROCKETNET_DEV`)**:
     - `ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS`, `ROCKETNET_CLIENT_TRANSPORT_CONNECTION_FIELDS`, `ROCKETNET_SERVER_TRANSPORT_CONNECTION_FIELDS` X-macros (`sdk/rocketnet_connection_reset.h`).
     - `struct rocketnet_reset_member_t`, `ROCKETNET_RESET_MEMBER`, `rocketnet_reset_align_up`, and `rocketnet_reset_inventory_matches` (`sdk/rocketnet_connection_reset.h`, `sdk/rocketnet_connection_reset.cpp`).
     - `rocketnet_client_connection_reset_inventory_matches_for_test`, `rocketnet_client_transport_connection_reset_inventory_matches_for_test`, `rocketnet_server_connection_reset_inventory_matches_for_test` (`sdk/rocketnet_connection_reset.h`, `sdk/rocketnet_connection_reset.cpp`, `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`).

3. **Client and Server Transports, Link Simulator, and Session Seams (`sdk/rocketnet.cpp`, `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`, `sdk/rocketnet_client_slots.h`, `sdk/rocketnet_network_simulator.h`, `sdk/rocketnet_network_simulator.cpp`)**:
   - **Guard `#if ROCKETNET_DEV`**:
     - In-memory link simulator (`rocketnet_network_simulator_*` in `sdk/rocketnet_network_simulator.cpp` and `sdk/rocketnet.cpp`).
     - `rocketnet_dev_session_sequence_start` (`sdk/rocketnet_layers.h`, `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`).
     - `rocketnet_server_transport_dev_config` (`sdk/rocketnet_layers.h`, `sdk/rocketnet_server_transport.cpp`).
     - Step-driven client transport (`rocketnet_client_create_step_driven_at_timestamp`, `rocketnet_client_create_testing`, `rocketnet_client_step_context_t`, `ROCKETNET_CLIENT_STEP_DRIVEN`).
     - `rocketnet_client_slot_handle_t` generation-checked slot helpers (`sdk/rocketnet_client_slots.h`).
   - **More `#if ROCKETNET_DEV` seams**:
     - `rocketnet_client_transport_set_memory_plan_seam_for_test`, `rocketnet_client_transport_clear_memory_plan_seam_for_test` (`sdk/rocketnet_client_transport.cpp`).
     - `rocketnet_server_transport_set_writer_fault_for_test`, `rocketnet_server_transport_force_send_batch_refusal_for_test` (`sdk/rocketnet_server_transport.cpp`).
     - The same guard also wraps seams in `sdk/rocketnet_api.cpp`, `sdk/rocketnet_channel_sequenced.cpp`, `sdk/rocketnet_client_protocol.cpp`, `sdk/rocketnet_client_slots.cpp`, `sdk/rocketnet_client_slots.h`, `sdk/rocketnet_encryption_map.cpp`, `sdk/rocketnet_encryption_map.h`, `sdk/rocketnet_network_simulator.cpp`, `sdk/rocketnet_network_simulator.h`, `sdk/rocketnet_packet_builder.cpp`, `sdk/rocketnet_packet_builder.h`, `sdk/rocketnet_server_protocol.cpp`, `sdk/rocketnet_server_protocol.h`, `sdk/rocketnet_server_replay_marks.cpp`, and `sdk/rocketnet_server_uring_linux.cpp`.

### 2.2 Platform-Specific Scaffolding (Linux, macOS, Windows)

1. **Linux (`sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_server_socket_linux.cpp`)**: the fake io_uring CQE source and multishot rearm counters; the AF_XDP socket and UMEM cases `test_xdp` runs; the TX staging invariant checker; the unit endpoint test's linker `--wrap=rocketnet_platform_time` clock-read counter in `test`.
2. **macOS (`sdk/rocketnet_platform.cpp`)**: `sendmsg_x` / `recvmsg_x` batch and `EMSGSIZE` fault hooks, run by `test_msgx` (linked against `rocketnet_dev`).
3. **Windows (`sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_rio_windows.cpp`)**: IOCP and RIO fault hooks in `test_iocp` (in `DEV=0`, `test_windows_transports_production` links the production `rocketnet` library); the fake RIO completion overflow seam; and the handle/socket/thread lifecycle ledger in `sdk/rocketnet_platform.h`, `sdk/rocketnet_platform.cpp`, `sdk/rocketnet_frame_sync.h`, `sdk/rocketnet_frame_sync.cpp`, `sdk/rocketnet_server_iocp_windows.cpp`, and `sdk/rocketnet_server_rio_windows.cpp`.

---

## 3. Build Targets and `DEV=0` vs `DEV=1` Matrix (`CMakeLists.txt`)

| Target | `ROCKETNET_DEV=ON` (`DEV=1`) | `ROCKETNET_DEV=OFF` (`DEV=0`) |
| --- | --- | --- |
| `rocketnet` (static) | `ROCKETNET_DEV=1` | `ROCKETNET_DEV=0`, zero test scaffolding or dev keys |
| `rocketnet_shared` (shared + import lib) | `ROCKETNET_DEV=1`, exports match `tools/exports.txt` | `ROCKETNET_DEV=0`, exports match `tools/exports.txt`, zero test scaffolding |
| `rocketnet_test_support` | Links `PUBLIC rocketnet_dev` | Links `PUBLIC rocketnet` (production static library) |
| `test` (`./bin/test`) | Runs full unit + seam + simulator suites against `rocketnet_dev` | Runs the production-compatible `tests/test_unit_*.cpp` suites directly against production `rocketnet` |
| `test_lifecycle`, `test_production_client_lifecycle`, `test_platform_primitives`, `test_transport`, `test_windows_transports_production` | Link `rocketnet` | Link production `rocketnet` (`DEV=0`) and verify real socket/transport lifecycle |
| `rocketnet_dev`, `rocketnet_shared_dev`, `functional`, `test_wide`, `test_channels_1024`, `test_channels_10k`, `test_xdp`, `test_msgx`, `test_iocp` | Built with `ROCKETNET_DEV=1` (`rocketnet_dev`) | Built with `ROCKETNET_DEV=1` (`rocketnet_dev`, never installed) when `ROCKETNET_BUILD_DEV_LIBRARIES=ON` |

---

## 4. Automated Enforcement (`tools/check_test_scaffolding_boundary.py` & `tools/check_exports.py`)

Run the boundary audit and negative-control suite against a `DEV=0` build:

```sh
python3 tools/check_test_scaffolding_boundary.py \
  --build build-dev0 \
  --shared bin/librocketnet.so \
  --archive build-dev0/librocketnet.a \
  --red-controls
```

On Windows (including the DLL import library):

```powershell
py -3 tools\check_test_scaffolding_boundary.py --build build-dev0 --shared bin\Release\rocketnet.dll --archive build-dev0\Release\rocketnet.lib --import-library build-dev0\Release\rocketnet_import.lib --red-controls
```

What `tools/check_test_scaffolding_boundary.py` verifies:
1. **Header and Source Audit**: `sdk/rocketnet.h` exposes no test-scaffolding token; none of the retired per-feature switches appears under `sdk/`, `tests/`, `functional/`, `CMakeLists.txt` or the workflows; `sdk/rocketnet_connect_token.h` and `sdk/rocketnet_connection_reset.h` guard all deterministic token and reset-inventory scaffolding under `#if ROCKETNET_DEV`.
2. **Build-Tree Audit**: `build-dev0` is configured with `ROCKETNET_DEV=OFF`, and compiles `rocketnet`, `rocketnet_shared`, `rocketnet_test_support`, and `test` with `ROCKETNET_DEV=0` and no retired switch.
3. **Artifact Symbol and Byte Audit (`tools/check_exports.py`)**: `rocketnet` static archive, `rocketnet_shared` shared library, and Windows `rocketnet_import.lib` match `tools/exports.txt`, keep the static crypto namespace private, and contain zero development/test symbols, zero `_for_test` witnesses, zero deterministic token or reset-inventory symbols, and zero development secret key bytes.
4. **Negative Controls (`--red-controls`)**:
   - A compiler negative control verifies that `rocketnet_dev_session_sequence_start` is undeclared with `ROCKETNET_DEV=0`.
   - Symbol negative controls verify that referencing `rocketnet_write_connect_token_deterministic`, `rocketnet_reset_inventory_matches`, or `rocketnet_*_connection_reset_inventory_matches_for_test` with `ROCKETNET_DEV=0` fails compilation.
