# Test Scaffolding and the `ROCKETNET_DEV` Boundary

This document records the complete inventory of development and test-only scaffolding across the RocketNet SDK (`sdk/*`), build graph (`CMakeLists.txt`), and test suites (`tests/*`, `functional/*`), and defines the invariant that every test seam, hook macro, deterministic fixture helper, fault injector, and inspection witness is compiled and declared only when `ROCKETNET_DEV=1`.

## Core Invariant

1. **Single Boundary (`ROCKETNET_DEV`)**:
   - `ROCKETNET_DEV=1` (CMake `-DROCKETNET_DEV=ON`, the development default) enables development keys, development token minting, the in-memory link simulator, and allows test-scaffolding macros (`ROCKETNET_TEST_SEAMS` and the specialized `*_TEST_HOOKS` / `*_TESTS` macros) on test targets and `rocketnet_test_hooks`.
   - `ROCKETNET_DEV=0` (CMake `-DROCKETNET_DEV=OFF`, production) compiles out all development and test scaffolding from `rocketnet` (static) and `rocketnet_shared` (shared/DLL + Windows import library), builds **no** `rocketnet_test_hooks` library, links `test` (`./bin/test`) directly against the production `rocketnet` archive through `rocketnet_test_support`, and refuses any attempt to enable `ROCKETNET_TEST_SEAMS` or any specialized test-hook macro at both CMake configure time (`CMakeLists.txt`) and C/C++ preprocess/compile time (`sdk/rocketnet_test_seams.h`).
