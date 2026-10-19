# Changelog

All notable changes to RocketNet are documented in this file.

## [1.0.0] - 2026-10-19

### Added

- **Public C99 Client and Server API ([sdk/rocketnet.h](sdk/rocketnet.h))**:
  - Reusable `rocketnet_client_t` and `rocketnet_server_t` objects: `create` and `create_sized` make them; the client connects with a connect token (`rocketnet_client_connect`) and disconnects (`rocketnet_client_disconnect`) any number of times; the server starts (`rocketnet_server_start`) and stops (`rocketnet_server_stop`) any number of times; `destroy` works from any state.
  - The frame loop: `update`, view delivery (`receive_message`, `release_message`), zero-copy sending (`start_message`, write in place, `finish_message`, or `abort_message`), then `send_packets`; delivery receipts (`receipts`), and monotonic counters and link statistics (`counter`, `stat`). Client indexes and destination ids are `uint64_t`.
  - Precedence-ordered configuration (`DEFAULT < STRUCT < ENV`) with strict validation that refuses out-of-range or unparseable overrides without clamping, plus ABI-versioned `_sized` entry points, `rocketnet_abi_version()`, and the layout queries `rocketnet_abi_type_size()`, `rocketnet_abi_field_count()`, `rocketnet_abi_field_offset()` and `rocketnet_abi_field_size()`.
  - Process-wide FFI-safe preformatted logging (`rocketnet_set_log_callback`), varargs logging (`rocketnet_log_function`), log levels (`ROCKETNET_LOG_LEVEL_NONE` through `ROCKETNET_LOG_LEVEL_SPAM`), and custom allocator hooks (`rocketnet_allocator`).
- **Normative Wire and Message Standards ([standard/TRANSPORT.md](standard/TRANSPORT.md), [standard/MESSAGES.md](standard/MESSAGES.md), [standard/RATIONALE.md](standard/RATIONALE.md))**:
  - Public-key connect tokens (Ed25519 signatures, X25519 key agreement, ChaCha20-Poly1305 AEAD), 6-byte prefilter header, stateless cookie gate with key rotation, forward-secret per-connection keys, replay protection, and bounded packet fragmentation and reassembly.
  - Eight reliability modes (`ROCKETNET_UNRELIABLE`, `ROCKETNET_UNRELIABLE_SEQUENCED`, `ROCKETNET_RELIABLE`, `ROCKETNET_RELIABLE_ORDERED`, `ROCKETNET_RELIABLE_SEQUENCED`, `ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT`, `ROCKETNET_RELIABLE_WITH_ACK_RECEIPT`, `ROCKETNET_RELIABLE_ORDERED_WITH_ACK_RECEIPT`) across up to 32 independent channels, with automatic promotion of oversize unreliable messages up to `max_message_bytes`.
  - Per-connection send budgets in kilobits per second, four priority classes with round-robin channel scheduling, and one fixed memory region per connection with zero allocations after create.
- **Runtime-Selected Server and Client Transports ([TRANSPORTS.md](TRANSPORTS.md), [docs/PLATFORMS.md](docs/PLATFORMS.md))**:
  - Eight server transports: `portable`, `gso`, `io_uring`, and `xdp` on Linux; `msgx` and `portable` on macOS; `iocp`, `rio`, and `portable` on Windows; and in-process `loopback` on all platforms.
  - Dedicated background client I/O threads on Linux, macOS, and Windows, plus optional `step_driven` caller-thread execution and development-build link simulation (`simulated_loss_percent`, `simulated_latency_ms`, `simulated_jitter_ms`, `simulated_duplicate_percent`).
- **Examples and Verification ([examples/README.md](examples/README.md))**:
  - Twelve self-contained C99 example programs (`hello`, `client_server`, `reliability`, `channels`, `ack_receipts`, `big_messages`, `budgets`, `stats`, `config_env`, `transports`, `game_loop`, `soak`).
  - Deterministic unit and functional test suites, Wycheproof and standard conformance vectors, pcapng corpus replay, libFuzzer targets, TLA+ formal specifications (`tla/`), and CBMC bounded model-checking proofs (`proofs/cbmc/`).
