# ABI Evolution: Sized Configs

This document is the rule for growing a public struct of `sdk/rocketnet.h`
without breaking a caller compiled against an older header, and the record of
every public struct's size at each revision. It is the document
`docs/ERRORS-AND-LOGGING.md` refers to as `docs/ABI-EVOLUTION.md`.

The SDK's ABI revision is `ROCKETNET_ABI_VERSION` (`sdk/rocketnet.h`), and the
shared library's soname follows it (`CMakeLists.txt`, `ROCKETNET_ABI_SOVERSION`).
A change this document admits is additive: it keeps `ROCKETNET_ABI_VERSION`
and adds exports to `tools/exports.txt`. A change it does not admit (a field
moved, removed, resized or retyped) is an incompatible change and bumps
`ROCKETNET_ABI_VERSION`.

## 1. The Rule

1. **Each public config struct is passed with its size.** Beside each config
   struct's default and create entry points there is a sized entry point that
   also takes `config_bytes`, the `sizeof` of the struct the caller was
   compiled against (section 3; section 7 names the config-reading queries
   that take their sized forms when a struct first grows).
2. **A struct only grows at its end.** A field is never moved, removed,
   resized or retyped; a new field goes after the last field of the previous
   revision, and the struct's `sizeof` grows (a field that fits inside the
   old trailing padding is not admitted: the size must change, or the library
   cannot tell the revisions apart).
3. **A field added later has a default that equals the old behaviour.** A
   caller that never sets it, and a caller compiled before it existed, both
   get exactly the behaviour of the revision before.
4. **An unknown larger size is refused.** A caller compiled against a newer
   header than the library is refused with a log line naming the struct and
   both sizes: the library cannot honour fields it does not have, and it never
   silently drops them. Any other size the struct never had (zero, a size
   between revisions, nonsense) is refused the same way.
5. **A smaller known size reads the prefix and defaults the rest.** A caller
   compiled against an older revision gets its own fields as it set them and
   the defaults (rule 3) for everything after.

A refusal reads nothing from the caller's struct and writes nothing to it. A
`NULL` config keeps its unsized meaning (the defaults), whatever size rides
with it.

The unsized entry points are the sized ones at revision 1. Revision 1 is the
whole of today's struct, so `rocketnet_client_default_config`,
`rocketnet_server_default_config` and `rocketnet_server_dev_config` write every
field, and the unsized creates, validations and required-memory queries read
every field, the four `simulated_*` controls included
(`rocketnet_client_default_config`, `rocketnet_client_create`,
`rocketnet_server_default_config`, `rocketnet_server_dev_config` and
`rocketnet_server_create` in `sdk/rocketnet_api.cpp`). When a config struct grows past revision 1, the unsized forms
keep reading and writing exactly the revision 1 bytes (so a binary built
against revision 1 keeps working against a newer library), and a caller that
wants a later field uses the sized forms.

This document names public client and server types and entry points from
`sdk/rocketnet.h`. Where it refers to internal transport or message-layer
details, those names live in `sdk/rocketnet_layers.h`; a game does not call
them.

## 2. The Choice: A Size Argument, Not a Leading Size Field

Two forms are common for this. The SDK uses the size argument.

| | Size argument (chosen) | Leading size field |
| :--- | :--- | :--- |
| Today's layout | Unchanged: every config struct keeps its revision 1 layout | Changes: a `size` member goes first and every offset moves, an incompatible change at revision 1 |
| Existing callers | Compile and run unchanged on the unsized entry points | Must be rebuilt, and must set the field |
| Forgetting it | Not possible on a sized call: the size is a parameter | A zeroed or stale `size` reads as an unknown size, or worse, as a wrong known one |
| C | `rocketnet_client_default_config_sized( &config, sizeof( config ) )` | `config.size = sizeof( config );` before every use |
| C#, other FFI | The binding passes `Marshal.SizeOf<T>()` (or the language's equivalent) as one more integer argument; the struct marshals as a plain blittable struct | The binding sets a field the marshaller does not know is special; a struct copied or defaulted by the runtime carries `size = 0` |
| One struct, two calls | The default call and the create call each carry the size, so neither depends on what the other left in the struct | The size travels in the struct, so a struct defaulted by one library revision and passed to another carries the first one's size |

