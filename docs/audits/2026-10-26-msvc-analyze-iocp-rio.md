# Audit: Windows MSVC `/analyze` on IOCP and RIO Transports (2026-10-26)

- **Date:** 2026-10-27
- **Runner:** `captain` (native Windows 11 x64, `MSBuild version 18.5.0-release-26180-04+c5bc3ed94`)
- **Compiler / Toolset:** `Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64` (Visual Studio 18 2026 Community, `vcvars64.bat`)
- **CMake:** `cmake version 4.4.0-rc2` (`-G "Visual Studio 18 2026" -A x64`)
- **Base Commit:** `100803606f932ff822f5fbe5b6682811cf37412d` (in-scope files identical at `origin/main` `0dc33a950e75d182f76a6a28f06e588da7b4be65`)
- **In-Scope Transport Paths:**
  - `sdk/rocketnet_server_iocp_windows.cpp`
  - `sdk/rocketnet_server_iocp.h`
  - `sdk/rocketnet_server_rio_windows.cpp`
  - `sdk/rocketnet_server_rio.h`
  - `sdk/rocketnet_rio_layout.h`
  - `sdk/rocketnet_server_transport.cpp`

---

## 1. Executive Summary

Previously, the static-analysis triage (`docs/audits/2026-10-26-static-analysis-triage.md`) covered Linux `clang-tidy` and `cppcheck`, while Windows `rocketnet_server_iocp_windows.cpp` and `rocketnet_server_rio_windows.cpp` were only built with MSVC `/W4 /WX` in CI without `/analyze`.

This audit configures, builds, and executes the actual Windows IOCP and RIO server/client transport implementations and test suites on `captain` under MSVC `/analyze` in two separate `Release` build trees:

1. **`ROCKETNET_DEV=ON`** (`src-on`) — development build with test seams (`ROCKETNET_DEV=1`, `ROCKETNET_TEST_SEAMS=1`) and fault-injection hooks (`ROCKETNET_IOCP_TESTS=1`, `ROCKETNET_RIO_TESTS=1`, `ROCKETNET_RIO_OVERFLOW_TEST_HOOKS=1`).
2. **`ROCKETNET_DEV=OFF`** (`src-off`) — production build without test seams (`ROCKETNET_DEV=0`), plus the production-only `test_windows_transports_production` target that links `rocketnet` (`ROCKETNET_DEV=0`) directly.

### Summary of Findings

- **Compiler warnings (`C4xxx` under `/W4 /WX`):** **0** in both `ROCKETNET_DEV=ON` and `ROCKETNET_DEV=OFF` across all built library and test targets.
- **In-scope files with 0 MSVC `/analyze` warnings (`DEV=ON` and `DEV=OFF`):**
  - `sdk/rocketnet_server_iocp_windows.cpp` (**0** warnings across 1,733 lines)
  - `sdk/rocketnet_server_iocp.h` (**0** warnings)
  - `sdk/rocketnet_server_rio.h` (**0** warnings)
  - `sdk/rocketnet_rio_layout.h` (**0** warnings)
- **In-scope files with MSVC `/analyze` diagnostics:**
  - `sdk/rocketnet_server_rio_windows.cpp`: **1** site (`C6326` at line 1544 in test helper `rocketnet_server_rio_test_worker_handoff`).
  - `sdk/rocketnet_server_transport.cpp`: **5** sites in `ROCKETNET_DEV=ON` and **6** sites in `ROCKETNET_DEV=OFF` (`C6262` at lines 841 and 3218; `C6386` at lines 906 and 907; `C6326` at lines 1029, 1030, and in `DEV=OFF` only at line 1523).
- **Demonstrated defects in IOCP/RIO/server transport:** **0**. Every in-scope analyzer diagnostic is individually proved below to be either an intentional compile-time constant expression (`C6326`), an intraprocedural path-sensitivity false positive (`C6386`), or an intentional main-thread stack array required by the zero-allocation-after-create invariant (`C6262`).
- **Suppressions / `#pragma warning` / code edits required:** **0**. No `#pragma warning`, SAL suppression, disabled analyzer family, or transport behavior change was introduced.
- **Runtime verification of `/analyze`-built binaries:** All 24 cases in `test_iocp.exe` (`DEV=ON` and `DEV=OFF`), both production cases in `test_windows_transports_production.exe` (`DEV=OFF`), the full `test.exe` unit suite (`DEV=ON` and `DEV=OFF`), all 25 IOCP functional cases in `functional.exe` (`DEV=ON` and `DEV=OFF`), all 22 RIO functional cases in `functional.exe` (`DEV=ON` and `DEV=OFF`), and all 12 Windows transport family matrix cell runs (`iocp portable`, `portable iocp`, `rio portable` x 4 cases in `DEV=ON` and `DEV=OFF`) passed with exit code `0` and `0` skipped tests.

