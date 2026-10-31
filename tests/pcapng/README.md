# tests/pcapng: the pcapng corpus

Real rocketnet traffic, captured in the functional tier and replayed into the packet readers by the functional suite's corpus-consistency cases (`tests/functional/corpus_consistency.cpp`). The replay reads this directory at run time and the directory grows as regressions land, so it is a functional case, never a unit one: `./bin/test` reads no file. The captures come from the functional channel harness only, over the in-repo network simulator: never a real network, never a deployed server. Nothing of console sockets or web support is captured.

## Files

| Path | What it is |
| --- | --- |
| `tests/pcapng/<name>.pcapng` | the corpus: one capture per functional test, named for the test, plus one file built from the fuzz seeds |
| `tests/pcapng/regress/<name>.pcapng` | minimized fuzz regressions (none yet) |
| `tests/pcapng.h` | the header-only C++ reader and writer |
| `tools/pcapng.py` | the python reader and writer (`read_packets`, `write_packets`, `udp_payload`, `--selftest`) |
| `tests/functional/channel_harness.h` | the capture tee |
| `tests/functional/corpus_consistency.cpp` | the replay (`test_pcapng_corpus_replay`), its witness (`test_pcapng_corpus_witness_unread_packet_goes_red`) and the control-type coverage (`test_pcapng_corpus_covers_control_packet_types`), rows of `rocketnet_functional_tests[]` run by `./bin/functional` |

## The format

pcapng (draft-ietf-opsawg-pcapng), the subset both readers accept and both writers emit:

- one Section Header Block, little-endian, version 1.0, section length -1;
- two Interface Description Blocks, no options (timestamps are microseconds, the default resolution), snap length 65535;
- Enhanced Packet Blocks, captured length equal to original length, with at most one option, `opt_comment`.

Anything else is refused by name: `PCAPNG_TRUNCATED_BLOCK` (a block runs past the file, or its trailing length disagrees), `PCAPNG_BLOCK_LENGTH_NOT_MULTIPLE_OF_4`, `PCAPNG_UNKNOWN_LINKTYPE`, `PCAPNG_FILE_OVER_CAP`, `PCAPNG_NOT_A_SECTION`, `PCAPNG_SECOND_SECTION`, `PCAPNG_UNKNOWN_BLOCK`, `PCAPNG_UNKNOWN_INTERFACE`, `PCAPNG_BAD_PACKET_LENGTH` and `PCAPNG_BAD_OPTION`. Every length is checked against what remains before it is used, so neither reader reads past its buffer. libpcap-based tools (tcpdump) refuse these files, because libpcap takes one link type per file and the corpus has two; these two readers are the reference.

### Interface 0: the wire

`LINKTYPE_IPV4` (228). Each packet is a synthetic IPv4 + UDP frame (header length 20, TTL 64, a correct header checksum, UDP checksum 0) around the exact datagram the transport handed the simulator. The addresses are the harness's test endpoints: the client `198.51.100.7:56789` and the server `203.0.113.10:40000`, so a frame's destination says which end's receive ladder it meets.

### Interface 1: the opened plaintext

`LINKTYPE_USER0` (147). Each record is one kind byte, then the bytes the transport hands that reader:

| Kind | Reader | Bytes |
| --- | --- | --- |
| 1 | `rocketnet_ack_block_read` | the whole opened plaintext of a session packet: the ack block, then the type data |
| 3 | `rocketnet_message_reader_begin` / `_next` | a payload's type data, as the harness hands it to the message layer |

The `opt_comment` names the receiving end, `server` or `client`, which picks the counter family a refusal fires; a kind-1 record adds the packet type (`server 5`), which picks the counter within it. A record without that context is refused as `PCAPNG_RECORD_NO_CONTEXT`.

Interface 1 is how the inner readers are reached without a key in the corpus. It carries no key, no nonce and no key-derived byte: plaintext only, opened in the harness on a copy of each received packet with the receiving end's own key, which stays in memory.

### Timestamps

The functional test's simulated clock in microseconds, starting at 1000.0 s (1000000000 us). Files built by `tools/pcapng.py` start at the same instant and step 1 ms per packet. A test that creates more than one harness appends to one file, and each harness's clock starts again at 1000.0 s.

## The capture: tap points and why

The tee in `tests/functional/channel_harness.h` is active only when `ROCKETNET_PCAPNG_CAPTURE_DIR` is set when a harness is created. Unset, every tee returns at its first line and the harness runs as it does without them, so the functional suite is unchanged.