The size argument keeps revision 1 binary-identical, so `ROCKETNET_ABI_VERSION`
stays 1, and it keeps the struct a plain value type for every FFI binding. Its
cost is a second entry point per config call; the leading field's cost is an
incompatible change now and a field every caller must remember forever.

## 3. The Implementation

Every sized entry point makes one of two calls (`sdk/rocketnet_abi.cpp`):

- `rocketnet_config_write_sized`, behind each sized default: when
  `config_bytes` is a known size, it copies the first `config_bytes` bytes of
  the library's defaults into the caller's struct (rule 5, and rule 3 for the
  fields the caller has); otherwise it refuses and writes nothing (rule 4).
- `rocketnet_config_read_sized`, behind each sized create: it fills a full
  struct with the defaults, and when `config_bytes` is a known size it copies
  the caller's first `config_bytes` bytes over them (rule 5); otherwise it
  refuses (rule 4). The create then runs on the full struct, exactly as the
  unsized create. A server config holds the secret key, so its stack copy is
  wiped on every path.

The sized entry points, each declared beside its unsized form:

| Struct | Sized defaults | Sized queries | Sized creates |
| :--- | :--- | :--- | :--- |
| `rocketnet_client_config_t` | `rocketnet_client_default_config_sized` | `rocketnet_client_validate_config_sized`, `rocketnet_client_required_memory_sized` | `rocketnet_client_create_sized` |
| `rocketnet_server_config_t` | `rocketnet_server_default_config_sized`, `rocketnet_server_dev_config_sized` | `rocketnet_server_validate_config_sized`, `rocketnet_server_required_memory_sized` | `rocketnet_server_create_sized` |

Each sized create takes one config struct and its size, checked by the rule.
A sized create refuses an unknown size before any other check: it
returns `NULL` with the log line, and the API misuse count does not move (a
size mismatch is a build mismatch, not a NULL misuse). The sized defaults
return `enum rocketnet_status_t` (`ROCKETNET_STATUS_OK` on success, or
`ROCKETNET_STATUS_INVALID_ARGUMENT` when `config` is `NULL` or `config_bytes`
is not a known revision size).

The public client and server configs each have one known size,
`ROCKETNET_CLIENT_CONFIG_BYTES_1` and `ROCKETNET_SERVER_CONFIG_BYTES_1`
(both in `sdk/rocketnet.h`; checked by `rn_public_config_size_known` in `sdk/rocketnet_api.cpp`).
The internal transport and message configs keep oldest-first known-size lists
in the internal layer header, so the library and a caller read one list. Each revision's size is written as the
end of that revision's last field rounded up to that revision's alignment
(`ROCKETNET_CONFIG_REVISION_BYTES`), never as `sizeof`, so the entry for an old
revision keeps its value when the struct grows past it.

## 4. Every Public Struct, Its Sizes per Revision

Sizes are bytes on every supported target (64-bit Linux x86_64 and s390x,
macOS arm64, Windows x86_64; `docs/PLATFORMS.md`). On any target,
`rocketnet_abi_type_size` is the authority.

| Struct | Direction | Sized |
| :--- | :--- | :--- |
| `rocketnet_client_config_t` | in | yes |
| `rocketnet_server_config_t` | in | yes |
| the client stat enum | out | no |
| the server stat enum | out | no |
| the per-client stat enum | out | no |
| `rocketnet_packet_context_t` | in and out | no |
| `rocketnet_frame_sync_stats_t` | out | no |
| `rocketnet_receipt_t` | out, in an array | no |

The client and server configs carry the message fields directly. A field added
to one goes at the end of the struct; a caller compiled against an older header
keeps the old size and reads the old prefix. The size of each revision is in the
public header and in `rocketnet_abi_type_size`, which is the
authority for a given build; this document does not repeat the numbers.

The client and server configs have one revision. Revision 1 is the whole
struct: the client config through the four development link-simulator
controls, the server config through `second_public_address`, which follows
them. `ROCKETNET_CLIENT_CONFIG_BYTES_1` ends at `simulated_duplicate_percent`
and `ROCKETNET_SERVER_CONFIG_BYTES_1` ends at `second_public_address` (both in
`sdk/rocketnet.h`). A `static_assert` requires each to equal the struct's
`sizeof` (`sdk/rocketnet_abi.cpp:6-7`).
`ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED` is appended immediately
before `ROCKETNET_STATUS_COUNT`.