---

## 2. Toolchain, Configure Flags, and Build Matrix

### 2.1 Exact Toolchain Version (`src-on-cl-version.log` / `src-off-cl-version.log`)

- **SHA-256:** `de521aada19dbcd627794dfef449a123c0f0546ead3d1d874d22adc3e74a017e`

```text
Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36260 for x64
Copyright (C) Microsoft Corporation.  All rights reserved.

usage: cl [ option... ] filename... [ /link linkoption... ]
cmake version 4.4.0-rc2

CMake suite maintained and supported by Kitware (kitware.com/cmake).
```

### 2.2 Configure Commands and Flag Rationale

CMake's `Modules/Platform/Windows-MSVC.cmake` initializes `CMAKE_C_FLAGS_INIT="/DWIN32 /D_WINDOWS"` and `CMAKE_CXX_FLAGS_INIT="/DWIN32 /D_WINDOWS /EHsc"`. Passing `-DCMAKE_C_FLAGS` or `-DCMAKE_CXX_FLAGS` on the command line replaces those initializers, so `/DWIN32 /D_WINDOWS` and `/EHsc` are explicitly preserved. Because `CMakeLists.txt` adds `/W4 /WX` to every target, `/analyze:WX-` is passed so that:
1. Standard MSVC compiler warnings (`C4xxx`) remain fatal errors under `/W4 /WX` (proving `0` compiler warnings whenever a build exits `0`), and
2. MSVC `/analyze` (`PREfast` / EspXEngine) runs to completion across every translation unit and emits all `C6xxx` / `C26xxx` / `C28xxx` diagnostics into the retained build logs rather than halting at the first translation unit.

- **`ROCKETNET_DEV=ON` configure command (`src-on-configure.log`, SHA-256 `ae6e2eb7de0308fa0c83dbd151a708b7e958c6607c66bf7172944c95be89ab1e`, exit `0`):**
  ```bat
  cmake -B build -G "Visual Studio 18 2026" -A x64 -DROCKETNET_DEV=ON ^
    "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /analyze /analyze:WX-" ^
    "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /analyze /analyze:WX-"
  ```

- **`ROCKETNET_DEV=OFF` configure command (`src-off-configure.log`, SHA-256 `e2f344e41655587df137b1a8be0a1471b0c69e57ae5ca378dc8c9805c9f385b1`, exit `0`):**
  ```bat
  cmake -B build -G "Visual Studio 18 2026" -A x64 -DROCKETNET_DEV=OFF ^
    "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /analyze /analyze:WX-" ^
    "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /analyze /analyze:WX-"
  ```

### 2.3 Build Steps and Retained Build Logs

All builds were executed on `captain` under `winlock` using:

```bat
cmake --build build --config Release --parallel 16 --target <targets...>
```

| Configuration | Step Log | SHA-256 | Targets Built | Exit Code | Compiler Warnings (`C4xxx`) | Analyzer Warning Lines (`C6xxx`/`C26xxx`/`C28xxx`) | Errors |
|---|---|---|---|---:|---:|---:|---:|
| `ROCKETNET_DEV=ON` | `on-lib-build.log` | `61442b2d2467fb25d1f0a8a0896b6991de793453154f963338b8f4a1cacc10e9` | `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_iocp` | `0` | `0` | `83` | `0` |
| `ROCKETNET_DEV=ON` | `on-unit2-build.log` | `819f6cbafcd28c3c3e11bb92548c98fc3ed6014450469e90051ea11877d28748` | `test` (all unit test targets + `rocketnet_test_hooks`) | `0` | `0` | `215` | `0` |
| `ROCKETNET_DEV=ON` | `on-func2-build.log` | `480abccc09f23451391712d1ca33f3589a9ef74ee5476c0942b22956a376358d` | `test_iocp`, `functional`, `test_lifecycle` | `0` | `0` | `74` | `0` |
| `ROCKETNET_DEV=ON` | `on-tests-build.log` | `7bd847944055b30a8c24662a731df599c03f0602bb6ef431a8a2211b3e1ff8b1` | Combined `test`, `functional`, `test_lifecycle`, `test_iocp` | `0` | `0` | `198` | `0` |
| `ROCKETNET_DEV=OFF` | `off-lib-build.log` | `7153957a3ff61ad870298bb08bf994330c49c4274b6331a3d73e519c62b7880b` | `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_iocp`, `test_windows_transports_production` | `0` | `0` | `85` | `0` |
| `ROCKETNET_DEV=OFF` | `off-unit2-build.log` | `261fa767a3ceca7a84c45983372d8d156e673953e5629f4528d767bbb24d03da` | `test` (all unit test targets) | `0` | `0` | `79` | `0` |
| `ROCKETNET_DEV=OFF` | `off-func2-build.log` | `c9d32b925687c397a7ff10d45b4b02f59c542088c417d753b93411a438f6afb2` | `test_iocp`, `test_windows_transports_production`, `functional`, `test_lifecycle` | `0` | `0` | `74` | `0` |
| `ROCKETNET_DEV=OFF` | `off-tests-build.log` | `ba4dbcc6b4d88cf305092744532213885a69b53c63e2a206eea2d4feab250589` | Combined `test`, `functional`, `test_lifecycle`, `test_iocp`, `test_windows_transports_production` | `0` | `0` | `74` | `0` |

