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
| `replayed_token` | a token already admitted, presented again | accept | refuse:replay |
| `full_server` | every client slot taken | refuse:full | refuse:full |
| `wrong_protocol_id` | protocol id does not match | refuse:protocol | refuse:protocol |
| `timeout_out_of_range` | timeout seconds zero | refuse:timeout | refuse:timeout |

The `replayed_token` row is the one deliberate, expected `rocketnet-stricter`
line. netcode 1.02 binds a token to a server address and an expiry and stops
there — a token is transferable for its whole lifetime and the protocol does
not notice. rocketnet adds a used-token history and an admitted set, so the
same token presented from a second address is refused. That is stricter than
the netcode 1.02 baseline, not weaker, so it stands under `REQUIREMENTS.md`.

## The two sides

* **rocketnet** is driven live, in process, with time passed in as an
  argument. The harness builds a server exactly as the unit suite's
  request-ladder test does, mints the token with the case's mutation, and
  runs `rocketnet_server_protocol_process_connection_request` through the
  cookie gate. The reason class is read from the counters the ladder
  actually incremented, never from the case's expectation.

* **netcode** has two modes. In the default **reference** mode the decision
  is read from the `netcode_reference` column of `cases.tsv`, recorded from
  the pinned mas-bandwidth/netcode source. With `NETCODE_DIR` on the command
  line the harness is built against the netcode 1.02 source and asks the
  library live, and it reports when the live answer disagrees with the
  reference column.

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
`ROCKETNET_DIFFERENTIAL_NETCODE_DIR` is defined. The live path (`netcode_decide_live`)
mirrors netcode 1.02's server checks — read the connect token, then protocol
id, expiry, timeout, server address, then the signature and the sealed private
data — using netcode's public API. The exact pin (the mas-bandwidth/netcode
tag) is recorded by the sprint; the harness is written against the 1.02
surface and any drift against the reference column is reported at run time.

## Files

* `cases.tsv` — the case table and the netcode reference decisions.
* `netcode_decisions.cpp` — the harness: drives rocketnet live and compares.
* `run.sh` — builds both libraries (netcode only when `NETCODE_DIR` is given)
  and runs the harness.
* `README.md` — this file.
