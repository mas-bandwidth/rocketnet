# Linux thirty-minute soak per transport

## Status

The earlier rows of this record (source head
`720393a0aadd60ac43a8b5bcf887c7c51c6be999`) ran on an unimpaired link and are
superseded. That version of `scripts/soak-per-transport` set environment
names the library does not read, and soak's transport creates dropped the
link simulator fields, so none of the five cells applied the loss, latency,
jitter or duplication the record claimed. Those rows are not evidence of an
impaired run.

The rows below are a re-run at source head
`cd5fa94259537c64ef4d045eed4afe79e1192377`, which contains the soak
impairment fix (`e8d209cc3f445b9dba7f8f585d7fd88f054e4b50`). They replace
both the unimpaired rows and the earlier re-run rows recorded at the fix's
own head.

The re-run uses the fixed harness: the script sets the library's
`ROCKETNET_DEV_SERVER_*` and `ROCKETNET_DEV_CLIENT_*` link simulator names,
soak prints the setting each end actually applied
(`soak link simulator: <end> ...`), and `--assert-flat` refuses an impaired
run in which the impairment leaves no trace (nothing missing, or no
already-received refusal, in either direction).

Verdict: the four socket cells PASS under the applied impairment; the xdp
cell FAILS (it never connects under the impairment, see below). The record
is therefore not green.

## Setup

- Source head for every build and cell:
  `cd5fa94259537c64ef4d045eed4afe79e1192377`, which contains the soak
  impairment fix.
- The record's own commit changes only this file; every cell ran on the
  code of the head above.
- Date (`date -u`): Sun Oct 25 2026 UTC.
- Impairment, at both ends of the link simulator: loss 5%, latency 30 ms,
  jitter 20 ms, duplication 2%.
- One machine of 64 cores, `uname -r` = `7.0.0-34-generic`,
  `/proc/sys/kernel/io_uring_disabled` = `0`. The four socket cells ran on
  loopback with kernel-granted ports. All five cells ran at the same time on
  that machine; the one-minute load, sampled every ten seconds for the
  whole window, peaked at 3.24 on 64 cores, so no cell ran with load above
  the core count and none was perturbed by another's work.
- XDP: a virtme-ng guest of the machine's 6.8 kernel, a guest-only veth
  pair, the server bound on `10.77.4.1:40412` and every client opened in the
  guest's far namespace. Nothing was attached to any interface outside the
  guest.
- Every cell except xdp ran
  `scripts/soak-per-transport --seconds 1800 --cell <cell> --log-dir <dir>`,
  which runs `bin/soak --seconds 1800 --assert-flat`.
- The xdp cell ran through a copy of the same script with one change: its
  short virtme-ng temporary directory is taken from the caller's short temp
  root instead of the literal `/tmp`, which the run environment forbids. The
  change moves no soak setting and no judge rule.
- The head built with `make -j8` in Release; the build exited 0, printed no
  warnings, and its last line was `[100%] Built target functional`.

## Applied settings