Note: Prior to `on-unit2-build.log`, `off-unit2-build.log`, `on-func2-build.log`, and `off-func2-build.log`, the non-library object directories (`test.dir/Release`, `functional.dir/Release`, etc.) were reset so that each dedicated unit and functional build recompiled and re-analyzed every translation unit it owns in full.

---

## 3. In-Scope IOCP, RIO, and Server Transport `/analyze` Inventory and Triage

### 3.1 Summary by In-Scope File

| File | `ROCKETNET_DEV=ON` Compiler Warnings | `ROCKETNET_DEV=OFF` Compiler Warnings | `ROCKETNET_DEV=ON` `/analyze` Sites | `ROCKETNET_DEV=OFF` `/analyze` Sites | Demonstrated Defects |
|---|---:|---:|---:|---:|---:|
| `sdk/rocketnet_server_iocp_windows.cpp` | `0` | `0` | `0` | `0` | `0` |
| `sdk/rocketnet_server_iocp.h` | `0` | `0` | `0` | `0` | `0` |
| `sdk/rocketnet_server_rio_windows.cpp` | `0` | `0` | `1` (`C6326`) | `1` (`C6326`) | `0` |
| `sdk/rocketnet_server_rio.h` | `0` | `0` | `0` | `0` | `0` |
| `sdk/rocketnet_rio_layout.h` | `0` | `0` | `0` | `0` | `0` |
| `sdk/rocketnet_server_transport.cpp` | `0` | `0` | `5` (`2x C6262`, `2x C6386`, `2x C6326` at 1 statement) | `6` (`2x C6262`, `2x C6386`, `3x C6326`) | `0` |
| **Total In-Scope** | **`0`** | **`0`** | **`6` unique sites** | **`7` unique sites** | **`0`** |

Both `sdk/rocketnet_server_iocp_windows.cpp` (compiled in `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `rocketnet_test_hooks`, `test_iocp`, and `test_windows_transports_production`) and the headers `sdk/rocketnet_server_iocp.h`, `sdk/rocketnet_server_rio.h`, and `sdk/rocketnet_rio_layout.h` are **100% clean** (`0` compiler warnings, `0` `/analyze` warnings) in both `ROCKETNET_DEV=ON` and `ROCKETNET_DEV=OFF`.

### 3.2 Individual Classification of Every In-Scope `/analyze` Finding

#### Finding 1 — `sdk/rocketnet_server_rio_windows.cpp(1544)` (`C6326`)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=ON`: `rocketnet_test_hooks.vcxproj`, `test_iocp.vcxproj` (`on-lib-build.log`, `on-unit2-build.log`)
  - `ROCKETNET_DEV=OFF`: `test_iocp.vcxproj` (`off-lib-build.log`, where `test_iocp` explicitly defines `ROCKETNET_DEV=1` and `ROCKETNET_RIO_TESTS=1`)
  - Note: Production and dev library targets (`rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_windows_transports_production`) emit **0** warnings on `sdk/rocketnet_server_rio_windows.cpp`.
- **Verbatim Tool Diagnostic:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_rio_windows.cpp(1544): warning C6326: Potential comparison of a constant with another constant. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\test_iocp.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_rio_windows.cpp:1541-1551` in `rocketnet_server_rio_test_worker_handoff`):**
  ```cpp
  // Every ready bank that fits the process view is exposed in one call, and
  // the rest wait in FIFO order. At current constants 4096 / 1024 = 4 >= 3,
  // so all three ready banks fit; the expression holds if either changes.
  const int fit = ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS / ROCKETNET_RIO_RECEIVES < ROCKETNET_RIO_RECEIVE_BANKS - 1 ?
      ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS / ROCKETNET_RIO_RECEIVES : ROCKETNET_RIO_RECEIVE_BANKS - 1;
  rocketnet_server_rio_receive_packets( rio );
  valid &= rio->process.num_packets == fit * ROCKETNET_RIO_RECEIVES &&
           rio->exposed_count == fit && rio->ready_count == ( ROCKETNET_RIO_RECEIVE_BANKS - 1 ) - fit &&
           rio->free_count == 0;
  ```
- **Classification:** **False positive (intentional compile-time constant expression).**
- **Evidence:** As the comment at lines 1541–1543 explicitly states, `fit` computes `min(ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS / ROCKETNET_RIO_RECEIVES, ROCKETNET_RIO_RECEIVE_BANKS - 1)` (`min(4096 / 1024, 4 - 1) = min(4, 3) = 3`) from header macros so that the unit test invariant remains accurate if either capacity macro is adjusted.

---

#### Finding 2 — `sdk/rocketnet_server_transport.cpp(841)` (`C6262`)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=ON`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `rocketnet_test_hooks`, `test`, `test_transport_control`
  - `ROCKETNET_DEV=OFF`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_transport_control`
- **Verbatim Tool Diagnostic:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(841): warning C6262: Function uses '25020' bytes of stack.  Consider moving some data to heap. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_transport.cpp:840-874` in `rocketnet_server_flush_control_buffer`):**
  ```cpp
  static void rocketnet_server_flush_control_buffer( struct rocketnet_server_transport_t * server_transport, bool shutdown )
  {
      rocketnet_assert( server_transport );
      rocketnet_assert( !rocketnet_server_transport_staged( server_transport ) );
      struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
      const int num_packets = send_buffer->num_packets;
      if ( num_packets <= 0 )
          return;
      ...
      struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_MAX_SEND_PACKETS];
      bool accepted[ROCKETNET_SERVER_MAX_SEND_PACKETS];
  ```