Revision 1 is the layout of the first release that carries the sized entry
points. `time_callback`, the `simulated_*` controls and `second_public_address`
join the configs before that release, so they are part of revision 1, not
revisions of their own; a revision is added only for a layout that a release
has already shipped with the sized entry points.

The structs marked "no" have no sized entry points, so their revision 1 layout
is frozen: a field added to one of them is an incompatible change until its
calls get sized forms by this rule (for an out struct, the library writes only
the caller's known prefix). `rocketnet_receipt_t` is also the element
of an array the library hands out, so its stride is part of the ABI and it
does not grow.

## 5. How the Layout Queries Pin It

`sdk/rocketnet_abi.cpp` answers `rocketnet_abi_type_size`,
`rocketnet_abi_field_count`, `rocketnet_abi_field_offset` and
`rocketnet_abi_field_size` for the first eight structs above, with offsets and
sizes taken from the compiler. A binding compares them against its own
declarations before its first call. They pin this rule three ways:

- `static_assert`s in `sdk/rocketnet_abi.cpp` require each config's revision 1
  size formula to equal the compiler's `sizeof`, so a formula wrong for a
  target fails the build there (`sdk/rocketnet_abi.cpp:6-7` for the public
  client and server configs).
- `public_config_default_fills_every_field` and
  `public_config_plain_create_applies_simulation` (`test_unit_sized_config.cpp`)
  require the unsized defaults to write the same whole struct as the sized
  defaults at `sizeof`, and the unsized creates to carry the `simulated_*`
  controls to the transport.
- `sized_config_accepts_every_known_size` (`test_unit_sized_config.cpp`)
  requires each known-size list to be strictly growing and to end at the size
  `rocketnet_abi_type_size` reports, so a revision added without its size, or
  a field that does not grow the struct, fails the suite.
- The queries report the library's current revision, so a binding compiled
  against an older header reads a larger size and larger field count than its
  own: the sized entry points are then the ones it calls, with its own
  `sizeof`. `tools/abi_mismatch.cpp` is the deliberately incompatible library
  for checking that a binding refuses a mismatch; it builds the queries alone
  (`ROCKETNET_ABI_QUERIES_ONLY`), without the sized entry points.

## 6. Adding a Field to a Config Struct

1. Add the field at the end of the struct (section 4 for the client config),
   with a default that equals the old behaviour.
2. Add `ROCKETNET_<STRUCT>_BYTES_<N>` for the new revision and add it to the
   struct's known sizes (for the public configs, the one-entry lists at
   `rn_client_config_known_bytes` and `rn_server_config_known_bytes` in `sdk/rocketnet_api.cpp`); the earlier entries keep their formulas.
3. Keep the unsized entry points on the revision 1 prefix: they copy the
   caller's revision 1 bytes over the defaults, as the sized read does for
   that size.
4. Add a row for the revision to section 4, and a case to
   `test_unit_sized_config.cpp` for the old size.
5. Leave `ROCKETNET_ABI_VERSION` unchanged.

## 7. Sized Queries

The config-reading queries have sized forms alongside their revision-1
unsized forms. The sized validation and required-memory calls accept the
config's known size and refuse every other non-NULL size
with the standard size-mismatch log. `rocketnet_server_dev_config_sized`
applies the same rule before writing. A NULL config retains each call's
existing default or counted-no-op behavior.

The client and server creates return a handle or `NULL` and write the outcome
through a final nullable status out-parameter: `ROCKETNET_STATUS_OK` with the
handle, or the refusal's specific code with `NULL`. The four creates remain
`rocketnet_client_create`, `rocketnet_client_create_sized`,
`rocketnet_server_create` and `rocketnet_server_create_sized`; the sized forms
also take the config size. There is no second `_ex` or `_status` form.

## 8. The Public Counter and Stat Enums

A public enum that ends in a `_COUNT` value is an enum whose members are read
one at a time through a query, not a struct that travels as a block. The
`_COUNT` member is its end marker: `ROCKETNET_CLIENT_COUNTER_COUNT`,
`ROCKETNET_SERVER_COUNTER_COUNT`, `ROCKETNET_CLIENT_STAT_COUNT`,
`ROCKETNET_SERVER_STAT_COUNT`, `ROCKETNET_SERVER_CLIENT_COUNTER_COUNT` and
`ROCKETNET_SERVER_CLIENT_STAT_COUNT`.