- **Interface 0, at every hand-off to the simulator**: `rocketnet_channel_endpoint_send_control` (control packets, after the wire's drop mode), `rocketnet_channel_endpoint_send_packets` (payload frames), `rocketnet_channel_endpoint_disconnect`, and `rocketnet_channel_harness_deliver_held` (held packets released in a scripted order). These are the only places a datagram leaves an end, so the corpus is exactly the traffic, before the simulator's loss, duplication and reordering.
- **Interface 1, at the two receive hand-offs**, just before `rocketnet_server_protocol_process_packet` and `rocketnet_client_protocol_process_packet`: a session packet (types 4, 5, 6, 8) from a connected mapping (server) or a client holding session keys is opened a second time on a copy, with the sequence reconstructed from the same replay state the protocol is about to use. A packet that does not open is not recorded: no reader is handed anything for it. Kind 3 is recorded where an accepted payload is handed to the message layer.

One file per functional test process, `<dir>/<test name>.pcapng` (the name from the runner's `--run <name>`), rewritten whole at each harness destroy and at exit. A capture stops recording at 72 KiB, so a long test commits a deterministic prefix. The buffers are `std::vector`, outside the counting allocator, so `test_harness_no_allocation_after_create` counts the same.

## The recipe

From the repository root on a Linux build; this regenerates every committed file:

```sh
make
rm -f tests/pcapng/*.pcapng
for name in \
    test_harness_one_message_each_way_lossy \
    test_ordered_small_and_large_interleaved \
    test_reliable_mixed_sizes_over_lossy_link \
    test_reliable_duplicate_after_gap_delivered_once \
    test_reliable_sequenced_held_record_released \
    test_unreliable_receipts_over_lossy_link \
    test_unreliable_at_most_once_over_duplicating_link \
    test_unreliable_dropped_when_builder_out_of_budget_never_arrives \
    test_control_disconnect_burst_each_end \
    test_control_standalone_ack_each_end \
    test_control_denied_when_server_full
do
    ROCKETNET_PCAPNG_CAPTURE_DIR=tests/pcapng ./bin/functional --run $name > /dev/null || exit 1
done
python3 tools/pcapng.py hostile tests/fuzz/corpus tests/pcapng/hostile_prefilter.pcapng
python3 tools/pcapng.py summary tests/pcapng/*.pcapng
./bin/functional --run test_pcapng_corpus_replay
```

`./bin/functional --run <name>` runs one test in-process (a bare name runs the whole suite).

| File | Covers |
| --- | --- |
| every capture | the connect handshake: request, cookie, request with cookie, challenge, response, first keep-alives |
| `test_harness_one_message_each_way_lossy` | unreliable traffic each way over loss, jitter and duplication |
| `test_ordered_small_and_large_interleaved` | reliable-ordered traffic, large messages carried as message-layer blocks, over loss, reorder and duplication |
| `test_reliable_mixed_sizes_over_lossy_link` | reliable traffic of mixed sizes over loss |
| `test_reliable_duplicate_after_gap_delivered_once` | held payloads released in a scripted order (the reorder hook) |
| `test_reliable_sequenced_held_record_released` | the same hook on a reliable-sequenced channel |
| `test_unreliable_receipts_over_lossy_link` | unreliable-with-receipt traffic over loss |
| `test_unreliable_at_most_once_over_duplicating_link` | a 50% duplicating link |
| `test_unreliable_dropped_when_builder_out_of_budget_never_arrives` | the send-drop case |
| `test_control_disconnect_burst_each_end` | the redundant disconnect burst (type 6) each way, client to server and server to client |
| `test_control_standalone_ack_each_end` | a standalone ack (type 8) the server sends once its ack debt reaches the threshold |
| `test_control_denied_when_server_full` | a denied (type 1) when the server's client slots are all full |
| `hostile_prefilter` | every `tests/fuzz/corpus/prefilter_*` seed less its selector byte, client to server: empty, too short, wrong epoch, wrong filter value, reserved type, an all-zero request, and three well-sized session packets |

The harness sends a standalone type 8 ack (once the ack debt reaches the threshold; the eliciting payloads are valid message payloads and are read, never refused), a denied (when the server has no free slot) and the full redundant disconnect burst (type 6, every copy) from either end, so those three types reach the readers from the functional harness as well as through the regressions. No captured case is left undone.

## The caps

Each file at most 128 KiB, `tests/pcapng/` (with `regress/`) at most 512 KiB in all. Both readers refuse a file over 128 KiB (`PCAPNG_FILE_OVER_CAP`), the C++ one after reading at most one 4 KiB chunk past the cap; `test_pcapng_corpus_replay` checks the total.

## What is stable

Captures are not byte-reproducible: the connection keys and salts are random per run, so every sealed byte differs. What a re-capture by the recipe reproduces is the count of packets per type and per end, and the count per refusal name, because the simulator's seed is pinned (`ROCKETNET_CHANNEL_SEED` unset) and no size or schedule depends on a key. `test_pcapng_corpus_replay` pins exactly that: for each captured file, every name a packet ended under and its count. A re-capture that changes a count is a behaviour change, and the pin in `tests/functional/corpus_consistency.cpp` is updated with the reason.

## The replay

`test_pcapng_corpus_replay` reads every `.pcapng` here and under `regress/`, with file input only: no sockets, no threads, and no time but the files' timestamps, each at or after 1000.0 s.

- An interface-0 frame is unwrapped (`PCAPNG_FRAME_NOT_IPV4_UDP` when it is not one) and its datagram goes through the receive ladder of the end it is addressed to, up to the first rung that needs a key or connection state: `rocketnet_prefilter_check`, the reserved types, the direction rule, the per-type wire size. A server-bound read is checked against `ROCKETNET_WIRE_SERVER_PACKET_VALID`, the kernel filter's rule. A connect request goes on through the fixed-offset version and protocol id comparisons into `rocketnet_read_connect_token`, `rocketnet_verify_signed_connect_data`, `rocketnet_open_connect_token_private` and `rocketnet_connect_token_private_consistent`, under the published test keys of `sdk/rocketnet_test_vectors.h` that the harness signs with; the opened private fields are wiped at once and never printed.
- An interface-1 record goes to its reader. An ack block is read with an honest sender's context (the block's own largest stands in for the receiver's highest sent), then the type data is held to its type's opened size. A message payload is read with the default message limits and every first-version channel type enabled.

Each packet ends read (`read <end> <what>`) or refused under the name of the counter the code fires (`sdk/rocketnet_counters.h`; a message payload's refusal is `MESSAGE_PROTOCOL_VIOLATION` plus the violation's name). A packet that ends neither way fails the test with its file and index. Under a counting allocator the replay's live bytes end at zero.

`test_pcapng_corpus_witness_unread_packet_goes_red` replays with one planted fault, the prefilter's refusal name dropped, and requires the replay to report the hostile datagrams the prefilter refuses as unnamed, each by file and index.