2. **Public Header (`sdk/rocketnet.h`)**:
   - `sdk/rocketnet.h` never declares any internal test seam, `_for_test` witness, deterministic connect-token fixture helper, or reset-inventory walker.
   - Development-only public helpers (`ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and `rocketnet_create_dev_connect_token`) are guarded by `#if ROCKETNET_DEV` in `sdk/rocketnet.h` and compiled only when `ROCKETNET_DEV=1`.

---

## 1. Compiler Definitions and Preprocessor Guard Inventory (`sdk/rocketnet_test_seams.h`)

Every test-scaffolding macro in the repository is normalized in `sdk/rocketnet_test_seams.h`. When `ROCKETNET_DEV=0`, enabling any of the 22 macros below triggers an immediate `#error "<MACRO> requires ROCKETNET_DEV=1"` in `sdk/rocketnet_test_seams.h` and a `message(FATAL_ERROR ...)` at CMake configure time:

| Macro | Default | Scope / Targets (`ROCKETNET_DEV=1` only) | Implementation Files & Guarded Declarations / State / Branches |
| --- | --- | --- | --- |
| `ROCKETNET_TEST_SEAMS` | `1` when `ROCKETNET_DEV=1`, `0` when `ROCKETNET_DEV=0` (see `sdk/rocketnet_test_seams.h`) | `rocketnet_test_hooks`, `test`, `test_wide`, `test_channels_1024`, `test_channels_10k` | `sdk/rocketnet_api.cpp`, `sdk/rocketnet_client_protocol.cpp`, `sdk/rocketnet_client_slots.h`, `sdk/rocketnet_client_transport.cpp`, `sdk/rocketnet_server_transport.cpp`: internal unit-test seams. |
| `ROCKETNET_ACK_VISIT_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test`, `test_wide` | `sdk/rocketnet_ack.cpp`: ACK visit counters. |
| `ROCKETNET_ACK_COST_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_allocator.h`, `sdk/rocketnet_window_ack.h`, `sdk/rocketnet_window_ack.cpp`: ACK span-cost counters. |
| `ROCKETNET_ACK_STATS_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_client_transport.cpp`: ACK pending-stats witnesses. |
| `ROCKETNET_BLOCK_COST_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_channel.h`, `sdk/rocketnet_channel_reliable.cpp`: reliable-block scan-cost counters. |
| `ROCKETNET_LINK_SIMULATOR_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test`, `test_wide`, `test_channels_1024`, `test_channels_10k` | `sdk/rocketnet_api.cpp`, `sdk/rocketnet_config.cpp`, `sdk/rocketnet_config.h`: link-simulator test switch. |
| `ROCKETNET_MESSAGE_COPY_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_message.h`, `sdk/rocketnet_message.cpp`: payload-copy counters. |
| `ROCKETNET_SECRET_WIPE_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_config.cpp`, `sdk/rocketnet_connect_token.cpp`: secret-wipe witnesses. |
| `ROCKETNET_URING_REAP_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_server_uring.h`, `sdk/rocketnet_server_uring_linux.cpp`: io_uring reap test hooks. |
| `ROCKETNET_RECEIVE_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_platform.h`, `sdk/rocketnet_platform.cpp`: receive hooks. |
| `ROCKETNET_PORTABLE_TX_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_driver_builtin.cpp`: portable transmit fault hooks. |
| `ROCKETNET_RIO_OVERFLOW_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` (Windows `DEV=1`) | `sdk/rocketnet_server_rio_windows.cpp`: RIO completion-overflow seam. |
| `ROCKETNET_XDP_STAGING_TEST_HOOKS` | `0` | `test` | `sdk/rocketnet_server_transport.cpp`: XDP transmit-staging invariant checks. |
| `ROCKETNET_ENVIRONMENT_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test_wide` | `sdk/rocketnet_platform.h`, `sdk/rocketnet_platform.cpp`: in-memory environment table for tests. |
| `ROCKETNET_SEND_ERROR_TEST_HOOKS` | `0` | `rocketnet_dev`, `rocketnet_shared_dev`, `functional` | `sdk/rocketnet_platform.h`, `sdk/rocketnet_platform.cpp`, `sdk/rocketnet_client_gso.h`, `sdk/rocketnet_server_gso.h`: send-error hooks. |
| `ROCKETNET_LOOPBACK_TEST_HOOKS` | `0` | `rocketnet_test_hooks`, `test` | `sdk/rocketnet_driver_loopback.h`, `sdk/rocketnet_driver_loopback.cpp`: loopback fault hooks. |
| `ROCKETNET_MSGX_TEST_HOOKS` | `0` | `tests/test_msgx.cpp` (macOS) | `sdk/rocketnet_platform.cpp`: Darwin `sendmsg_x` / `recvmsg_x` fault and clamp hooks. |
| `ROCKETNET_WINDOWS_LIFECYCLE_TRACK` | `0` | `rocketnet_dev`, `functional` (Windows) | `sdk/rocketnet_platform.cpp`, `sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_rio_windows.cpp`: Windows handle/thread/socket lifecycle tracking. |
| `ROCKETNET_IOCP_TESTS` | `0` | `test_iocp` (Windows) | `sdk/rocketnet_server_iocp.h`, `sdk/rocketnet_server_iocp_windows.cpp`: IOCP fault-injection counters and hooks. |
| `ROCKETNET_RIO_TESTS` | `0` | `test_iocp` (Windows) | `sdk/rocketnet_server_rio.h`, `sdk/rocketnet_server_rio_windows.cpp`: RIO fault-injection counters and hooks. |
| `ROCKETNET_XDP_TESTS` | `0` | `test_xdp` (Linux) | `sdk/rocketnet_server_socket_linux.cpp`, `sdk/rocketnet_gateway_netlink_linux.cpp`: XDP/netlink test hooks. |
| `ROCKETNET_TEST_CLOCK_WRAP` | `0` | `tests/test_unit_endpoint.cpp` (Linux `DEV=1`) | `tests/test_unit_endpoint.cpp`: GNU ld `--wrap=rocketnet_platform_time` clock-read counter wrapper. |

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
   - **Guard `#if ROCKETNET_TEST_SEAMS` (requires `ROCKETNET_DEV=1`)**:
     - `rocketnet_client_transport_set_memory_plan_seam_for_test`, `rocketnet_client_transport_clear_memory_plan_seam_for_test` (`sdk/rocketnet_client_transport.cpp`).
     - `rocketnet_server_transport_set_writer_fault_for_test`, `rocketnet_server_transport_force_send_batch_refusal_for_test` (`sdk/rocketnet_server_transport.cpp`).

### 2.2 Platform-Specific Scaffolding (Linux, macOS, Windows)

1. **Linux (`sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_server_socket_linux.cpp`, `sdk/rocketnet_gateway_netlink_linux.cpp`)**:
   - `ROCKETNET_URING_REAP_TEST_HOOKS`: fake io_uring CQE source and multishot rearm counters.
   - `ROCKETNET_XDP_TESTS`: AF_XDP socket/UMEM/netlink fault injection in `test_xdp` (compiled with `rocketnet_dev` definitions).
   - `ROCKETNET_XDP_STAGING_TEST_HOOKS`: TX staging invariant checker in `test`.
   - `ROCKETNET_TEST_CLOCK_WRAP`: linker `--wrap=rocketnet_platform_time` in `test` (`DEV=1` only).
2. **macOS (`sdk/rocketnet_platform.cpp`)**:
   - `ROCKETNET_MSGX_TEST_HOOKS`: `sendmsg_x` / `recvmsg_x` batch and `EMSGSIZE` fault hooks in `test_msgx` (linked against `rocketnet_dev`).
3. **Windows (`sdk/rocketnet_server_iocp_windows.cpp`, `sdk/rocketnet_server_rio_windows.cpp`)**:
   - `ROCKETNET_IOCP_TESTS` and `ROCKETNET_RIO_TESTS`: IOCP and RIO fault hooks in `test_iocp` (compiled with `rocketnet_dev` definitions; in `DEV=0`, `test_windows_transports_production` links the production `rocketnet` library with zero test macros).
   - `ROCKETNET_RIO_OVERFLOW_TEST_HOOKS`: fake RIO completion overflow seam in `rocketnet_test_hooks` (`DEV=1` only).
   - `ROCKETNET_WINDOWS_LIFECYCLE_TRACK`: Windows handle/socket/thread lifecycle ledger in `rocketnet_dev` and `functional`.

---

## 3. Build Targets and `DEV=0` vs `DEV=1` Matrix (`CMakeLists.txt`)

| Target | `ROCKETNET_DEV=ON` (`DEV=1`) | `ROCKETNET_DEV=OFF` (`DEV=0`) |
| --- | --- | --- |
| `rocketnet` (static) | `ROCKETNET_DEV=1`, no `*_TEST_HOOKS` | `ROCKETNET_DEV=0`, zero test scaffolding or dev keys |
| `rocketnet_shared` (shared + import lib) | `ROCKETNET_DEV=1`, exports match `tools/exports.txt` | `ROCKETNET_DEV=0`, exports match `tools/exports.txt`, zero test scaffolding |
| `rocketnet_test_hooks` | Built (`STATIC EXCLUDE_FROM_ALL`) with `ROCKETNET_TEST_SEAMS=1` and unit `*_TEST_HOOKS=1` | **Not built** (target does not exist) |
| `rocketnet_test_support` | Links `PUBLIC rocketnet_test_hooks` | Links `PUBLIC rocketnet` (production static library) |
| `test` (`./bin/test`) | Runs full unit + seam + simulator suites against `rocketnet_test_hooks` | Runs the production-compatible `tests/test_unit_*.cpp` suites directly against production `rocketnet` |
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
1. **Header and Source Audit**: `sdk/rocketnet.h` exposes no test-scaffolding token; `sdk/rocketnet_test_seams.h` guards all 22 test-hook macros with `#if !ROCKETNET_DEV` `#error`; `sdk/rocketnet_connect_token.h` and `sdk/rocketnet_connection_reset.h` guard all deterministic token and reset-inventory scaffolding under `#if ROCKETNET_DEV`.
2. **Build-Tree Audit**: `build-dev0` is configured with `ROCKETNET_DEV=OFF`, produces no `rocketnet_test_hooks` archive, and compiles `rocketnet`, `rocketnet_shared`, `rocketnet_test_support`, and `test` without any `ROCKETNET_TEST_SEAMS` or `*_TEST_HOOKS` macro.
3. **Artifact Symbol and Byte Audit (`tools/check_exports.py`)**: `rocketnet` static archive, `rocketnet_shared` shared library, and Windows `rocketnet_import.lib` match `tools/exports.txt`, keep the static crypto namespace private, and contain zero development/test symbols, zero `_for_test` witnesses, zero deterministic token or reset-inventory symbols, and zero development secret key bytes.
4. **Negative Controls (`--red-controls`)**:
   - Compiler negative controls verify that enabling `ROCKETNET_TEST_SEAMS=1` or any of the 21 specialized test-hook macros with `ROCKETNET_DEV=0` fails compilation with `<MACRO> requires ROCKETNET_DEV=1`.
   - Symbol negative controls verify that referencing `rocketnet_write_connect_token_deterministic`, `rocketnet_reset_inventory_matches`, or `rocketnet_*_connection_reset_inventory_matches_for_test` with `ROCKETNET_DEV=0` fails compilation.
   - CMake negative controls verify that configuring with `-DROCKETNET_DEV=OFF` and `-DROCKETNET_TEST_SEAMS=1` or `-DCMAKE_CXX_FLAGS=-DROCKETNET_ACK_VISIT_TEST_HOOKS=1` fails at configure time.
