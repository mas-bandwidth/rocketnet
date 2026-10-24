# Differential: rocketnet refusal decisions vs netcode 1.02

A cross-implementation differential that compares the **decision** — accept or
refuse, and the refusal reason class — that rocketnet and netcode 1.02 reach
for the same connect-token case. The two wire formats differ, so the packet
bytes are never compared; only the decisions are.

The gate exists because the one defect a cross-cutting assurance gate must
catch is the **expiry-admits** class: a case where netcode refuses and
rocketnet admits. `REQUIREMENTS.md` states "no security property weaker than
netcode 1.02", so every such case is a finding. The reverse — rocketnet
refuses where netcode admits — is listed with its reason and judged against
`REQUIREMENTS.md` (stricter is permitted).

## What is compared

| case | mutation | netcode 1.02 | rocketnet |
| --- | --- | --- | --- |
| `valid_signature` | a valid token | accept | accept |
| `expired_token` | expire timestamp in the past | refuse:expired | refuse:expired |
| `bad_signature` | one signature byte flipped | refuse:signature | refuse:signature |
| `wrong_server_address` | token names an address the server is not advertised at | refuse:address | refuse:address |
| `replayed_token` | a token already admitted, presented again | refuse:replay | refuse:replay |
| `full_server` | every client slot taken | refuse:full | refuse:full |
| `wrong_protocol_id` | protocol id does not match | refuse:protocol | refuse:protocol |
| `timeout_out_of_range` | timeout seconds zero | accept | refuse:timeout |

Measured against tag v1.2.1, the first release whose version info is
`NETCODE 1.02`. That server keeps a connect-token MAC history, so a second
presentation from another address is `refuse:replay` on both sides (`agree`).
A timeout of zero is admitted by netcode 1.02 and refused by rocketnet
(`rocketnet-stricter`). Stricter is permitted under `REQUIREMENTS.md`. Weaker
(netcode refuses, rocketnet admits) is a finding. The live run is the table
in the report; the reference column matches that run.

## The two sides

* **rocketnet** is driven live, in process, with time passed in as an
  argument. The harness builds a server exactly as the unit suite's
  request-ladder test does, mints the token with the case's mutation, and
  runs `rocketnet_server_protocol_process_connection_request` through the
  cookie gate. The reason class is read from the counters the ladder
  actually incremented, never from the case's expectation.

* **the rocketnet replay case** drives whole handshakes, because the server
  binds a token to its source when a connection response opens, never at
  the request (`standard/TRANSPORT.md`; `tla/RNSecurityTables.tla`,
  `BoundOnlyToOpener`). netcode 1.02 binds at the request. A first client
  holding the token connects (request, cookie, challenge, response) through
  `rocketnet_server_protocol_process_packet`, and the server admits it. A
  second client holding the same token then connects from another address,
  its handshake driven through response open as far as the server lets it.
  It is refused at the request ladder with `TOKEN_ALREADY_USED`: the first
  response open bound the token to the first address. The reason class
  `replay` is read from `CONNECTION_REQUEST_TOKEN_ALREADY_USED` or
  `CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED`, whichever fired
  (`CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED` is a retired slot that reads
  zero; tools/differential/netcode_decisions.cpp reads both). The second
  client holds the token keys, so it is at least as strong as an attacker
  replaying the captured request, which cannot open the challenge.

* **netcode** has two modes. In the default **reference** mode the decision
  is read from the `netcode_reference` column of `cases.tsv`. With
  `NETCODE_DIR` on the command line the harness compiles
  mas-bandwidth/netcode tag **v1.2.1** (`1dd10f892f4742498a99a0bb5d9b1f8f6e94f26b`,
  version info `NETCODE 1.02`) and asks that library. It mints a connect
  token through `netcode_generate_connect_token`, lets a client write the
  connection request, and passes the packet to a server created with
  send/receive overrides, so the run opens no socket. The printed netcode
  column is the library's decision. A disagreement with the reference column
  is reported on stderr. The bad-signature case flips one byte of the
  connect token's AEAD MAC; netcode 1.02 has no Ed25519 signature on the
  token, and that MAC is the authenticity check. The full-server case
  completes one handshake so the single slot is taken, then presents a
  second request. The replay case presents the same request from a second
  address. Expiry uses the library's own wall-clock stamp: `expire_seconds`
  of zero is already expired. The in-process clocks passed to create and
  update are arguments.