The rule is the enum form of section 1's append-only rule:

1. **A new member goes immediately before `_COUNT`.** Every earlier member
   keeps its value, and `_COUNT` moves up by one so it still names the number
   of members.
2. **A member is never removed, reordered or renamed.** A retired member keeps
   its value and its query returns its documented zero, the same way a retired
   counter keeps its index in `sdk/rocketnet_counters.h`.
3. **`_COUNT` is never a query argument.** A query given `_COUNT`, or any other
   value outside the enum, returns its documented zero.
4. **The enums need no sized entry points.** A query names one member and
   returns one value, so there is no block whose layout a revision changes;
   an older caller that does not know a new member simply never asks for it.

A counter member is wired to the internal counter of the same meaning in
`sdk/rocketnet_counters.h`; the query is the public face of that internal
count, and a member with no internal counter and no code path is not admitted.

## 9. The Lifecycle API: ABI 1 at v1.0.0

`ROCKETNET_ABI_VERSION` is 1 at v1.0.0 (soname `librocketnet.so.1`), the
first released ABI. The lifecycle API changed the calling contract once
before 1.0.0; no earlier ABI was released, so the release starts at 1:

- `rocketnet_client_create` and `rocketnet_client_create_sized` take no
  connect token and report through a final nullable status out-parameter; the
  token moves to the new `rocketnet_client_connect`. The same status contract
  applies to `rocketnet_server_create` and `rocketnet_server_create_sized`.
- `rocketnet_server_create` and `_sized` bind nothing; the new
  `rocketnet_server_start`, `rocketnet_server_stop` and
  `rocketnet_server_running` run the transport, and
  `rocketnet_server_disconnect_all_clients` is new.
- Every public client index is `uint64_t` (`rocketnet_client_index` returns
  one; every server call taking a client index or destination id takes one).
- The message calls are renamed verb first, noun last (`_start_message`,
  `_finish_message`, `_abort_message`, `_receive_message`,
  `_release_message`, `_send_packets`); the public broadcast call is
  removed; `rocketnet_init_ex`, `rocketnet_term_ex` and
  `rocketnet_allocator_ex` leave the public header and `tools/exports.txt`.
- `ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED`, `SERVER_NOT_STOPPED` and
  `FLOW_ID_RESERVED` append before `ROCKETNET_STATUS_COUNT` (section 1's
  append-only rule).
- The assert callback has one way in, `rocketnet_set_assert_callback` (NULL
  restores the default), as netcode.h has only its setter: the writable
  global pointer the header used to declare leaves the public header and
  `tools/exports.txt`, and the pointer the library calls is internal and not
  exported. `rocketnet_set_quiet` replaces the quiet switch under the set_
  naming, and the status count is `ROCKETNET_STATUS_COUNT`, like every other
  `_COUNT`. Removing and renaming these exports before the first release keeps
  ABI 1: no released binary was ever linked against the old names.
## 10. Public enums are int-sized, and zero is a named receipt status

The library and its callers are built without `-fshort-enums`. Every public
enum in `sdk/rocketnet.h` is the size of `int`. The header check
`rocketnet_public_enums_are_int` fails a translation that shortens one.

`rocketnet_receipt_status_t` names zero. `ROCKETNET_RECEIPT_STATUS_NONE` is 0,
`ROCKETNET_RECEIPT_STATUS_ACKED` is 1, and `ROCKETNET_RECEIPT_STATUS_LOSS` is 2.
A zero-filled receipt reads NONE. The library writes ACKED or LOSS.

`rocketnet_disconnect_reason_t` keeps its values. Value 2 is reserved and is
not a reason: the reliable-timeout error is byte 6 of the committed vector
`m9.resend-trace` (`standard/MESSAGES.md`, `00 01 00 01 00 01 04 01 00 01`).
`rocketnet_disconnect_reason_name` returns a name for every value in the enum
and `"unknown"` for 2 and for every other value outside it. The same rule
holds for `rocketnet_status_name`, `rocketnet_log_level_name` and
`rocketnet_client_state_name`.

