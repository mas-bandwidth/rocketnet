# CBMC proofs. `make -C proofs/cbmc` runs the proofs that finish:
# the arithmetic leaves below, then the bounded readers, fragment bounds,
# the message reader, and the XDP free list.
#
# Arithmetic leaves:
#   replay-delta-1-3   proved: harness_replay_near in proof_replay_advance.c,
#                      unwind 65, 4096-bit (64-word) window, admitted deltas
#                      1, 2, and 3 (sequence > highest_opened and
#                      sequence - highest_opened < 4); one arbitrary word
#                      stands for every word. Deltas 4..4095 are not proved.
#   reconstruct        proved: proof_reconstruct_sequence.c, unwind 1:
#                      - rocketnet_reconstruct_near (k=32, 2^32 <= expected <
#                        2^64 - 2^32): keeps wire low 32 bits, |result -
#                        expected| <= 2^31, round-trips every in-window value
#                        (tie at 2^31 goes to the larger candidate).
#                      - rocketnet_reconstruct_sequence (reference =
#                        highest_opened + 1, 2^32 - 1 <= highest_opened <
#                        2^64 - 2^32 - 1): same claims.
#                      - rocketnet_reconstruct_u24 (k=24 signed half-range,
#                        2^23 <= reference <= 2^64 - 2^23): keeps wire low 24
#                        bits, |result - reference| <= 2^23, round-trips every
#                        in-window value (tie at 2^23 goes to the lower
#                        candidate).
#   replay-far         expected counterexample (not a passing proof):
#                      harness_replay_far in proof_replay_advance.c, unwind 65,
#                      sequence - highest_opened >= 4096. Fails at
#                      sdk/rocketnet_replay.cpp because the far branch clears
#                      window[0]..window[3] only, not all 64 words (fix card:
#                      rocketnet-replay-far-advance-clears-every-window-word).
#
# `make -C proofs/cbmc slow-unproved` runs replay-near-4096 and ack-run-bound-8.
# Neither is proved, and neither is in the default target or CI:
#   replay-near-4096   not proved: harness_replay_near with REPLAY_DELTA_BOUND
#                      4096 and unwind 4096 (deltas 1..4095).
#   ack-run-bound-8    not proved: proof_ack_apply_run.c with ACK_RUN_BOUND 8
#                      and unwind 8 (runs of length 1..8 inside the 4096-record
#                      sent window); the full 4096 bound is also not proved.

CBMC ?= cbmc

CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check \
             --unsigned-overflow-check --div-by-zero-check \
             --unwinding-assertions

# Bounded readers, fragment bounds, the message reader, and the XDP free
# list. Same flags as the arithmetic leaves. Each proof is a bounded
# instance, in the same sense as replay-delta-1-3:
#   read-write         proved: proof_read_write.c, unwind 9. Symbolic
#                      buffers of 8 bytes. Writers and readers round-trip;
#                      the bounded readers, including the variable-length
#                      byte reader, refuse a read past the end pointer and
#                      leave the cursor alone. A negative byte count is
#                      refused and writes nothing. These readers are a
#                      contract for helpers the wire path does not call yet.
#                      rocketnet_copy_string with dest_size 0 writes nothing
#                      and returns 0; a positive dest_size up to 8 keeps the
#                      copy inside the buffer and terminates it. The backing
#                      object for a bounded read is 8 bytes of slack past the
#                      end pointer. The proof shows no dereference past that
#                      end. It does not show that the guard's pointer add
#                      stays inside a caller's object that ends at the end
#                      pointer.
#   fragment-bounds    proved: proof_fragment_bounds.c, unwind 64, one
#                      connection and one slot. Fragment payloads are
#                      symbolic up to 8 bytes, or outside the legal size
#                      window (refused, no read). max_payload_bytes is
#                      symbolic from 1 to 16. A reconstructed total above
#                      that maximum is refused and does not write the arena.
#                      It does not evict a different payload. The same
#                      payload id clears the slot. A final that arrives
#                      while the stride is 0 is staged, and the payload
#                      region is untouched. Counts above 8-byte fragments
#                      and payload maxima above 16 are not proved. Any wire
#                      id below the slot occupant is out of scope too: the
#                      ordinary older-id path and the uint32 rollover half
#                      of the stale check. The overflow check stays on. A
#                      regression in slot_arena_bytes = max_payload_bytes +
#                      ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES would not be
#                      caught. The recipe diffs the copied functions against
#                      sdk/rocketnet_fragment.cpp. CBMC does not parse that
#                      file.
#   message-read       proved: proof_message_read.c, unwind 8, payloads of
#                      24 bytes. rocketnet_message_reader_begin / next read
#                      no byte past the given length. An accepted holdable
#                      record or block has its ring span inside
#                      receive_ring_bytes. A refused payload yields no
#                      record. The fixed-size config body reader refuses a
#                      short buffer.
#   xdp-free-list      proved: proof_xdp_free_list.c, unwind 5, capacity 3,
#                      four push/pop steps. The proof covers push, the real
#                      header function. Pop is a transcription of the static
#                      alloc_send_frame / alloc_receive_frame pattern, not
#                      those functions compiled. Push refuses a full list, a
#                      frame outside the region, and a frame not aligned to
#                      the 4096-byte XDP frame. Pop decrements, takes the
#                      slot, and stores invalid. The whole array is the
#                      frames pushed and not yet popped, and every other
#                      slot is invalid.