## Running

```sh
# reference mode (no netcode checkout needed)
bash tools/differential/run.sh

# live mode: ask the pinned netcode 1.02 source
bash tools/differential/run.sh /path/to/netcode
```

`run.sh` builds rocketnet once (the `rocketnet_dev` library, which carries the
token minting used to construct the cases while the admission path is the
production code), then compiles and runs the harness. The build fetches
nothing; the netcode source is only ever read from the path on the command
line.

The harness prints one line per case:

```
# case	netcode	rocketnet	verdict
valid_signature	accept	accept	agree
...
```

Then one line per fuzz corpus file (below). `verdict` is one of:

* `agree` — both libraries reach the same decision.
* `rocketnet-weaker` — netcode refuses, rocketnet admits. A finding.
* `rocketnet-stricter` — rocketnet refuses, netcode admits. Listed with the
  reason and judged against `REQUIREMENTS.md`.

## The netcode side, live

`netcode_decisions.cpp` compiles the netcode integration only when
`ROCKETNET_DIFFERENTIAL_NETCODE_DIR` is defined, which `run.sh` sets only
when `NETCODE_DIR` is a directory containing `netcode.c` and `netcode.h` from
tag v1.2.1. The core is compiled with `-DNDEBUG` and `-ffp-contract=off` and
linked to the vendored sodium symbols inside `librocketnet_dev.a`. The build
fetches nothing. `netcode_decide_live` does not reimplement the checks; the
server's log lines are the decision (`challenge` is accept; expired, wrong
protocol id, decrypt failure, whitelist, already used, and server full are
the refusal classes).

## The fuzz corpus mode

`run.sh` also runs every file of `fuzz/corpus_connect_token/` through both
libraries, after the case table. Each file is read the way
`fuzz/rocketnet_fuzz_connect_token.cpp` reads it: byte 0 bit 0 picks the raw
half (the rest of the input is the token, zero-padded to 768 bytes) or the
structured half (`(u16 offset, xor)` records over a valid baseline token,
the harness's own field values, minted to the differential server's test
keys); byte 1, as signed minutes, moves the server's clock against the
token's window (created at `ROCKETNET_TEST_CREATE_TS`, expiring 300 seconds
later). Only the committed corpus files are used; the mode adds no input.

A structured mutation is mapped by the token field each changed byte lands
in (measured against the baseline, so cancelling flips change nothing) to
the same relative byte of the equivalent netcode 1.02 field, with the same
xor:

| rocketnet field | bytes | netcode 1.02 equivalent |
| --- | --- | --- |
| version | 0-3 | the request's version info, byte for byte |
| protocol id | 4-11 | the request's protocol id |
| create | 12-19 | none: not comparable |
| expire | 20-27 | the request's expire timestamp |
| timeout | 28-31 | none (sealed inside netcode's private token): not comparable |
| address flags, IPv4, IPv6 | 32-56 | none (sealed inside the private token): not comparable |
| server key | 57-88 | none (netcode has no server public key): not comparable |
| sealed private block | 89-520 | the request's encrypted private token, same offset |
| signed pad | 521-575 | none: not comparable |
| signature | 576-639 | the private token's MAC, offset mod 16 |
| clear keys and pad | 640-767 | the public token's client keys and pad, before the client writes its request |

A case touching any not-comparable field prints `not-comparable` and is not
judged. The clock maps to netcode's `expire_seconds` (300 minus the offset,
zero when that is not positive): netcode stamps and checks expiry on its own
wall clock and keeps no create time, so a token "not yet created" is a valid
token there. The raw half maps to a netcode request whose bytes after the
packet type are the same input bytes, zero-padded.

The netcode column comes from the live library when `NETCODE_DIR` is given
and from `corpus_cases.tsv` otherwise, which records the live run against
tag v1.2.1; a corpus file with no row there prints `needs-NETCODE_DIR` and
`not-run`. The live run reports on stderr where it disagrees with that file,
and prints a `FINDING` line on stderr for any `rocketnet-weaker` case.