- **Classification:** **False positive / non-defect (intentional bounded stack allocation on the main update thread).**
- **Evidence:** `sends` (`1000 * 24 = 24,000` bytes) and `accepted` (`1000 * 1 = 1,000` bytes) total ~25 KB, which exceeds `/analyze`'s default 16 KB `C6262` advisory threshold but is ~2.4% of the standard 1 MiB Windows thread stack. `rocketnet_server_flush_control_buffer` is non-recursive and runs only on the caller's main/update thread. Moving `sends` or `accepted` to the heap at flush time is forbidden by RocketNet's zero-allocation-after-create invariant (enforced by `test_no_allocation_after_create*`).

---

#### Findings 3 & 4 — `sdk/rocketnet_server_transport.cpp(906)` & `(907)` (`C6386`)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=ON`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `rocketnet_test_hooks`, `test`, `test_transport_control`
  - `ROCKETNET_DEV=OFF`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_transport_control`
- **Verbatim Tool Diagnostics:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(906): warning C6386: Buffer overrun while writing to 'send_buffer->to':  the writable size is '20000' bytes, but 'index' bytes might be written.: Lines: 886, 887, 888, 898, 900, 902, 905, 906 [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(907): warning C6386: Buffer overrun while writing to 'send_buffer->packet_bytes':  the writable size is '4000' bytes, but 'index' bytes might be written.: Lines: 886, 887, 888, 898, 900, 902, 905, 906, 907 [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_transport.cpp:862, 884-909` in `rocketnet_server_flush_control_buffer` and `rocketnet_server_reserve_send_frame`):**
  ```cpp
  // In rocketnet_server_flush_control_buffer (line 862):
      send_buffer->num_packets = 0;

  // In rocketnet_server_reserve_send_frame (lines 898-908):
      struct rocketnet_server_send_buffer_t * send_buffer = &server_transport->send_buffer;
      rocketnet_assert( send_buffer->num_packets >= 0 && send_buffer->num_packets <= ROCKETNET_SERVER_MAX_SEND_PACKETS );
      if ( send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS )
      {
          rocketnet_server_flush_control_buffer( server_transport, shutdown );
      }
      rocketnet_assert( send_buffer->num_packets >= 0 && send_buffer->num_packets < ROCKETNET_SERVER_MAX_SEND_PACKETS );
      const int index = send_buffer->num_packets;
      send_buffer->to[index] = *to;
      send_buffer->packet_bytes[index] = 0;
  ```
- **Classification:** **False positive (intraprocedural analyzer limitation across helper call).**
- **Evidence:** `send_buffer->to` and `send_buffer->packet_bytes` both have capacity `ROCKETNET_SERVER_MAX_SEND_PACKETS` (`1000`, i.e., `20,000` bytes and `4,000` bytes). `send_buffer->num_packets` is initialized to `0` at transport creation (`line 369`) and is only incremented by `1` in `rocketnet_server_commit_send_frame` (`line 949`) after a reserve. When `send_buffer->num_packets >= ROCKETNET_SERVER_MAX_SEND_PACKETS` (`1000`) at line 900, `rocketnet_server_flush_control_buffer(server_transport, shutdown)` is called at line 902, which unconditionally sets `send_buffer->num_packets = 0` at line 862 (`num_packets >= 1000 > 0`, so the early return `if (num_packets <= 0)` is never taken). Because MSVC `/analyze` performs intraprocedural analysis and does not inline `rocketnet_server_flush_control_buffer`'s postcondition `send_buffer->num_packets == 0`, it retains the precondition `num_packets >= 1000` from the `if` branch and falsely concludes `index` can still be `>= 1000` at lines 906–907. In reality, `index` at line 905 is strictly in `0 .. ROCKETNET_SERVER_MAX_SEND_PACKETS - 1` (`0..999`).

