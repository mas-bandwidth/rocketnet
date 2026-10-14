# CBMC proofs of the arithmetic leaves.
#
# `make -C proofs/cbmc` runs only the proofs that finish:
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

.PHONY: all replay-delta-1-3 replay-far reconstruct \
        slow-unproved replay-near-4096 ack-run-bound-8 clean

all: replay-delta-1-3 replay-far reconstruct

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

clean:
	rm -f *.goto *.output
