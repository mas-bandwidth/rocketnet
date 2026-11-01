# CBMC proofs. `make -C proofs/cbmc` runs the proofs that finish:
# the arithmetic leaves below, then the bounded readers, the message reader,
# and the XDP free list.
#
# Every target prints the bound it proves beside the deployed constant the
# bound is measured against, so a bounded proof cannot read as the whole
# range.
#
# Arithmetic leaves:
#   replay-delta-1-3   the quick per-commit instance of harness_replay_near,
#                      admitting deltas 1, 2, and 3.
#   replay-near-4096   the full declared instance of the same source-bound
#                      harness, admitting deltas 1..4095. It first proves that
#                      the 13 clear-loop-shape and selected-word relation
#                      partitions are pairwise disjoint and jointly exhaustive
#                      over the full domain (with valid per-partition
#                      witnesses), then proves each partition with its
#                      intended-failure reachability control.
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
#   replay-far         proved: harness_replay_far in proof_replay_advance.c,
#                      unwind 65, sequence - highest_opened >= 4096: a delta of
#                      at least 4096 clears every word, then sets the new
#                      sequence bit. Runs against sdk/rocketnet_replay.cpp
#                      itself, which the harness includes.
#
# `ack-run-bound-8`, `ack-run-full-4096`, and `ack-run-negative-control`
# use ESBMC on the current C++ leaf in sdk/rocketnet_ack.cpp. They remain
# explicit targets until their exact-head results are recorded in
# full-bounds-results.md.

CBMC ?= cbmc

CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check \
             --unsigned-overflow-check --div-by-zero-check \
             --unwinding-assertions

SDK = ../../sdk

# The deployed constants each bound is printed beside, read from the sdk
# headers. A macro written as an arithmetic expression carries its deployed
# value in a trailing comment. Keep literal parentheses out of these shell
# calls: GNU make parses them even inside the quoted sed expression.
deployed_literal = $(shell grep -E '^\#define[[:space:]]+$(1)[[:space:]]+[0-9]+' $(SDK)/$(2) | awk '{print $$3}' | head -1)
deployed_comment = $(shell grep -E '^\#define[[:space:]]+$(1)[[:space:]]+' $(SDK)/$(2) | awk '{print $$NF}' | head -1)

REPLAY_WINDOW_BITS := $(call deployed_literal,ROCKETNET_HISTORY_PACKETS,rocketnet_history.h)
# Unwind for the replay proofs, derived from ROCKETNET_HISTORY_PACKETS (via
# ROCKETNET_REPLAY_WINDOW_BITS): one iteration per history word plus one.
REPLAY_UNWIND := $(shell expr $(REPLAY_WINDOW_BITS) / 64 + 1)
MAX_PAYLOAD_BYTES := $(call deployed_literal,ROCKETNET_MAX_PACKET_PAYLOAD_BYTES,rocketnet.h)
XDP_NUM_FRAMES := $(call deployed_literal,ROCKETNET_XDP_NUM_FRAMES,rocketnet_constants.h)
XDP_FRAME_SIZE := $(call deployed_literal,ROCKETNET_XDP_FRAME_SIZE,rocketnet_constants.h)

# Bounded readers, the message reader, and the XDP free
# list. Same flags as the arithmetic leaves. Each proof is a bounded
# instance, in the same sense as replay-delta-1-3:
#   read-write         proved: proof_read_write.c, unwind 9. Symbolic
#                      buffers of 8 bytes. Writers and readers round-trip;
#                      rocketnet_copy_string with
#                      dest_size 0 writes nothing and returns 0; a positive
#                      dest_size up to 8 keeps the copy inside the buffer
#                      and terminates it.
#   message-read       proved: proof_message_read.c, unwind 8, payloads of
#                      24 bytes. rocketnet_message_reader_begin / next read
#                      no byte past the given length. An accepted holdable
#                      record or block has its ring span inside
#                      receive_message_buffer_bytes. A refused payload yields no
#                      record. The fixed-size config body reader refuses a
#                      short buffer.
#   xdp-free-list      proved: proof_xdp_free_list.c, unwind 5, capacity 3,
#                      four push/pop steps. Push refuses a full list, a
#                      frame outside the region, and a frame not aligned to
#                      the 4096-byte XDP frame. Pop is the alloc-frame
#                      pattern (decrement, take the slot, store invalid).
#                      The list is exactly the frames pushed and not yet
#                      popped.