---

#### Findings 5 & 6 — `sdk/rocketnet_server_transport.cpp(1029)` & `(1030)` (`C6326`)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=ON`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `rocketnet_test_hooks`, `test`, `test_transport_control`
  - `ROCKETNET_DEV=OFF`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_transport_control`
- **Verbatim Tool Diagnostics:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(1029): warning C6326: Potential comparison of a constant with another constant. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(1030): warning C6326: Potential comparison of a constant with another constant. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_transport.cpp:1022-1035` in `rocketnet_server_transport_memory_plan`):**
  ```cpp
      const int max_clients = config->max_clients;
      const int payload = ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES;
      const int send_Alloc = ROCKETNET_TRANSPORT_MAX_IN_FLIGHT_PACKETS * 2;
      const int recv_Alloc = ROCKETNET_TRANSPORT_MAX_IN_FLIGHT_PACKETS * 2;
      if ( max_clients <= 0 || max_clients > ROCKETNET_MAX_CLIENTS ||
           config->num_channels < 1 || config->num_channels > ROCKETNET_MAX_CHANNELS ||
           payload < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ||
           payload > ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES )
      {
          return false;
      }
  ```
- **Classification:** **False positive (intentional defensive constant-bounds guard).**
- **Evidence:** `payload` is initialized to the compile-time constant `ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES` and checked against `[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES]` alongside the runtime `config` fields so that `rocketnet_server_transport_memory_plan` enforces the same envelope check if `payload` or the header constants are ever parameterized.

---

#### Finding 7 — `sdk/rocketnet_server_transport.cpp(1523)` (`C6326`, `ROCKETNET_DEV=OFF` only)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=OFF`: `rocketnet.vcxproj`, `rocketnet_shared.vcxproj` (`off-lib-build.log`)
  - Note: Does not fire in `ROCKETNET_DEV=ON` (`rocketnet_dev`, `rocketnet_shared_dev`), where `ROCKETNET_TEST_SEAMS` is `1`.
- **Verbatim Tool Diagnostic:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-off\sdk\rocketnet_server_transport.cpp(1523): warning C6326: Potential comparison of a constant with another constant. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-off\build\rocketnet.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_transport.cpp:564-574` and `1520-1525`):**
  ```cpp
  // Lines 564-574:
  #if ROCKETNET_TEST_SEAMS
  static int rocketnet_server_test_create_send_capacity = 0;
  ...
  #else // #if ROCKETNET_TEST_SEAMS
  static const int rocketnet_server_test_create_send_capacity = 0;
  #endif // #if ROCKETNET_TEST_SEAMS

  // Lines 1520-1525 in rocketnet_server_transport_create_internal:
      const int default_send_capacity = server_transport->io.send_capacity > 0 ?
          server_transport->io.send_capacity : ROCKETNET_SERVER_MAX_SEND_PACKETS;
      server_transport->send_capacity = rocketnet_server_test_create_send_capacity > 0 ?
          rocketnet_server_test_create_send_capacity : default_send_capacity;
  ```
- **Classification:** **False positive (intentional compile-time constant seam stub in production builds).**
- **Evidence:** When `ROCKETNET_TEST_SEAMS == 0` (`rocketnet` and `rocketnet_shared` targets), `rocketnet_server_test_create_send_capacity` is defined as `static const int rocketnet_server_test_create_send_capacity = 0;` so the ternary `rocketnet_server_test_create_send_capacity > 0 ? ... : default_send_capacity` folds at compile time to `default_send_capacity` without duplicating `#if ROCKETNET_TEST_SEAMS` at the assignment site.

---

#### Finding 8 — `sdk/rocketnet_server_transport.cpp(3218)` (`C6262`)

- **Configurations / Targets:**
  - `ROCKETNET_DEV=ON`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `rocketnet_test_hooks`, `test`, `test_transport_control`
  - `ROCKETNET_DEV=OFF`: `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, `test_transport_control`
- **Verbatim Tool Diagnostic:**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\sdk\rocketnet_server_transport.cpp(3218): warning C6262: Function uses '29048' bytes of stack.  Consider moving some data to heap. [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\rocketnet.vcxproj]
  ```
- **Source Context (`sdk/rocketnet_server_transport.cpp:3217-3251` in `rocketnet_server_transport_send_packets_impl`):**
  ```cpp
  static void rocketnet_server_transport_send_packets_impl( struct rocketnet_server_transport_t * server_transport )
  {
      rocketnet_assert( server_transport );
      ...
      struct rocketnet_platform_send_t sends[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
      int send_client_index[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
      bool accepted[ROCKETNET_SERVER_PAYLOAD_RING_SIZE];
  ```
