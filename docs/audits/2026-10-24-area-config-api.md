# Public API, config and lifecycle audit, 2026-10-24

Cold reading audit of `mas-bandwidth/rocketnet` at
`9775266a705148969cc00fcadc1463b06879cfbd` (`main`). Scope read whole:
`sdk/rocketnet.h`, `sdk/rocketnet_config.h`, `sdk/rocketnet_config.cpp`,
`sdk/rocketnet_api.cpp`, `sdk/rocketnet.cpp`, `sdk/rocketnet_abi.cpp`,
`sdk/rocketnet_endpoint.cpp`, `sdk/rocketnet_allocator.cpp`, `USAGE.md`,
`docs/ENVIRONMENT.md` and `docs/ERRORS-AND-LOGGING.md`, with the config tables
in `sdk/rocketnet_internal.h`, the message object's start and finish in
`sdk/rocketnet_message_server.cpp`, `docs/ABI-EVOLUTION.md`, and the unit and
functional tests that pin this surface (`tests/test_unit_config_*.cpp`,
`tests/test_unit_status*.cpp`, `tests/test_unit_create_reports_status.cpp`,
`tests/test_unit_api_polish*.cpp`, `tests/test_unit_client_server_lifecycle.cpp`,
`tests/test_lifecycle.cpp`). Method: every declaration, documented range and
documented status was read against the code that implements it, and every
candidate was traced to the input or call sequence that reaches it. Findings
1, 3, 4, 6 and 7 were confirmed with a temporary unit probe (no socket, no
thread), built and run once on Linux at this sha and then deleted; it is not
part of the repository. No code was changed. Findings already filed in
`docs/audits/` are listed once under "Already filed". Findings are ordered
most severe first; each gives severity (`bug` / `risk` / `quality`),
`file:line` at 9775266a, a one-sentence defect, a one-sentence concrete
failure or cost, and the smallest fix.

## Findings

### 1. bug — `sdk/rocketnet_api.cpp:716`