Every cell's log carries, for both ends, exactly the setting the cell asked
for:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
```

The xdp cell's log carries the same two lines before the failed connect.

## Cell results

Witness counts are soak's `soak link witness:` lines: payloads missing in
each direction, and the receiving end's already-received refusals of the
planted duplicates. "Applied" is the body of the two `soak link simulator:`
lines above; each cell printed it for the server and for the client.

| Cell | Granted server / client | Applied (server and client, each) | Iterations / connects | Sent / received / verified | Missing c->s / s->c | Duplicates refused server / client | Max load (cores) | Result |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| portable | portable / portable | `latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2` | 1,515,002 / 60 | 3,008,816 / 2,855,908 / 2,855,908 | 73,692 / 75,078 | 45,666 / 44,696 | 3.24 (64) | PASS |
| gso | gso / portable | `latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2` | 1,516,436 / 60 | 3,012,116 / 2,858,988 / 2,858,988 | 73,818 / 75,074 | 44,765 / 48,162 | 3.24 (64) | PASS |
| gso-plain | gso / portable | `latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2` | 1,518,018 / 60 | 3,015,262 / 2,861,754 / 2,861,754 | 73,920 / 75,322 | 44,791 / 48,214 | 3.24 (64) | PASS |
| io_uring | io_uring / portable | `latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2` | 1,494,408 / 60 | 2,965,518 / 2,814,496 / 2,814,496 | 72,645 / 74,206 | 44,229 / 45,649 | 3.24 (64) | PASS |
| xdp | xdp / portable | `latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2` | 7,433 / 0 | 0 / 0 / 0 | 0 / 0 | 0 / 0 | 3.24 (64) | FAIL |

Every PASS cell printed `conservation ok (server and every client)`, 60
connects (59 reconnects, the planned cycles) and zero failures, and the
script's judge line named `link=impaired`.

The cells' `SOAK` lines, which also carry the flat resource counts:

```text
SOAK cell=portable PASS server=portable client=portable seconds=1800 verified=2855908 fds=7/7/9 threads=3/3/4 resident_bytes=115605504/115605504/116244480 link=impaired missing=73692/75078 duplicates_refused=45666/44696
SOAK cell=gso PASS server=gso client=portable seconds=1800 verified=2858988 fds=9/9/11 threads=6/6/7 resident_bytes=293679104/293679104/294346752 link=impaired missing=73818/75074 duplicates_refused=44765/48162
SOAK cell=gso-plain PASS server=gso client=portable seconds=1800 verified=2861754 fds=9/9/11 threads=6/6/7 resident_bytes=293679104/293679104/294354944 link=impaired missing=73920/75322 duplicates_refused=44791/48214
SOAK cell=io_uring PASS server=io_uring client=portable seconds=1800 verified=2814496 fds=9/9/11 threads=1/1/2 resident_bytes=1269886976/1269886976/1270702080 link=impaired missing=72645/74206 duplicates_refused=44229/45649
SOAK cell=xdp FAIL soak exited 1: soak: client connection failed (ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2);soak: insufficient coverage: require a connection and verified payloads in both directions;soak: bounded run ended before its duration target;
```

## Resource start / end / peak

The post-warmup process-resource lines (`soak resources:`). Equal fd and
thread starts and ends, and resident and library memory ends equal to their
starts, satisfied `--assert-flat` in every PASS cell.

| Cell | fds | threads | resident_bytes | library_bytes |
| --- | --- | --- | --- | --- |
| portable | 7 / 7 / 9 | 3 / 3 / 4 | 115,605,504 / 115,605,504 / 116,244,480 | 105,681,026 / 105,681,026 / 125,552,863 |
| gso | 9 / 9 / 11 | 6 / 6 / 7 | 293,679,104 / 293,679,104 / 294,346,752 | 283,739,481 / 283,739,481 / 303,611,318 |
| gso-plain | 9 / 9 / 11 | 6 / 6 / 7 | 293,679,104 / 293,679,104 / 294,354,944 | 283,739,481 / 283,739,481 / 303,611,318 |
| io_uring | 9 / 9 / 11 | 1 / 1 / 2 | 1,269,886,976 / 1,269,886,976 / 1,270,702,080 | 1,259,317,887 / 1,259,317,887 / 1,279,189,724 |
| xdp | none (the run ended inside the warm cycle) | | | |

## The xdp cell

The xdp cell fails at connect under the impairment and passes without it.
The impaired run ended after 15 seconds; its log tail:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server xdp
soak transport: client portable
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak: client connection failed (ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2)
soak: insufficient coverage: require a connection and verified payloads in both directions
soak: bounded run ended before its duration target
soak: REFUSAL on a clean run at server: SEND_NO_LINK_ADDRESS = 96
soak link witness: client_to_server missing=0 late=0 repeated=0 duplicates_refused=0
soak link witness: server_to_client missing=0 late=0 repeated=0 duplicates_refused=0
soak: --assert-flat FAILED: link impairment not observed: client loss_percent=5 and client_to_server missing=0
soak: --assert-flat FAILED: link impairment not observed: server loss_percent=5 and server_to_client missing=0
soak: --assert-flat FAILED: no post-warmup and end samples (the run ended inside the warm cycle)
soak finished: 7433 iterations, 0 connects, 0 payloads verified, 9 failures
soak summary: server_CONNECTION_REQUEST_RECEIVED 97
soak summary: server_CONNECTION_REQUEST_NO_COOKIE 97
soak summary: server_SEND_NO_LINK_ADDRESS 96
```

The server received 97 connection requests and refused 96 of its replies
with `SEND_NO_LINK_ADDRESS`. The guest's veth has no gateway (the server
warns `server found no gateway on rnxdpsoak0`), so the xdp driver can
address a reply only to a client whose MAC it has learned, or as the answer
to a frame in the receive view it is processing
(`rocketnet_server_socket_start_packet`, `sdk/rocketnet_server_socket_linux.cpp`).
A cookie reply sent through the server's link simulator is held and sent
later from `rocketnet_server_link_drain` with a copied address, outside any
receive view, and the client's MAC is learned only when the connection
opens, after the cookie round trip. Every cookie reply is therefore refused
and the client times out. This is a library defect on the xdp transport with
the development link simulator active at the server; this record does not
change code.

The same cell passed without the impairment on this head and machine (a
90-second diagnosis, `--clean`): both ends' lines showed
`latency_ms=0 jitter_ms=0 loss_percent=0 duplicate_percent=0`, the judge
line named `link=clean verified=84240`, and soak finished
`38604 iterations, 3 connects, 75569 payloads verified, 0 failures`.

## Raw logs

The kept raw soak logs' SHA-256 digests:

```text
portable   2119036a7afbf18d6f8306acb7ab12a1cc9ea986e7e40593b6352e2e52184d14
gso        622c129ff2dfbe12b83781e3ae748fa1bea4e69dfe2cce29c524c8cb7aa0a771
gso-plain  37a6f2ea35a9e291bc7b7ce3c72e998d9387e456580b4d26ff201a3d42886179
io_uring   66df27d051ebdfa11da12a2eb708ec53c2f9dfbcc0c3d3d2e10bdea5b342d4b3
xdp        2939b7dfd9c891d8979f193f2a3b660b75247e1b86d78cfea30030f2dddc41de
```
