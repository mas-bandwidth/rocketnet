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
  the request (`STANDARD.md`; `tla/RNSecurityTables.tla`,
  `BoundOnlyToOpener`). netcode 1.02 binds at the request. A first client
  holding the token connects (request, cookie, challenge, response) through
  `rocketnet_server_protocol_process_packet`, and the server admits it. A
  second client holding the same token then connects from another address,
  its handshake driven through response open as far as the server lets it.
  It is refused at the request ladder with `TOKEN_ALREADY_USED`: the first
  response open bound the token to the first address. The reason class
  `replay` is read from `CONNECTION_REQUEST_TOKEN_ALREADY_USED`,
  `CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED` or
  `CONNECTION_RESPONSE_TOKEN_ALREADY_CONNECTED`, whichever fired (the two
  `TOKEN_ALREADY_CONNECTED` counters are retired with the admitted-token set
  and read zero; the reader keeps them so a record from an older build
  still classifies). The second
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

`verdict` is one of:

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

## Files

* `cases.tsv` — the case table and the netcode reference decisions.
* `netcode_decisions.cpp` — the harness: drives rocketnet live and compares.
* `run.sh` — builds both libraries (netcode only when `NETCODE_DIR` is given)
  and runs the harness.
* `README.md` — this file.