- **Classification:** **False positive / non-defect (intentional bounded stack allocation on the main update thread).**
- **Evidence:** `sends` (`1000 * 24 = 24,000` bytes), `send_client_index` (`1000 * 4 = 4,000` bytes), and `accepted` (`1000 * 1 = 1,000` bytes) total ~29 KB (~2.8% of the 1 MiB Windows thread stack) in the non-recursive main-thread send flush function. As in `rocketnet_server_flush_control_buffer`, heap allocation on the send path is prohibited by the zero-allocation-after-create invariant.

---

## 4. Windows Transport Test Targets and Out-of-Scope SDK `/analyze` Summary

### 4.1 Windows Transport Test Target (`tests/test_iocp.cpp(549)`)

Although `tests/test_iocp.cpp` is outside `PATHS`, `test_iocp.vcxproj` is built as part of the IOCP/RIO verification matrix and emits a single `/analyze` diagnostic:

- **Verbatim Tool Diagnostic (`on-lib-build.log` / `off-lib-build.log`):**
  ```text
  C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\tests\test_iocp.cpp(549): warning C28182: Dereferencing NULL pointer. 'batch->packet_data[0]' contains the same NULL value as 'view' did. : Lines: 532, 533, 534, 535, 536, 538, 539, 540, 541, 542, 543, 545, 546, 547, 549, 550, 552, 543, 545, 546, 547, 549 [C:\rowan-jobs\rn-windows-msvc-analyze-iocp-rio\src-on\build\test_iocp.vcxproj]
  ```
- **Source Context (`tests/test_iocp.cpp:542-554` in `test_receive_publication_never_waits`):**
  ```cpp
      const uint8_t * view = NULL;
      while ( !view && rocketnet_platform_time() < view_deadline )
      {
          rocketnet_server_iocp_receive_packets( iocp );
          rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( iocp );
          if ( batch->num_packets > 0 )
          {
              rocketnet_check( batch->num_packets == 1 && batch->packet_bytes[0] == 1 && batch->packet_data[0][0] == marker );
              view = batch->packet_data[0];
          }
          else rocketnet_platform_sleep( 0.001 );
      }
  ```
- **Classification:** **False positive.** When `batch->num_packets > 0`, `rocketnet_server_iocp_receive_packets` has populated `batch->packet_data[0] = buffer->data[i]`, which is the non-null address of an inline array `uint8_t data[ROCKETNET_IOCP_RECEIVE_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE]` inside `rocketnet_iocp_receive_buffer_t`. Because `/analyze` does not know `batch->packet_data[0]` is non-null when `batch->num_packets > 0`, it explores a hypothetical path where `batch->packet_data[0]` was `NULL` on iteration 1 (`view = batch->packet_data[0]` leaves `view == NULL`), re-enters `while (!view ...)`, and warns at `batch->packet_data[0][0]` on iteration 2.

### 4.2 Out-of-Scope SDK Files Summary

For completeness across `rocketnet`, `rocketnet_dev`, `rocketnet_shared`, `rocketnet_shared_dev`, and `rocketnet_test_hooks`, the remaining `/analyze` diagnostics in out-of-scope `sdk/` files are:

| Out-of-Scope File | Line | Warning | Summary |
|---|---:|---|---|
| `sdk/rocketnet_api.cpp` | `184` | `C6011` | `Dereferencing NULL pointer 'end'` after `rocketnet_assert(end)` in dev-only `rocketnet_network_simulator_parse_spec` (`strtod` `end` pointer is never NULL). |
| `sdk/rocketnet_client_transport.cpp` | `1065` | `C6285` / `C6326` | Constant comparison on `ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES` bounds in `rocketnet_client_transport_memory_plan` (mirrors `rocketnet_server_transport.cpp(1029-1030)`). |
| `sdk/rocketnet_config.cpp` | `782`, `784` | `C6235` / `C6326` | Constant comparison on compile-time protocol capacity constants. |
| `sdk/rocketnet_driver_builtin.cpp` | `801` | `C6262` | `26444` bytes of stack in `rocketnet_test_hooks`. |
| `sdk/rocketnet_encryption_map.cpp` | `54` | `C6011` | `Dereferencing NULL pointer 'end'` after `rocketnet_assert(end != NULL)` in test-only helper. |
| `sdk/rocketnet_platform.cpp` | `5571` | `C6262` | `65768` bytes of stack (`uint8_t buffer[65536]`) in test-only raw UDP drain helper `rocketnet_platform_test_udp_drain`. |
| `sdk/rocketnet_tests.cpp` | various | `C6262` (38), `C6294` (2), `C6001` (1) | Embedded `ROCKETNET_DEV=ON` self-test fixtures. |

