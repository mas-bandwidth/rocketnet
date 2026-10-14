# CBMC proofs for the arithmetic leaves.
#
# `make -C proofs/cbmc` proves the near replay advance, the two sequence
# reconstructions, and the ack run, and it keeps the far replay advance as
# an expected counterexample (see proof_replay_advance.expected-fail).
#
# Complete unwinding bounds, also stated in the harness comments:
#   replay near   4096  (every delta below the 4096-bit window)
#   replay far      65  (64 word checks, no data-dependent loop)
#   reconstruct      1  (straight-line)
#   ack run       4096  (one pass over the real sent window)
# REPLAY_DELTA_BOUND and ACK_RUN_BOUND shrink a proof to that many steps
# when the complete bound does not fit in the five-minute budget. The
# replay near unwind stays at least 64, because the word-mask reference
# runs once per word.

CBMC ?= cbmc

CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check \
             --unsigned-overflow-check --div-by-zero-check \
             --unwinding-assertions

REPLAY_DELTA_BOUND ?= 4096
ACK_RUN_BOUND ?= 4096

.PHONY: all replay-near replay-far reconstruct ack clean

all: replay-near replay-far reconstruct ack

replay-near: proof_replay_advance.c
	$(CBMC) $(CBMC_FLAGS) --unwind $(REPLAY_DELTA_BOUND) \
	    -DREPLAY_DELTA_BOUND=$(REPLAY_DELTA_BOUND) \
	    --function harness_replay_near proof_replay_advance.c

replay-far: proof_replay_advance.c proof_replay_advance.expected-fail
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
	python3 -c 'import json,sys; data=json.load(open(sys.argv[1])); properties=[p for group in data if isinstance(group,dict) for p in group.get("result",[]) if isinstance(p,dict)]; bad=[p for p in properties if p.get("status") not in ("SUCCESS","UNREACHABLE")]; expected="a delta at least the width clears every word"; ok=len(bad)==1 and bad[0].get("status")=="FAILURE" and expected in bad[0].get("description","") and "harness_replay_far" in bad[0].get("property",""); print("expected-fail: far advance counterexample (see proof_replay_advance.expected-fail)" if ok else "replay-far: unrelated or missing failed property", file=sys.stdout if ok else sys.stderr); sys.exit(0 if ok else 1)' "$$output"

reconstruct: proof_reconstruct_sequence.c
	$(CBMC) $(CBMC_FLAGS) --unwind 1 proof_reconstruct_sequence.c

ack: proof_ack_apply_run.c
	$(CBMC) $(CBMC_FLAGS) --unwind $(ACK_RUN_BOUND) \
	    -DACK_RUN_BOUND=$(ACK_RUN_BOUND) \
	    proof_ack_apply_run.c

clean:
	rm -f *.goto *.output
