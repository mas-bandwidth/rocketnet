# Sequence-buffer proofs. Included by the nightly scaffold only.

NIGHTLY += proofs-sequence_buffers

SEQUENCE_BUFFERS_SOURCE = sequence_buffers/proof_sequence_buffers.c
SEQUENCE_BUFFERS_CPP = sequence_buffers/.proof_sequence_buffers.esbmc.cpp
SEQUENCE_BUFFERS_FLAGS = --std c++17 --z3 --overflow-check -DROCKETNET_ESBMC -DNDEBUG -I$(SDK) -I.
ESBMC ?= esbmc
ESBMC_RUN ?= $(ESBMC)

.PHONY: proofs-sequence_buffers

proofs-sequence_buffers:
	@cp $(SEQUENCE_BUFFERS_SOURCE) $(SEQUENCE_BUFFERS_CPP); \
	trap 'rm -f $(SEQUENCE_BUFFERS_CPP)' EXIT HUP INT TERM; \
	overall=0; \
	for spec in \
	    harness_replay_already_received:2 \
	    harness_replay_advance:4097 \
	    harness_ack_record_received:2 \
	    harness_ack_record_sent_classified:2 \
	    harness_ack_mark_keep_alive:2 \
	    harness_window_receive_init:2 \
	    harness_window_receive_accept:1048577 \
	    harness_window_receive_release:2; do \
	    harness=$${spec%%:*}; unwind=$${spec#*:}; \
	    output=$${SEQUENCE_BUFFERS_LOG_DIR:-sequence_buffers}/$$harness.log; \
	    mkdir -p "$$(dirname "$$output")"; \
	    status=0; \
	    timeout 1800s $(ESBMC_RUN) $(SEQUENCE_BUFFERS_FLAGS) \
	        --unwind $$unwind --function $$harness \
	        $(SEQUENCE_BUFFERS_CPP) > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    elif grep -qi "unwinding assertion" "$$output"; then \
	        verdict="unwinding assertion failed"; \
	    elif [ $$status -eq 152 ] || [ $$status -eq 137 ] || [ $$status -eq 124 ]; then \
	        verdict="timed out or resource limited (exit $$status)"; \
	    else \
	        verdict="FAILED (exit $$status)"; \
	    fi; \
	    echo "$$harness $$verdict"; \
	    if [ $$status -ne 0 ]; then overall=1; fi; \
	done; \
	exit $$overall
