# ABI Evolution: Sized Configs

This document is the rule for growing a public struct of `sdk/rocketnet.h`
without breaking a caller compiled against an older header. It is the document
`docs/ERRORS-AND-LOGGING.md` refers to as `docs/ABI-EVOLUTION.md`.

The SDK's ABI revision is its major version (`ROCKETNET_VERSION_MAJOR`), and
the shared library's soname follows it (`CMakeLists.txt`,
`ROCKETNET_ABI_SOVERSION`). A change this document admits is additive: it keeps
the major version and adds exports to `tools/exports.txt`. A change it does not
admit (a field moved, removed, resized or retyped) is an incompatible change
and bumps the major version.

The v1.0.0-preview-1 API is provisional. It may change after feedback.

### Receipt reads after preview-1

`rocketnet_client_receipts` and `rocketnet_server_receipts` now return the
number of available outcomes, rather than the number copied. Their signatures
and receipt layout are unchanged, but callers must iterate over at most
`min(returned_count, capacity)` entries. A NULL output with zero capacity
queries the count. If it exceeds capacity, read again into a larger buffer
before the next update. Reads do not consume outcomes. Ring overflow still
drops the oldest outcome and increments `receipts_dropped`; the returned count
includes only retained outcomes. Rebuild preview-1 callers with this contract
before linking to the updated library.

## 1. The Rule

1. **Each public config struct carries its own size.** The first field of
   `rocketnet_client_config_t` and `rocketnet_server_config_t` is
   `size_t config_bytes`, the `sizeof` of the struct the caller compiled
   against. The header's inline `rocketnet_client_default_config` and
   `rocketnet_server_default_config` set it to `sizeof( *config )` and then
   call the library's `rocketnet_client_default_config_internal` or
   `rocketnet_server_default_config_internal`, which writes the defaults. The
   caller's idiom is two lines:

   ```c
   struct rocketnet_client_config_t config;
   rocketnet_client_default_config( &config );
   ```

2. **Zero or too small is refused.** A `config_bytes` below
   `sizeof( size_t )`, too small to hold the field itself (0 is a zeroed config
   that skipped default_config), is refused with
   `ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED` and one log line, before any other
   check. Create writes the code through its status out-parameter and returns
   `NULL`; required_memory returns 0; the `_internal` default writes nothing.
3. **Larger is read as far as the library knows.** A caller compiled against a
   newer header than the library passes a larger `config_bytes`: the library
   reads the fields it knows and ignores the tail.
4. **Smaller is read as a prefix, with defaults after it.** A caller compiled
   against an older header passes a smaller `config_bytes`: the library reads
   the caller's fields as it set them and takes the defaults for every field
   past them.
5. **The library never reads or writes past `config_bytes`.** Every byte the
   library reads from or writes to the caller's struct is below the caller's
   `config_bytes`. A `NULL` config, where a call accepts one, means the
   defaults.

This document names public client and server types and entry points from
`sdk/rocketnet.h`. Where it refers to internal transport or message-layer
details, those names live in `sdk/rocketnet_layers.h`; a game does not call
them.

## 2. Growing a Config Struct

1. **A struct only grows at its end.** A field is never moved, removed,
   resized or retyped.
2. **A new field starts at or after the previous revision's `sizeof`.** The
   previous revision's struct may end in trailing padding, and an older
   caller's `config_bytes` covers that padding. A new field placed inside it
   would be read from an older caller's uninitialized padding as if it were
   set. Starting the new field at or past the old `sizeof` means an older
   caller's prefix never reaches it, so the field takes its default (rule 4).
3. **A field added later has a default that equals the old behaviour.** A
   caller that never sets it, and a caller compiled before it existed, both
   get exactly the behaviour of the revision before. One field is a
   deliberate exception: `loopback_hub` (both configs) defaults to `NULL`,
   and a list naming `loopback` with a `NULL` hub is refused with
   `ROCKETNET_STATUS_LOOPBACK_HUB_MISSING`. The loopback transport no longer
   keeps a process-wide endpoint table to fall back on, so a caller compiled
   before the field must set it to use loopback; every other transport is
   unchanged for that caller.

The steps for adding a field:

1. Add the field at the end of the struct, starting at or after the previous
   revision's `sizeof`, with a default that equals the old behaviour.
2. Write its default in the library's default fill, and its environment
   variable beside the field in the header, as for every other field.
3. Add a case to `tests/test_unit_sized_config.cpp` that passes the previous
   revision's size as `config_bytes` and checks the new field reads its
   default.
4. Leave the major version unchanged.

## 3. The Implementation

