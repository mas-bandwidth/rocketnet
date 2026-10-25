# Linux thirty-minute soak per transport

## Status

The earlier rows of this record (source head
`720393a0aadd60ac43a8b5bcf887c7c51c6be999`) ran on an unimpaired link and are
superseded. That version of `scripts/soak-per-transport` set environment
names the library does not read, and soak's transport creates dropped the
link simulator fields, so none of the five cells applied the loss, latency,
jitter or duplication the record claimed. Those rows are not evidence of an
impaired run and are replaced by the rows below.

The re-run below uses the fixed harness: the script sets the library's
`ROCKETNET_DEV_SERVER_*` and `ROCKETNET_DEV_CLIENT_*` link simulator names,
soak prints the setting each end actually applied
(`soak link simulator: <end> ...`), and `--assert-flat` refuses an impaired
run in which the impairment leaves no trace (nothing missing, or no
already-received refusal, in either direction).

Verdict: every cell PASSES under the applied impairment. The four socket
cells passed at the first re-run head. The xdp cell failed there (it never
connected under the impairment, a library defect on the xdp transport with
the server's link simulator active); that FAIL row is superseded by the xdp
re-run below, at a head that contains the library fix, which passed.

## Setup

- Source head for the four socket cells:
  `e8d209cc3f445b9dba7f8f585d7fd88f054e4b50`, which contains the soak
  impairment fix.
- Source head for the xdp cell:
  `77178a490babc8902508e0f24c546f5a2f1cf67f`, which contains both the soak
  impairment fix and the xdp fix for replies the link simulator holds
  (`c37fb637`). The socket rows were not re-run at this head. The commits
  between the two heads also change library and soak code the socket cells
  run (among them `sdk/rocketnet_server_socket_linux.cpp`,
  `sdk/rocketnet_server_uring_linux.cpp`, `sdk/rocketnet_platform.cpp` and
  `soak.c`), so the socket rows are evidence for the first head only, not
  for the xdp head.
- Date (`date -u`): Sun Oct 25 2026 UTC; the xdp re-run ran from 11:58:11
  to 12:28:15 UTC.
- Impairment, at both ends of the link simulator: loss 5%, latency 30 ms,
  jitter 20 ms, duplication 2%.
- Every cell ran `scripts/soak-per-transport --seconds 1800 --cell <cell>
  --log-dir <dir>` (which runs `bin/soak --seconds 1800 --assert-flat`)
  under the bench slot lock (`benchslot -- ...`), one soak per machine at a
  time, with the machine's one-minute load sampled every ten seconds.
- Socket machines: 16 cores each, `uname -r` = `7.0.0-38-generic`,
  `/proc/sys/kernel/io_uring_disabled` = `0`. All socket traffic used
  loopback, each cell on its own port (`SOAK_SERVER_ADDRESS=127.0.0.1:<port>`).
- XDP machine: the dedicated XDP bench, 17 cores, `uname -r` =
  `6.8.0-142-generic`. The script's virtme-ng guest of that kernel, a
  guest-only veth pair, server on `10.77.4.1:40412`, clients in the guest's
  far namespace. Nothing was attached to any interface outside the guest.
  For the xdp re-run the machine's one-minute load, sampled every ten
  seconds, peaked at 2.82 (17 cores), at the start.
- Every machine built the head with `make -j4` under the slot lock; each
  build exited 0 with the last line `[100%] Built target functional`. The
  xdp machine rebuilt the xdp head the same way, with the same last line.

## Applied settings

Every socket cell's log carries, for both ends, exactly the setting the
cell asked for:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
```

The xdp re-run's log carries the same two lines, with
`soak transport: server xdp` and `soak transport: client portable`.

## Cell results

Witness counts are soak's `soak link witness:` lines: payloads missing in
each direction, and the receiving end's already-received refusals of the
planted duplicates.

| Cell | Granted server/client | Iterations / connects | Sent / received / verified | Missing c->s / s->c | Duplicates refused at server / client | Max load (cores) | Result |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| portable | portable / portable | 1,591,080 / 60 | 3,153,086 / 2,992,676 / 2,992,676 | 77,295 / 78,883 | 48,450 / 48,542 | 11.79 (16) | PASS |
| gso | gso / portable | 1,595,706 / 60 | 3,158,910 / 2,998,619 / 2,998,619 | 77,293 / 78,677 | 47,575 / 50,563 | 13.79 (16) | PASS |
| gso-plain | gso / portable | 1,591,831 / 60 | 3,151,190 / 2,990,872 / 2,990,872 | 77,430 / 78,576 | 47,417 / 50,428 | 15.10 (16) | PASS |
| io_uring | io_uring / portable | 1,580,446 / 60 | 3,129,170 / 2,970,122 / 2,970,122 | 76,767 / 78,016 | 47,139 / 48,181 | 1.34 (16) | PASS |
| xdp (re-run, xdp head) | xdp / portable | 1,593,534 / 60 | 3,147,298 / 2,896,529 / 2,896,529 | 77,109 / 165,137 | 43,434 / 44,302 | 2.82 (17) | PASS |
| xdp (first head, superseded) | xdp / portable | 9,342 / 0 | 0 / 0 / 0 | 0 / 0 | 0 / 0 | 3.24 at start (17) | FAIL, superseded |

The superseded xdp row is kept for the record of the defect; the xdp
re-run row replaces it.

Every PASS cell printed `conservation ok (server and every client)`, 60
connects (59 reconnects, the planned cycles) and zero failures, and the
script's judge line named `link=impaired`.

The cells' `SOAK` lines:

```text
SOAK cell=portable PASS server=portable client=portable seconds=1800 verified=2992676 fds=8/8/10 threads=3/3/4 resident_bytes=115965952/115965952/116609024 link=impaired missing=77295/78883 duplicates_refused=48450/48542
SOAK cell=gso PASS server=gso client=portable seconds=1800 verified=2998619 fds=10/10/12 threads=6/6/7 resident_bytes=294453248/294453248/295124992 link=impaired missing=77293/78677 duplicates_refused=47575/50563
SOAK cell=gso-plain PASS server=gso client=portable seconds=1800 verified=2990872 fds=10/10/12 threads=6/6/7 resident_bytes=294453248/294453248/295120896 link=impaired missing=77430/78576 duplicates_refused=47417/50428
SOAK cell=io_uring PASS server=io_uring client=portable seconds=1800 verified=2970122 fds=10/10/12 threads=1/1/2 resident_bytes=1269981184/1269981184/1270730752 link=impaired missing=76767/78016 duplicates_refused=47139/48181
SOAK cell=xdp PASS server=xdp client=portable seconds=1800 verified=2896529 fds=18/18/20 threads=2/2/3 resident_bytes=584273920/575909888/588935168 link=impaired missing=77109/165137 duplicates_refused=43434/44302
```

The superseded first-head xdp line:

```text
SOAK cell=xdp FAIL soak exited 1: soak: client connection failed (ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2);soak: insufficient coverage: require a connection and verified payloads in both directions;soak: bounded run ended before its duration target;
```

The xdp re-run's witness lines:

```text
soak link witness: client_to_server missing=77109 late=1014961 repeated=0 duplicates_refused=43434
soak link witness: server_to_client missing=165137 late=953962 repeated=0 duplicates_refused=44302
```

Its server-to-client missing count is about twice the client-to-server
count and twice the socket cells' (the client also counted
`SEND_HISTORY_LOST 104857`); it is recorded as measured, not explained.
The log has no `SEND_NO_LINK_ADDRESS` line.

## Resource start / end / peak

The post-warmup process-resource lines (`soak resources:`). Equal fd and
thread starts and ends, and resident and library memory ends no greater
than starts, satisfied `--assert-flat` in every PASS cell.

| Cell | fds | threads | resident_bytes | library_bytes |
| --- | --- | --- | --- | --- |
| portable | 8 / 8 / 10 | 3 / 3 / 4 | 115,965,952 / 115,965,952 / 116,609,024 | 105,680,962 / 105,680,962 / 125,552,767 |
| gso | 10 / 10 / 12 | 6 / 6 / 7 | 294,453,248 / 294,453,248 / 295,124,992 | 283,739,321 / 283,739,321 / 303,611,126 |
| gso-plain | 10 / 10 / 12 | 6 / 6 / 7 | 294,453,248 / 294,453,248 / 295,120,896 | 283,739,321 / 283,739,321 / 303,611,126 |
| io_uring | 10 / 10 / 12 | 1 / 1 / 2 | 1,269,981,184 / 1,269,981,184 / 1,270,730,752 | 1,259,317,887 / 1,259,317,887 / 1,279,189,692 |
| xdp (re-run) | 18 / 18 / 20 | 2 / 2 / 3 | 584,273,920 / 575,909,888 / 588,935,168 | 558,094,915 / 558,094,915 / 577,966,752 |
| xdp (first head, superseded) | none (the run ended inside the warm cycle) | | | |

## The xdp cell at the first head (superseded)

The xdp re-run above, at a head with the library fix, connected 60 times
with no failure and supersedes this section, which records the defect.

At the first head the xdp cell failed at connect under the impairment and
passed without it. The impaired run ended after 15 seconds; its log tail:

```text
soak link simulator: server latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak transport: server xdp
soak transport: client portable
soak link simulator: client latency_ms=30 jitter_ms=20 loss_percent=5 duplicate_percent=2
soak: client connection failed (ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT, COOKIE_REQUEST_TIMED_OUT, -2)
soak: insufficient coverage: require a connection and verified payloads in both directions
soak: bounded run ended before its duration target
soak: REFUSAL on a clean run at server: SEND_NO_LINK_ADDRESS = 93
soak link witness: client_to_server missing=0 late=0 repeated=0 duplicates_refused=0
soak link witness: server_to_client missing=0 late=0 repeated=0 duplicates_refused=0
soak: --assert-flat FAILED: link impairment not observed: client loss_percent=5 and client_to_server missing=0
soak: --assert-flat FAILED: link impairment not observed: server loss_percent=5 and server_to_client missing=0
soak: --assert-flat FAILED: no post-warmup and end samples (the run ended inside the warm cycle)
soak finished: 9342 iterations, 0 connects, 0 payloads verified, 9 failures
soak summary: server_CONNECTION_REQUEST_RECEIVED 94
soak summary: server_CONNECTION_REQUEST_NO_COOKIE 94
soak summary: server_SEND_NO_LINK_ADDRESS 93
```

The server received 94 connection requests and refused 93 of its replies
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

The same cell passed without the impairment on the same head and machine
(a 90-second diagnosis, `--clean`): both ends' lines showed
`latency_ms=0 jitter_ms=0 loss_percent=0 duplicate_percent=0`, and soak
finished `81068 iterations, 3 connects, 160964 payloads verified, 0 failures`.
A run of the cell in a guest of a 7.0 kernel on a socket machine failed
the same way (`SEND_NO_LINK_ADDRESS = 96`).

## Discarded run

A first portable run at the same head passed (`verified=2987025`,
`missing=77043/78478`, `duplicates_refused=48515/47731`), but its
machine's one-minute load went over its 16 cores for about a minute
(six samples from 19.98 to 31.68) from unrelated work. A run with load
above the core count is not evidence, so it was discarded; the portable
row above is the re-run on a quiet machine, whose load stayed at or below
11.79.

## Raw logs

The kept raw soak logs' SHA-256 digests:

```text
portable   7444caa191c5bef4a0209f1da7209618f26a3f7af95b35184f93fe0de026487a
gso        4a8904070e90e741b15c5f40de46f948379aa90f21b344760885d6df90ca47e4
gso-plain  97884f77fcc9dd780f2ee13ea9416f8d775175c34243b3b9d9dca8c0128a8064
io_uring   1a0e7658b449302a133531ec1d4dc2ace340d7d16d5e9b47821bbba8f63d81ff
xdp        631fe5c9c2f7689a2eb3af3282c5126c4a252e09b4261483548c464ef8b8b551  (first head, superseded)
xdp        e29d34105b01ffed2b2ad0cc658d67db84a89c0e48f67736f2c74e260ab40d34  (re-run, xdp head)
```