---

## 5. Runtime Execution Verification (`ROCKETNET_DEV=ON` and `ROCKETNET_DEV=OFF`)

All tests were executed on `captain` under `winlock` using the `/analyze`-built binaries in `src-on/bin/Release/` and `src-off/bin/Release/`.

### 5.1 Dedicated IOCP/RIO Executables and Unit Suite

| Configuration | Binary | Retained Log | SHA-256 | Result | Exit Code |
|---|---|---|---|---|---:|
| `ROCKETNET_DEV=ON` | `bin/Release/test_iocp.exe` | `src-on-func-cells/test_iocp.log` | `5b04ddcf78258e8b7b3aee10d7ffedc013a6e0008f979883ab9e95ccef5010a3` | 24/24 IOCP & RIO cases passed | `0` |
| `ROCKETNET_DEV=OFF` | `bin/Release/test_iocp.exe` | `src-off-func-cells/test_iocp.log` | `5b04ddcf78258e8b7b3aee10d7ffedc013a6e0008f979883ab9e95ccef5010a3` | 24/24 IOCP & RIO cases passed | `0` |
| `ROCKETNET_DEV=OFF` | `bin/Release/test_windows_transports_production.exe` | `src-off-func-cells/test_windows_transports_production.log` | `98c89b37d5710ceb811e315562015f64a324e383da77655c05bc6b8f07fae8f3` | `test_iocp_production_exchange_shutdown` (`features=3`), `test_rio_production_exchange_shutdown` (`*** PRODUCTION WINDOWS TRANSPORTS PASSED ***`) | `0` |
| `ROCKETNET_DEV=ON` | `bin/Release/test.exe` | `src-on-unit.log` | `54136fe080ef874e394f5161ef1ee39c1e03651d31ed9bf11274a84c0c78ba97` | `*** ALL TESTS PASSED ***` (29s) | `0` |
| `ROCKETNET_DEV=OFF` | `bin/Release/test.exe` | `src-off-unit.log` | `6077632fd11012b3e5e6dcf3727adc83a3092d73a957c1750238b95f7d251744` | `*** ALL TESTS PASSED ***` (7s) | `0` |

### 5.2 IOCP and RIO Functional Cases (`functional.exe --run <case>`)

Every IOCP and RIO functional test case registered in `functional/` was executed in both `ROCKETNET_DEV=ON` and `ROCKETNET_DEV=OFF`:

- **25 IOCP functional cases** (`src-on-func-iocp/*.log` and `src-off-func-iocp/*.log` — **25/25 PASS**, `rc=0`, `0` skipped in both `DEV=ON` and `DEV=OFF`):
  - `test_iocp_client_transport_wire_dual_stack`
  - `test_iocp_client_transport_wire_ipv4`
  - `test_iocp_client_transport_wire_ipv6`
  - `test_iocp_transport_server_io_pin`
  - `test_iocp_transport_server_uso_uro_probe`
  - `test_iocp_transport_server_uso_wire`
  - `test_iocp_transport_windows_client_create_destroy_1000`
  - `test_iocp_transport_windows_connect_disconnect_1000`
  - `test_iocp_transport_windows_connreset_hard_killed_client_ipv4`
  - `test_iocp_transport_windows_connreset_hard_killed_client_ipv6`
  - `test_iocp_transport_windows_connreset_hard_killed_client_plain_ipv4`
  - `test_iocp_transport_windows_connreset_hard_killed_client_plain_ipv6`
  - `test_iocp_transport_windows_connreset_send_to_closed_port_ipv4`
  - `test_iocp_transport_windows_connreset_send_to_closed_port_ipv6`
  - `test_iocp_transport_windows_connreset_send_to_closed_port_plain_ipv4`
  - `test_iocp_transport_windows_connreset_send_to_closed_port_plain_ipv6`
  - `test_iocp_transport_windows_init_term_1000`
  - `test_iocp_transport_windows_server_create_destroy_1000`
  - `test_iocp_transport_windows_server_drops_oversize_datagram_ipv4`
  - `test_iocp_transport_windows_server_drops_oversize_datagram_ipv6`
  - `test_iocp_transport_windows_server_drops_runt_and_garbage_ipv4`
  - `test_iocp_transport_windows_server_drops_runt_and_garbage_ipv6`
  - `test_iocp_transport_windows_server_restart_1000`
  - `test_session_reconnect_isolation_iocp`
  - `test_windows_server_counts_oversize_as_prefilter_too_large_iocp`