.PHONY: all replay-delta-1-3 replay-near-4096 replay-far reconstruct \
        read-write message-read xdp-free-list \
        ack-run-bound-8 ack-run-full-4096 ack-run-negative-control clean

all: replay-delta-1-3 replay-far reconstruct \
     read-write message-read xdp-free-list proofs-packet_open

# The quick per-commit instance remains explicit about its reduced bound.
replay-delta-1-3: proof_replay_advance.c
	@echo "replay-delta-1-3 bound delta 1..3 deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS); deltas 4..$(shell expr $(REPLAY_WINDOW_BITS) - 1) are not proved by this target"
	$(CBMC) $(CBMC_FLAGS) --unwind $(REPLAY_UNWIND) -I$(SDK) \
	    -DREPLAY_DELTA_BOUND=4 \
	    --function harness_replay_near proof_replay_advance.c

# The full declared near-window instance. Unwind 65 initializes all 64
# symbolic words and covers either production range-clear path. The path
# predicates partition, but do not reduce, the full delta 1..4095 domain.
CBMC_REPLAY_TIMEOUT ?= 1800
CBMC_REPLAY_MEMORY_KB ?= 16000000
CBMC_REPLAY_SOLVER ?= cadical

define run_replay_near_path
	@command -v $(CBMC) >/dev/null || { echo "replay-near-4096-$(1): $(CBMC) not on PATH" >&2; exit 1; }; \
	command -v $(CBMC_REPLAY_SOLVER) >/dev/null || { echo "replay-near-4096-$(1): $(CBMC_REPLAY_SOLVER) not on PATH" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	status=0; \
	( ulimit -v $(CBMC_REPLAY_MEMORY_KB); \
	  timeout $(CBMC_REPLAY_TIMEOUT) $(CBMC) $(CBMC_FLAGS) --unwind $(REPLAY_UNWIND) -I$(SDK) \
	    --external-sat-solver $(CBMC_REPLAY_SOLVER) \
	    -DREPLAY_DELTA_BOUND=$(REPLAY_WINDOW_BITS) \
	    --function $(2) proof_replay_advance.c ) \
	    > "$$work/proof.log" 2>&1 || status=$$?; \
	cat "$$work/proof.log"; \
	if [ $$status -eq 124 ]; then echo "replay-near-4096-$(1) HOLD timeout after $(CBMC_REPLAY_TIMEOUT)s"; exit 1; fi; \
	if [ $$status -eq 137 ] || grep -qi 'out of memory' "$$work/proof.log"; then echo "replay-near-4096-$(1) HOLD out of memory"; exit 1; fi; \
	if grep -q 'UNKNOWN' "$$work/proof.log"; then echo "replay-near-4096-$(1) HOLD UNKNOWN"; exit 1; fi; \
	if grep -Eqi 'unwinding assertion.*FAILURE|FAILURE.*unwinding assertion' "$$work/proof.log"; then echo "replay-near-4096-$(1) HOLD unwinding assertion failed"; exit 1; fi; \
	if [ $$status -ne 0 ] || ! grep -q '^VERIFICATION SUCCESSFUL' "$$work/proof.log"; then echo "replay-near-4096-$(1) HOLD exit=$$status"; exit 1; fi; \
	echo "replay-near-4096-$(1) VERIFICATION SUCCESSFUL full-domain=1..$$(expr $(REPLAY_WINDOW_BITS) - 1) partition=$(1) unwind=$(REPLAY_UNWIND)"; \
	status=0; \
	( ulimit -v $(CBMC_REPLAY_MEMORY_KB); \
	  timeout $(CBMC_REPLAY_TIMEOUT) $(CBMC) $(CBMC_FLAGS) --unwind $(REPLAY_UNWIND) -I$(SDK) \
	    --external-sat-solver $(CBMC_REPLAY_SOLVER) \
	    -DREPLAY_DELTA_BOUND=$(REPLAY_WINDOW_BITS) -DREPLAY_REACHABILITY_WITNESS=1 \
	    --function $(2) proof_replay_advance.c ) \
	    > "$$work/reachability.log" 2>&1 || status=$$?; \
	cat "$$work/reachability.log"; \
	if [ $$status -eq 124 ]; then echo "replay-near-4096-$(1)-reachability HOLD timeout after $(CBMC_REPLAY_TIMEOUT)s"; exit 1; fi; \
	if [ $$status -eq 137 ] || grep -qi 'out of memory' "$$work/reachability.log"; then echo "replay-near-4096-$(1)-reachability HOLD out of memory"; exit 1; fi; \
	if grep -q 'UNKNOWN' "$$work/reachability.log"; then echo "replay-near-4096-$(1)-reachability HOLD UNKNOWN"; exit 1; fi; \
	if grep -Eqi 'unwinding assertion.*FAILURE|FAILURE.*unwinding assertion' "$$work/reachability.log"; then echo "replay-near-4096-$(1)-reachability HOLD unwinding assertion failed"; exit 1; fi; \
	failures=$$(grep -c ': FAILURE$$' "$$work/reachability.log" || true); \
	if [ $$status -eq 10 ] && [ $$failures -eq 1 ] && \
	   grep -q '^VERIFICATION FAILED' "$$work/reachability.log" && \
	   grep -q 'replay path reachability negative control' "$$work/reachability.log"; then \
	  echo "replay-near-4096-$(1)-reachability EXPECTED FAILURE"; \
	else \
	  echo "replay-near-4096-$(1)-reachability HOLD: expected assertion did not fail alone (exit=$$status failures=$$failures)"; exit 1; \
	fi
endef

replay-near-4096: proof_replay_advance.c
	@echo "replay-near-4096 bound delta 1..$(shell expr $(REPLAY_WINDOW_BITS) - 1) deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS)"
	$(CBMC) $(CBMC_FLAGS) --unwind 1 -I$(SDK) \
	    -DREPLAY_DELTA_BOUND=$(REPLAY_WINDOW_BITS) \
	    --function harness_replay_near_partition_completeness proof_replay_advance.c
	@echo "replay-near-4096-partition-completeness VERIFICATION SUCCESSFUL full-domain=1..$(shell expr $(REPLAY_WINDOW_BITS) - 1) partitions=13"
	$(call run_replay_near_path,nw_empty_clear,harness_replay_near_nw_empty_clear)
	$(call run_replay_near_path,nw_single_partial_cleared_set,harness_replay_near_nw_single_partial_cleared_set)
	$(call run_replay_near_path,nw_single_partial_cleared_only,harness_replay_near_nw_single_partial_cleared_only)
	$(call run_replay_near_path,nw_single_partial_set_only,harness_replay_near_nw_single_partial_set_only)
	$(call run_replay_near_path,nw_single_partial_untouched,harness_replay_near_nw_single_partial_untouched)
	$(call run_replay_near_path,nw_multi_first_word,harness_replay_near_nw_multi_first_word)
	$(call run_replay_near_path,nw_multi_interior_word,harness_replay_near_nw_multi_interior_word)
	$(call run_replay_near_path,nw_multi_last_word,harness_replay_near_nw_multi_last_word)
	$(call run_replay_near_path,nw_multi_outside_word,harness_replay_near_nw_multi_outside_word)
	$(call run_replay_near_path,wrap_coincident_word,harness_replay_near_wrap_coincident_word)
	$(call run_replay_near_path,wrap_high_span_only,harness_replay_near_wrap_high_span_only)
	$(call run_replay_near_path,wrap_low_span_only,harness_replay_near_wrap_low_span_only)
	$(call run_replay_near_path,wrap_outside_spans,harness_replay_near_wrap_outside_spans)

replay-far: proof_replay_advance.c
	@echo "replay-far bound delta >= $(REPLAY_WINDOW_BITS) deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS)"
	$(CBMC) $(CBMC_FLAGS) --unwind $(REPLAY_UNWIND) -I$(SDK) \
	    --function harness_replay_far proof_replay_advance.c

reconstruct: proof_reconstruct_sequence.c
	@echo "reconstruct bound 32-bit and 24-bit wire half-ranges deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS)"
	$(CBMC) $(CBMC_FLAGS) --unwind 1 proof_reconstruct_sequence.c

ESBMC ?= esbmc
ACK_RUN_SOURCE = $(CURDIR)/proof_ack_apply_run.c
ACK_RUN_FLAGS = --std c++17 --z3 --overflow-check --ub-shift-check \
                -DROCKETNET_ESBMC -DNDEBUG -DROCKETNET_DEV=1 \
                -I$(SDK) --include-file $(CURDIR)/esbmc_shim.h
ACK_RUN_TIMEOUT ?= 1800
ACK_RUN_MEMORY_KB ?= 16000000

define run_ack_proof
	@command -v $(ESBMC) >/dev/null || { echo "$(1): $(ESBMC) not on PATH" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	printf '#include "%s"\n' "$(ACK_RUN_SOURCE)" > "$$work/proof.cpp"; \
	status=0; \
	( ulimit -v $(ACK_RUN_MEMORY_KB); \
	  timeout $(ACK_RUN_TIMEOUT) $(ESBMC) $(ACK_RUN_FLAGS) --unwind $(3) \
	    -DACK_RUN_BOUND=$(2) --function harness_ack_apply_run \
	    "$$work/proof.cpp" ) > "$$work/output.log" 2>&1 || status=$$?; \
	cat "$$work/output.log"; \
	if [ $$status -eq 124 ]; then echo "$(1) HOLD timeout after $(ACK_RUN_TIMEOUT)s"; exit 1; fi; \
	if [ $$status -eq 137 ] || grep -qi 'out of memory' "$$work/output.log"; then echo "$(1) HOLD out of memory"; exit 1; fi; \
	if grep -q 'UNKNOWN' "$$work/output.log"; then echo "$(1) HOLD UNKNOWN"; exit 1; fi; \
	if grep -A3 '^Violated property' "$$work/output.log" | grep -q 'unwinding assertion'; then echo "$(1) HOLD unwinding assertion failed"; exit 1; fi; \
	if [ $$status -eq 0 ] && grep -q '^VERIFICATION SUCCESSFUL' "$$work/output.log"; then \
	  echo "$(1) VERIFICATION SUCCESSFUL bound=$(2) unwind=$(3)"; \
	else \
	  echo "$(1) HOLD exit=$$status bound=$(2) unwind=$(3)"; exit 1; \
	fi
endef

ack-run-bound-8: proof_ack_apply_run.c esbmc_shim.h
	$(call run_ack_proof,ack-run-bound-8,8,65)

ack-run-full-4096: proof_ack_apply_run.c esbmc_shim.h
	$(call run_ack_proof,ack-run-full-4096,$(REPLAY_WINDOW_BITS),4097)

ack-run-negative-control: proof_ack_apply_run.c esbmc_shim.h
	@command -v $(ESBMC) >/dev/null || { echo "ack-run-negative-control: $(ESBMC) not on PATH" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	printf '#include "%s"\n' "$(ACK_RUN_SOURCE)" > "$$work/proof.cpp"; \
	status=0; \
	( ulimit -v $(ACK_RUN_MEMORY_KB); \
	  timeout $(ACK_RUN_TIMEOUT) $(ESBMC) $(ACK_RUN_FLAGS) --unwind 65 \
	    -DACK_RUN_BOUND=8 -DACK_NEGATIVE_CONTROL=1 \
	    --function harness_ack_apply_run "$$work/proof.cpp" ) \
	    > "$$work/output.log" 2>&1 || status=$$?; \
	cat "$$work/output.log"; \
	if [ $$status -eq 124 ]; then echo "ack-run-negative-control HOLD timeout after $(ACK_RUN_TIMEOUT)s"; exit 1; fi; \
	if [ $$status -eq 137 ] || grep -qi 'out of memory' "$$work/output.log"; then echo "ack-run-negative-control HOLD out of memory"; exit 1; fi; \
	if grep -q 'UNKNOWN' "$$work/output.log"; then echo "ack-run-negative-control HOLD UNKNOWN"; exit 1; fi; \
	if grep -A3 '^Violated property' "$$work/output.log" | grep -q 'unwinding assertion'; then echo "ack-run-negative-control HOLD unwinding assertion failed"; exit 1; fi; \
	if [ $$status -eq 1 ] && \
	   grep -q '^VERIFICATION FAILED' "$$work/output.log" && \
	   grep -q 'negative control reaches the production ACK operation' "$$work/output.log" && \
	   grep -Eq '\*\* 1 of [0-9]+ properties failed' "$$work/output.log"; then \
	  echo "ack-run-negative-control EXPECTED FAILURE"; \
	else \
	  echo "ack-run-negative-control HOLD: expected assertion did not fail (exit=$$status)"; exit 1; \
	fi

read-write: proof_read_write.c
	@echo "read-write bound buffers 8 bytes deployed ROCKETNET_MAX_PACKET_PAYLOAD_BYTES=$(MAX_PAYLOAD_BYTES)"
	$(CBMC) $(CBMC_FLAGS) --unwind 9 -I$(SDK) proof_read_write.c

message-read: proof_message_read.c
	@echo "message-read bound payload 24 bytes deployed ROCKETNET_MAX_PACKET_PAYLOAD_BYTES=$(MAX_PAYLOAD_BYTES)"
	$(CBMC) $(CBMC_FLAGS) --unwind 8 -DNDEBUG -I$(SDK) proof_message_read.c

xdp-free-list: proof_xdp_free_list.c
	@echo "xdp-free-list bound capacity 3 frames deployed ROCKETNET_XDP_NUM_FRAMES=$(XDP_NUM_FRAMES) frames of $(XDP_FRAME_SIZE) bytes"
	$(CBMC) $(CBMC_FLAGS) --unwind 5 -I$(SDK) proof_xdp_free_list.c

# Paired reachability controls. Each per-commit target above has a
# `<target>-reachability` partner that recompiles the same harness with a
# guarded __CPROVER_assert(0) after the production call(s). The control
# passes only when CBMC reports exactly one failure, on the named control
# assertion, with VERIFICATION FAILED; anything else is a HOLD.
define run_reachability
	@work=$$(mktemp -d); trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	status=0; \
	( $(CBMC) $(CBMC_FLAGS) --unwind $(2) $(3) $(4) ) > "$$work/out.log" 2>&1 || status=$$?; \
	cat "$$work/out.log"; \
	failures=$$(grep -c ': FAILURE$$' "$$work/out.log" || true); \
	if [ $$status -eq 10 ] && [ $$failures -eq 1 ] && \
	   grep -q '^VERIFICATION FAILED' "$$work/out.log" && \
	   grep -q '$(5)' "$$work/out.log"; then \
	  echo "$(1)-reachability EXPECTED FAILURE"; \
	else \
	  echo "$(1)-reachability HOLD: expected exactly one named failure (exit=$$status failures=$$failures)"; exit 1; \
	fi
endef

.PHONY: controls replay-delta-1-3-reachability replay-far-reachability reconstruct-reachability \
        read-write-reachability message-read-reachability xdp-free-list-reachability

# The per-commit set runs every control beside its proof: a harness whose
# assumptions became unsatisfiable proves anything, and only its control,
# which must reach the code past them, can go red.

CONTROLS = replay-delta-1-3-reachability replay-far-reachability reconstruct-reachability \
           read-write-reachability message-read-reachability xdp-free-list-reachability

controls: $(CONTROLS)

all: controls

replay-delta-1-3-reachability: proof_replay_advance.c
	$(call run_reachability,replay-delta-1-3,$(REPLAY_UNWIND),-I$(SDK) -DREPLAY_DELTA_BOUND=4 -DREPLAY_REACHABILITY_WITNESS=1 --function harness_replay_near proof_replay_advance.c,,replay path reachability negative control)

replay-far-reachability: proof_replay_advance.c
	$(call run_reachability,replay-far,$(REPLAY_UNWIND),-I$(SDK) -DREPLAY_FAR_REACHABILITY=1 --function harness_replay_far proof_replay_advance.c,,replay-far reachability control)

reconstruct-reachability: proof_reconstruct_sequence.c
	$(call run_reachability,reconstruct,1,-DREACHABILITY_CONTROL=1 proof_reconstruct_sequence.c,,reconstruct reachability control)

read-write-reachability: proof_read_write.c
	$(call run_reachability,read-write,9,-I$(SDK) -DREACHABILITY_CONTROL=1 proof_read_write.c,,read-write reachability control)

message-read-reachability: proof_message_read.c
	$(call run_reachability,message-read,8,-DNDEBUG -I$(SDK) -DREACHABILITY_CONTROL=1 proof_message_read.c,,message-read reachability control)

xdp-free-list-reachability: proof_xdp_free_list.c
	$(call run_reachability,xdp-free-list,5,-I$(SDK) -DREACHABILITY_CONTROL=1 proof_xdp_free_list.c,,xdp-free-list reachability control)

clean:
	rm -f *.goto *.output

# Nightly proofs. Each reader directory holds a proof.mk that appends its
# target to NIGHTLY, so a CBMC card adds a reader without editing a shared
# file. The `all` target above is the per-commit set and is unchanged; the
# nightly target runs the longer proofs that are not part of CI.

NIGHTLY :=

-include $(wildcard */proof.mk)

# The full near-window proof runs nightly, not per commit (see
# .github/workflows/nightly-proofs.yml and the HOLD in INVENTORY.md).
NIGHTLY += replay-near-4096

.PHONY: nightly

nightly: $(NIGHTLY)
