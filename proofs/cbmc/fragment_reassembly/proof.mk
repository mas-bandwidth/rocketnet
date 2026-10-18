# Fragment reassembly proofs on the real sdk code. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the reader on the nightly list.

NIGHTLY += proofs-fragment_reassembly

FRAG_REASSEMBLY_SOURCE = fragment_reassembly/proof_fragment_reassembly.c
FRAG_REASSEMBLY_FLAGS = --cpp11 -DNDEBUG -I$(SDK)
FRAG_REASSEMBLY_UNWIND = 16

.PHONY: proofs-fragment_reassembly

proofs-fragment_reassembly:
	@echo "proofs-fragment_reassembly"; \
	set -e; \
	for harness in \
	    harness_header_and_plan \
	    harness_process_fragment \
	    harness_memory_sizing; do \
	    output=$$(mktemp); \
	    status=0; \
	    $(CBMC) $(CBMC_FLAGS) --unwind $(FRAG_REASSEMBLY_UNWIND) $(FRAG_REASSEMBLY_FLAGS) \
	        --function $$harness $(FRAG_REASSEMBLY_SOURCE) > "$$output" 2>&1 || status=$$?; \
	    if grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; cat "$$output"; \
	    elif grep -q "unwinding assertion" "$$output"; then \
	        verdict="unwinding assertion failed"; cat "$$output"; \
	    else \
	        verdict="FAILED (exit $$status)"; cat "$$output"; \
	    fi; \
	    echo "$$harness $$verdict"; \
	    rm -f "$$output"; \
	    if [ $$status -ne 0 ]; then exit 1; fi; \
	done