- **22 RIO functional cases** (`src-on-func-rio/*.log` and `src-off-func-rio/*.log` — **22/22 PASS**, `rc=0`, `0` skipped in both `DEV=ON` and `DEV=OFF`):
  - `test_rio_transport_server_io_pin`
  - `test_rio_transport_server_probe`
  - `test_rio_transport_server_send_full`
  - `test_rio_transport_server_wire_dual_stack`
  - `test_rio_transport_server_wire_ipv4`
  - `test_rio_transport_server_wire_ipv6`
  - `test_rio_transport_windows_client_create_destroy_1000`
  - `test_rio_transport_windows_connect_disconnect_1000`
  - `test_rio_transport_windows_connreset_hard_killed_client_ipv4`
  - `test_rio_transport_windows_connreset_hard_killed_client_ipv6`
  - `test_rio_transport_windows_connreset_send_to_closed_port_ipv4`
  - `test_rio_transport_windows_connreset_send_to_closed_port_ipv6`
  - `test_rio_transport_windows_init_term_1000`
  - `test_rio_transport_windows_server_create_destroy_1000`
  - `test_rio_transport_windows_server_drops_oversize_datagram_ipv4`
  - `test_rio_transport_windows_server_drops_oversize_datagram_ipv6`
  - `test_rio_transport_windows_server_drops_runt_and_garbage_ipv4`
  - `test_rio_transport_windows_server_drops_runt_and_garbage_ipv6`
  - `test_rio_transport_windows_server_restart_1000`
  - `test_session_reconnect_isolation_rio`
  - `test_windows_server_counts_oversize_as_prefilter_too_large_rio`
  - `test_windows_server_counts_runt_as_prefilter_too_short_rio`

### 5.3 Windows Transport Family Matrix Cells (`iocp portable`, `portable iocp`, `rio portable`)

Per `.github/workflows/ci.yml` (`windows-transport-families`), the three Windows transport family cells (`ROCKETNET_SERVER_TRANSPORT` x `ROCKETNET_CLIENT_TRANSPORT` in `iocp portable`, `portable iocp`, `rio portable`, exported from WSL into Win32 via `WSLENV=ROCKETNET_SERVER_TRANSPORT:ROCKETNET_CLIENT_TRANSPORT`) were run across all 4 transport family cases in both `ROCKETNET_DEV=ON` (`src-on-func-cellsonly/*.log`) and `ROCKETNET_DEV=OFF` (`src-off-func-cellsonly/*.log`):

| Cell (`SERVER` / `CLIENT`) | Case | `DEV=ON` / `DEV=OFF` Log SHA-256 | Exit Code |
|---|---|---|---:|
| `iocp` / `portable` | `test_loopback_transport_udp` | `9c42b409e694a4e15b59a7d2f981874308e4a32de2ff9b95df5aedd846e69e85` | `0` |
| `iocp` / `portable` | `test_loopback_transport_ipv6` | `e4913baa184464ee5ae521e33646e42115f9150ef5767658cda01f4ca73272dd` | `0` |
| `iocp` / `portable` | `test_transport_dual_stack_ipv4_client` | `04382886dce0ca6c6ff5c1946a4fae9fc488db59dd946f57e84c0e4048ec95f2` | `0` |
| `iocp` / `portable` | `test_transport_env_pin_is_honoured` | `cbebebd2fe96da705d51d22177a9771e6b239e5725ad9798f09787122930eb28` | `0` |
| `portable` / `iocp` | `test_loopback_transport_udp` | `41ba6a91461531069c541c3dabfe8a3b498d95eb8439c33b9acce567a871e777` | `0` |
| `portable` / `iocp` | `test_loopback_transport_ipv6` | `c4e0e025e72a5395e73614b2f10e972d00e0bbf2622e13b6374362eee073d3b5` | `0` |
| `portable` / `iocp` | `test_transport_dual_stack_ipv4_client` | `ac279be9309277cfc1f19a31073cb2e51a848136da2449654f045b1119c58263` | `0` |
| `portable` / `iocp` | `test_transport_env_pin_is_honoured` | `4e36c7531b72c181463a1e48a06aec3e28b88983a1579992001b4658cfa3296c` | `0` |
| `rio` / `portable` | `test_loopback_transport_udp` | `273baaf0a5876642c82d3d8dfd520bfe83ff91b945453b88a051fcb7e162e938` | `0` |
| `rio` / `portable` | `test_loopback_transport_ipv6` | `b2843115dd433dc35614b576f0550084340a632ea4bab4e8f1a4f2ddc6e28f4e` | `0` |
| `rio` / `portable` | `test_transport_dual_stack_ipv4_client` | `5a8f53e65b32ca779d2af0fe06d2bc1e7501c75086aa812d9315531783b98579` | `0` |
| `rio` / `portable` | `test_transport_env_pin_is_honoured` | `a8f6037ff5d828ecba8d61b86ce621d0abdab1ecb3d8936db8c504ec278e0fd2` | `0` |