- Defect: `rn_client_create_current` returns `NULL` with `ROCKETNET_STATUS_OUT_OF_MEMORY` when the block allocation fails without logging anything (the server's copy at line 1658 is the same), while `sdk/rocketnet.h:1522` says create "logs any failure" and `docs/ERRORS-AND-LOGGING.md:36` and `:40` promise "`NULL` with one error line".
- Failure or cost: A game whose allocator refuses the create block gets a `NULL` client or server and no log line at all (the probe counted 0 error lines on this path), so the one refusal most likely in a constrained process is the one a log-only operator cannot see.
- Smallest fix: Log one error naming the side and the byte count before writing `OUT_OF_MEMORY` at both sites, as the short-block branch at line 704 already does.

### 2. bug — `sdk/rocketnet_message_server.cpp:1058`

- Defect: A finish whose `bytes_written` is outside 1 .. the reserved size is refused and its start aborted with no log line on any path (here and at lines 989 and 1035), and a second start while one is pending is refused at line 807 the same way, while `sdk/rocketnet.h:1636` says "An invalid size logs an error" and `sdk/rocketnet.h:994`–`996` say invalid sends "return NULL or 0 and log".
- Failure or cost: A game that finishes a message with `bytes_written` 0 or past its reservation loses the message with only a 0 return, no log and no public stats field (the refusal counter, `MESSAGE_SEND_REFUSED`, is not in either stats struct), so the bug that caused it cannot be diagnosed from the log the header points at.
- Smallest fix: Log one error naming the refused `bytes_written` and the reserved size beside each `rn_message_connection_cancel_pending` on the refused-finish paths, and one for the pending-start refusal.

### 3. risk — `sdk/rocketnet_api.cpp:559`

- Defect: The exported `rocketnet_client_light_config` (and `rocketnet_server_light_config` at line 1413) sets `config->config_bytes = sizeof( *config )` inside the library, so it uses the library's struct size rather than the caller's, unlike the header's inline `default_config`, which captures the caller's `sizeof` before calling into the library.
- Failure or cost: Once the config struct grows, a program built against the older header and linked to the newer library calls the light config with its smaller struct, and the library writes every field of its larger struct past the caller's object, breaking `docs/ABI-EVOLUTION.md` rule 5 ("never reads or writes past `config_bytes`"); the probe showed the call overwriting `config_bytes` 16 with 96 and writing `max_message_receipts` at offset 76.
- Smallest fix: Make both light configs header inlines that set `config_bytes` to the caller's `sizeof` and call an exported `_internal` form that honours the incoming `config_bytes`, as `rocketnet_client_default_config` does.

### 4. risk — `sdk/rocketnet_api.cpp:527`

- Defect: `rn_public_config_bytes` accepts any `config_bytes` of at least `sizeof( size_t )` and `rn_public_config_copy` copies exactly that many bytes, so a size that ends inside a field copies part of it and leaves the rest at the default.
- Failure or cost: A client config with `config_bytes` 12 copies the low four bytes of the `transport` pointer over the default's `NULL`, and create or required_memory then calls `strlen` on the truncated pointer; the probe's `rocketnet_client_required_memory` died with SIGSEGV on exactly this input, where the header promises a refusal for a too-small size.
- Smallest fix: Accept only the sizes the struct has had (`ROCKETNET_CLIENT_CONFIG_BYTES_1` and `ROCKETNET_SERVER_CONFIG_BYTES_1` already exist in `sdk/rocketnet_layers.h:26`), refusing any other size with `CONFIG_NOT_INITIALIZED`, as the internal configs' known-size lists do.

### 5. risk — `sdk/rocketnet.cpp:210`

- Defect: `rocketnet_status_name` switches directly on its enum parameter, and `rocketnet_client_state_name` and `rocketnet_disconnect_reason_name` (`sdk/rocketnet_api.cpp:1007`, `:1034`) load theirs before casting, while `rocketnet_log_level` and `rocketnet_log_level_name` (line 90) copy the argument into an `int` first because, as their comment says, loading a C caller's out-of-range enum value is undefined in C++.
- Failure or cost: The header invites any value ("Returns UNKNOWN for other values"), but a C caller passing a value outside the enum's range of values (for example 100 to `rocketnet_status_name`) reaches undefined behaviour that `-fsanitize=enum` reports, which the unit tests miss because they only pass in-range unknowns (-32, 1, 5, -14).
- Smallest fix: Copy the argument into an `int` with `memcpy` and switch on that in all three functions, as `rocketnet_log_level_name` does.

### 6. quality — `sdk/rocketnet_config.cpp:513`

- Defect: `rocketnet_config_apply_env_uint64`, the reader of `ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES` and `ROCKETNET_SERVER_MEMORY_BUDGET_BYTES`, assigns a parsed value without the override line every other environment override writes (`rocketnet_config_apply_env_value`, line 626), and ignores an empty value without the "ignores empty" line.
- Failure or cost: An operator who sets a memory budget gets no line saying it took effect (the probe saw one override line for `ROCKETNET_CLIENT_NUM_CHANNELS` and none for the budget), so a later `TRANSPORT_LADDER_EXHAUSTED` at connect or start has no logged cause, against the engine's own rule that every override logs with the value it displaced (`sdk/rocketnet_config.h:21`).
- Smallest fix: Log `"%s config override %s=%s (displaces %" PRIu64 ")"` after a successful parse, and the empty-value info line, as `rocketnet_config_apply_env` does.

### 7. quality — `sdk/rocketnet_config.h:108`

- Defect: The comment on `rocketnet_config_apply_env` says an old process-wide `ROCKETNET_<FIELD>` "is never applied and logs one error naming its replacement, once per create", but neither `rocketnet_config_apply_env` (`sdk/rocketnet_config.cpp:631`) nor any caller checks the old names, and `rocketnet_config_env_var_name`, the process-wide derivation, is called only by a unit test.
- Failure or cost: An operator still setting an old name such as `ROCKETNET_NUM_CHANNELS` is ignored in silence (the probe saw no line naming it and no error), the opposite of what the comment promises, and a maintainer reading the comment believes a guard exists that does not.
- Smallest fix: Rewrite the comment to say old process-wide names are not read, and move `rocketnet_config_env_var_name` to the test that uses it, or implement the one-error check the comment describes.

### 8. quality — `sdk/rocketnet_api.cpp:12`

- Defect: The file's opening comment says the transport fields "take theirs at the transport's create, as they always have", but `rn_client_split` (line 258) and `rn_server_split` (line 344) apply the transport table's overrides at public create, and connect and start never read the environment (`sdk/rocketnet.h:1018`–`1020`).
- Failure or cost: A maintainer trusting the file header looks for the transport overrides in the transport create and can reintroduce a second environment read at connect or start, the drift the resolve-once tests exist to prevent.
- Smallest fix: Rewrite the sentence to say every override, transport rows included, is applied once at public create by the split.

### 9. quality — `sdk/rocketnet_config.h:118`

- Defect: The comment "validate a config struct against the descriptors ... refuses loudly, never clamps" sits above the declarations of the default-config and transport-create functions (lines 123–146), and the declaration it describes, `rocketnet_config_validate`, is at line 148 with no comment.
- Failure or cost: A reader of the header attributes validation semantics to create entry points that do not validate in that way, and has to search for the function the comment belongs to.
- Smallest fix: Move the four comment lines to directly above `rocketnet_config_validate` at line 148.

### 10. quality — `sdk/rocketnet_api.cpp:143`

- Defect: Comments cite line numbers that no longer show what they name: this one gives `rn_client_split (:135)` and `rn_server_split (:173)` (they are at lines 231 and 297), `sdk/rocketnet_message_server.cpp:214` repeats the same `:135`/`:173`, `sdk/rocketnet_internal.h:571` cites `sdk/rocketnet_api.cpp:119 and :125` for the message env pass (it is `rn_apply_message_and_budget_env` at line 145), and `sdk/rocketnet_config.cpp:709` puts the `step_driven` row at `:88` (it is line 89).
- Failure or cost: Each cite sends a reader to an unrelated line (line 119 is `rn_public_send_kbps`), so the cross-references meant to tie the config rules together cost a search apiece and can be followed to the wrong code.
- Smallest fix: Replace the line numbers with the function or row names they point at.

### 11. quality — `docs/ERRORS-AND-LOGGING.md:36`

- Defect: The create rows list the refusal codes as complete, but the client's list omits `TEST_PACKET_FILTER_IN_PRODUCTION` (returned through `rn_client_config_valid`, `sdk/rocketnet_api.cpp:619`) and the server's ("the client's codes, plus `INVALID_ADDRESS`", line 40) omits both that code and `CRYPTO_INIT_FAILED` (`sdk/rocketnet_api.cpp:1671`), and neither `sdk/rocketnet.h:1526`–`1528` nor `:1753`–`1754` names the codes create can return.
- Failure or cost: A game that switches on the documented create codes has no case for the production filter refusal, the first refusal every production build with default settings meets, and reports it as an unexpected status.
- Smallest fix: Add the two codes to both rows of the table and to the section 2.1 paragraph that lists create's codes.

### 12. quality — `docs/ERRORS-AND-LOGGING.md:80`

- Defect: The status rules say the failure codes are "dense from `-1` to `-30`", while the enum printed below them and `sdk/rocketnet.h:344` end at `ROCKETNET_STATUS_CONNECT_TOKEN_PORT_ZERO = -31`; likewise `docs/ABI-EVOLUTION.md:174` says each disconnect reason "is negative, -1 to -5", while `sdk/rocketnet.h:495` defines `MESSAGE_NOT_RECEIVED = -6`.
- Failure or cost: A binding or FFI layer generated from the prose ranges maps -31 and -6 to unknown values, and the two documents disagree with the header they name as the authority.
- Smallest fix: Change the ranges to `-1` to `-31` and `-1` to `-6`.

### 13. quality — `sdk/rocketnet.h:1211`

- Defect: The `max_clients` comment says "Default is 1000", but the default is `ROCKETNET_DEFAULT_MAX_CLIENTS` (`sdk/rocketnet_internal.h:522`), which is the build's `ROCKETNET_MAX_CLIENTS` when that is 512 .. 999.
- Failure or cost: A program built with `-DROCKETNET_MAX_CLIENTS=512` that relies on the documented default sizes its own tables for 1000 clients while the server accepts at most 512.
- Smallest fix: Say "Default is 1000, or ROCKETNET_MAX_CLIENTS when the build sets it lower".

### 14. quality — `sdk/rocketnet_api.cpp:1199`

- Defect: `RN_CLIENT_SEND_DEFERRED` is read from the counters into every connection's counts and summed into the lifetime counts, but no field of `struct rocketnet_client_stats_t` is filled from it.
- Failure or cost: A reader sees a count carried across connections as if it were published, and the loop spends a counter read and a sum on a value nothing returns.
- Smallest fix: Delete the enum entry and its read, or publish it in the stats struct (an ABI addition at the struct's end).

## Counts

- bug: 2 (findings 1, 2)
- risk: 3 (findings 3–5)
- quality: 9 (findings 6–14)
- total: 14

## Already filed

- `sdk/rocketnet_config.cpp:330`, the identity function `rocketnet_config_env_field_name`: the 2026-10-24 code-quality audit in `docs/audits/` with 15 findings, finding 14.
- `sdk/rocketnet_api.cpp:176`, the development simulator's bare `strtol` environment parse: the 2026-10-24 code-quality audit in `docs/audits/` with 12 findings, finding 8.
