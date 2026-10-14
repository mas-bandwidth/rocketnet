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
	@status=0; \
	$(CBMC) $(CBMC_FLAGS) --unwind 65 --function harness_replay_far \
	    proof_replay_advance.c || status=$$?; \
	if [ $$status -eq 10 ]; then \
	    echo "expected-fail: far advance counterexample (see proof_replay_advance.expected-fail)"; \
	    exit 0; \
	fi; \
	if [ $$status -eq 0 ]; then \
	    echo "expected-fail marker is stale: harness_replay_far proved"; \
	    exit 1; \
	fi; \
	exit $$status

reconstruct: proof_reconstruct_sequence.c
	$(CBMC) $(CBMC_FLAGS) --unwind 1 proof_reconstruct_sequence.c

ack: proof_ack_apply_run.c
	$(CBMC) $(CBMC_FLAGS) --unwind $(ACK_RUN_BOUND) \
	    -DACK_RUN_BOUND=$(ACK_RUN_BOUND) \
	    proof_ack_apply_run.c

clean:
	rm -f *.goto *.output