The `_internal` defaults, the creates and the required-memory calls of both
endpoints read `config_bytes` first through one check in
`sdk/rocketnet_api.cpp` (`rn_public_config_bytes`): below `sizeof( size_t )`
is refused with `ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED`, and otherwise the
bytes used are the smaller of the caller's `config_bytes` and the library's
`sizeof`. A default copies that many bytes of the library's defaults into the
caller's struct, after `config_bytes`, which it keeps; a create or
required-memory call fills a full struct with the defaults and copies the
caller's bytes over them, then runs on the full struct. The server config
contains the backend's public key and protocol id; it never contains a secret
key. Each server object generates its own X25519 keypair during create,
exposes its public key through `rocketnet_server_public_key`, and wipes its
secret key when destroyed.

The internal transport and message configs keep oldest-first known-size lists
in the internal layer header, checked in `sdk/rocketnet_abi.cpp`; the public
configs need no such list, because rules 3 and 4 accept every size of at least
`sizeof( size_t )`.

## 4. The Other Public Structs

| Struct | Direction | Carries its size |
| :--- | :--- | :--- |
| `rocketnet_client_config_t` | in | yes, `config_bytes` |
| `rocketnet_server_config_t` | in | yes, `config_bytes` |
| `rocketnet_client_stats_t` | out | no |
| `rocketnet_server_stats_t` | out | no |
| `rocketnet_server_client_stats_t` | out | no |
| `rocketnet_receipt_t` | out, in an array | no |
| `rocketnet_message_batch_t` | out | no |

The structs marked "no" carry no size, so a
field added to one of them is an incompatible change and bumps the major
version. `rocketnet_receipt_t` is also the element of an array
the library hands out, so its stride is part of the ABI.

## 5. The lifecycle API in ABI 1

The ABI revision is 1 at v1.0.0-preview-1 (soname `librocketnet.so.1`).
Client creation returns a disconnected object and connection begins with
`rocketnet_client_connect`. Server creation returns a stopped object;
`rocketnet_server_start`, `rocketnet_server_stop` and
`rocketnet_server_running` control its transport. Client indices are `int`;
only `rocketnet_server_start_message` takes a `uint64_t` destination id, a
client index today and a flow id (`ROCKETNET_FLOW_ID_BIT` set) later. Message
calls use the verb-first names `start_message`, `finish_message`,
`abort_message`, `receive_messages` and `send_packets`; the server's finish and
abort act on its one pending start and take no destination. The endpoint-wide
receive queue uses `rocketnet_message_batch_t` and needs no release call.

`rocketnet_client_create( context, config, &status )` and
`rocketnet_server_create( context, address, NULL, config, &status )` take a nullable
status out-parameter, so a NULL result has a specific `rocketnet_status_t`.
Each makes one block through the allocator; `rocketnet_client_required_memory`
and `rocketnet_server_required_memory` give the lifecycle allocator peak,
including planned built-in transport allocations at connect or start and
repeated cycles with unchanged configuration and environment overrides.
Development builds include the link simulator. OS-owned memory and custom
transport allocations are excluded. The process-wide
allocator, assert function, log function, log level, quiet flag and thread
priority switch are installed through `rocketnet_init`,
`rocketnet_assert_function`, `rocketnet_log_function`, `rocketnet_log_level`,
`rocketnet_quiet` and `rocketnet_thread_priority`. Public enum values and
exported symbols follow the append-only rules: a new status code takes the
next negative value, and no published value changes.

The backend signing keypair is separate from the server's X25519 keypair.
Only the backend public key is supplied in `rocketnet_server_config_t`; the
backend keeps its signing secret and mints tokens for the server public key.
Development builds expose `ROCKETNET_DEV_BACKEND_PUBLIC_KEY` and
`rocketnet_create_dev_connect_token` for local use. A release server refuses
that published development key.

## 6. Public enums are int-sized, and zero is a named receipt status

The library and its callers are built without `-fshort-enums`. Every public
enum in `sdk/rocketnet.h` is the size of `int`. A static assert in
`sdk/rocketnet.h` fails a translation that shortens one.

`rocketnet_receipt_status_t` names zero. `ROCKETNET_RECEIPT_STATUS_NONE` is 0,
`ROCKETNET_RECEIPT_STATUS_ACKED` is 1, and `ROCKETNET_RECEIPT_STATUS_LOSS` is -1.
A zero-filled receipt reads NONE. The library writes ACKED or LOSS.

`rocketnet_disconnect_reason_t` keeps its values. NONE is 0 and each reason
is negative, -1 to -8. `rocketnet_disconnect_reason_name` returns a name for
every value in the enum. It returns `"UNKNOWN"` for any other value. The same rule
holds for `rocketnet_status_name`, `rocketnet_log_level_name` and
`rocketnet_client_state_name`.