.PHONY: all replay-delta-1-3 replay-far reconstruct \
        read-write fragment-bounds message-read xdp-free-list \
        slow-unproved replay-near-4096 ack-run-bound-8 clean

all: replay-delta-1-3 replay-far reconstruct \
     read-write fragment-bounds message-read xdp-free-list

# delta = sequence - highest_opened is at least 1 and strictly less than 4,
# so the admitted deltas are 1, 2, and 3. Unwind 65 initializes all 64
# symbolic window words; the clear loop runs at most twice.
replay-delta-1-3: proof_replay_advance.c
	$(CBMC) $(CBMC_FLAGS) --unwind 65 \
	    -DREPLAY_DELTA_BOUND=4 \
	    --function harness_replay_near proof_replay_advance.c

replay-far: proof_replay_advance.c
	@output=$$(mktemp); \
	trap 'rm -f "$$output"' EXIT HUP INT TERM; \
	status=0; \
	$(CBMC) $(CBMC_FLAGS) --unwind 65 --function harness_replay_far \
	    --json-ui --trace proof_replay_advance.c > "$$output" 2>&1 || status=$$?; \
	cat "$$output"; \
	if [ $$status -ne 10 ]; then \
	    echo "replay-far: expected CBMC verification failure (exit 10), got $$status" >&2; \
	    exit 1; \
	fi; \
	python3 -c 'import json,sys; data=json.load(open(sys.argv[1])); properties=[p for group in data if isinstance(group,dict) for p in group.get("result",[]) if isinstance(p,dict)]; bad=[p for p in properties if p.get("status") not in ("SUCCESS","UNREACHABLE")]; expected="a delta at least the width clears every word"; ok=len(bad)==1 and bad[0].get("status")=="FAILURE" and expected in bad[0].get("description","") and "harness_replay_far" in bad[0].get("property",""); print("expected-fail: far advance counterexample (rocketnet-replay-far-advance-clears-every-window-word)" if ok else "replay-far: unrelated or missing failed property", file=sys.stdout if ok else sys.stderr); sys.exit(0 if ok else 1)' "$$output"

reconstruct: proof_reconstruct_sequence.c
	$(CBMC) $(CBMC_FLAGS) --unwind 1 proof_reconstruct_sequence.c

# Not proved. Not part of `all`.
slow-unproved: replay-near-4096 ack-run-bound-8

replay-near-4096: proof_replay_advance.c
	$(CBMC) $(CBMC_FLAGS) --unwind 4096 \
	    -DREPLAY_DELTA_BOUND=4096 \
	    --function harness_replay_near proof_replay_advance.c

ack-run-bound-8: proof_ack_apply_run.c
	$(CBMC) $(CBMC_FLAGS) --unwind 8 \
	    -DACK_RUN_BOUND=8 \
	    proof_ack_apply_run.c

SDK = ../../sdk

read-write: proof_read_write.c
	$(CBMC) $(CBMC_FLAGS) --unwind 9 -I$(SDK) proof_read_write.c

fragment-bounds: proof_fragment_bounds.c check_fragment_copy.py $(SDK)/rocketnet_fragment.cpp
	python3 check_fragment_copy.py
	$(CBMC) $(CBMC_FLAGS) --unwind 64 -I$(SDK) proof_fragment_bounds.c

message-read: proof_message_read.c
	$(CBMC) $(CBMC_FLAGS) --unwind 8 -DNDEBUG -I$(SDK) proof_message_read.c

xdp-free-list: proof_xdp_free_list.c
	$(CBMC) $(CBMC_FLAGS) --unwind 5 -I$(SDK) proof_xdp_free_list.c

clean:
	rm -f *.goto *.output