Measured against tag v1.2.1:

```
# corpus	fields	netcode	rocketnet	verdict
raw_empty	raw,clock+0m	refuse:version	refuse:version	agree
raw_version_prefix	raw,clock+0m	refuse:version	refuse:protocol	agree
structured_before_create	untouched,clock-127m	accept	refuse:future	rocketnet-stricter
structured_expired	untouched,clock+127m	refuse:expired	refuse:expired	agree
structured_flip_clear_key	clear,clock+0m	accept	accept	agree
structured_flip_protocol_id	protocol,clock+0m	refuse:protocol	refuse:protocol	agree
structured_flip_signature	signature,clock+0m	refuse:signature	refuse:signature	agree
structured_untouched	untouched,clock+0m	accept	accept	agree
```

No case is `rocketnet-weaker`. The one `rocketnet-stricter` case is a token
presented before its create time: netcode 1.02 carries no create time to the
server and admits it; rocketnet refuses it (`TOKEN_CREATED_IN_FUTURE`), which
`REQUIREMENTS.md` permits. The verdict compares the decision only; the
reason classes may differ when both refuse (`raw_version_prefix`: netcode
stops at its version string, rocketnet at the protocol id).

## The differential fuzz target

`fuzz/rocketnet_fuzz_differential.cpp` is the libFuzzer target that turns
this decision gate into a fuzzer (nightly, Linux leg). The fixed gate and the
target share the whole decision core in `decide.h`, so they run the same
deciders on the same case model. The fuzzer never mutates packet bytes: the
two wire formats differ, so it mutates the case. The structured case is a
fixed 25-byte record (comment at the top of the target):

| bytes | field | meaning |
| --- | --- | --- |
| 0 | address_mode | 0 advertised IPv4, 1 wrong IPv4, 2 IPv6 family, 3 wrong port |
| 1 | protocol_wrong | 0 the test protocol id, nonzero a wrong id |
| 2 | flip_authenticator | nonzero flips one signature byte (rocketnet) / MAC byte (netcode) |
| 3 | replay | nonzero presents the same token from a second address |
| 4 | occupied_slots | how many client slots are taken before the candidate |
| 5-12 | create_offset | int64 little-endian seconds relative to the server's now |
| 13-20 | expire_offset | int64 little-endian seconds relative to now |
| 21-24 | timeout_seconds | int32 little-endian (0 and negative allowed) |

The target aborts on `rocketnet-weaker` (netcode refuses, rocketnet admits —
a finding). A `rocketnet-stricter` verdict is checked against the permitted
stricter classes this README documents: `timeout` (the `timeout_out_of_range`
row above) and `future` (the corpus `structured_before_create` row). A stricter
class not on that list also aborts, listing its reason, so a new stricter
class is never silently accepted. A mint/capture failure on either side is an
input-encoding artifact, not a decision, and is not judged.

### The netcode wall clock

netcode mints and checks expiry on its own wall clock (above, "The netcode
side, live"). To make a crash input reproduce, every case's times are
expressed relative to the moment netcode reads the clock: `expire_offset` is
mapped to netcode's `expire_seconds` (negative clamps to 0, already expired;
any other maps to the same count of seconds), and `create_offset` is not
carried by netcode at all. Seeds are one per case in `cases.tsv`, encoded in
this structured form under `fuzz/corpus_differential/`, with the field tags
dictionary at `fuzz/dict/rocketnet_fuzz_differential.dict`.

## Files

* `cases.tsv` — the case table and the netcode reference decisions.
* `corpus_cases.tsv` — the netcode 1.02 reference decision for each mapped
  fuzz corpus file, recorded from the live run.
* `decide.h` — the shared decision core (the case model, the rocketnet
  decider, the netcode live decider, the verdict), used by the gate and the
  fuzz target.
* `netcode_decisions.cpp` — the fixed gate: drives rocketnet live and compares.
* `run.sh` — builds both libraries (netcode only when `NETCODE_DIR` is given)
  and runs the harness.
* `fuzz/rocketnet_fuzz_differential.cpp` — the differential fuzz target.
* `README.md` — this file.
