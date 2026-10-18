# Fragment reassembly proofs on the real sdk code. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the reader on the nightly list. Nightly only: never part of `all`.
#
# The checker is ESBMC (8.5 or later on PATH), not CBMC: the harness
# includes sdk/rocketnet_fragment.cpp, which is C++ the CBMC front end does
# not parse. The harness is built as C++17 through a one-line wrapper, with
# esbmc_cprover.h mapping the __CPROVER_ names onto ESBMC. Each harness
# runs at the full production bound, under a 30-minute limit; one line of
# output per harness, then its property count and time.

NIGHTLY += proofs-fragment_reassembly

ESBMC ?= esbmc

FRAG_REASSEMBLY_SOURCE = $(CURDIR)/fragment_reassembly/proof_fragment_reassembly.c
FRAG_REASSEMBLY_SHIM = $(CURDIR)/fragment_reassembly/esbmc_cprover.h
FRAG_REASSEMBLY_FLAGS = --std c++17 --overflow-check --ub-shift-check \
                        -DNDEBUG -I$(SDK) --include-file $(FRAG_REASSEMBLY_SHIM)
FRAG_REASSEMBLY_TIMEOUT = 1800

# harness:unwind:extra flags (comma for space). Unwind 33 covers the
# harness's loops over the 32-byte received bitmap; the library memset loop
# gets 64, past the 56-byte slot record the reader clears. The memory
# sizing harness adds the unsigned overflow check: its claim is that no
# sizing sum or product wraps.
FRAG_REASSEMBLY_HARNESSES = \
    harness_header_and_plan:16:--z3 \
    harness_process_fragment:33:--unwindsetname,__memset_impl:0:64 \
    harness_memory_sizing:16:--unsigned-overflow-check

.PHONY: proofs-fragment_reassembly

proofs-fragment_reassembly:
	@echo "proofs-fragment_reassembly"; \
	command -v $(ESBMC) > /dev/null || { echo "proofs-fragment_reassembly: $(ESBMC) not on PATH" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	printf '#include "%s"\n' "$(FRAG_REASSEMBLY_SOURCE)" > "$$work/proof.cpp"; \
	failed=0; \
	for entry in $(FRAG_REASSEMBLY_HARNESSES); do \
	    harness=$${entry%%:*}; rest=$${entry#*:}; \
	    unwind=$${rest%%:*}; extra=$$(echo "$${rest#*:}" | tr ',' ' '); \
	    output="$$work/$$harness.log"; \
	    start=$$(date +%s); status=0; \
	    timeout $(FRAG_REASSEMBLY_TIMEOUT) $(ESBMC) $(FRAG_REASSEMBLY_FLAGS) --unwind $$unwind $$extra \
	        --function $$harness "$$work/proof.cpp" > "$$output" 2>&1 || status=$$?; \
	    seconds=$$(( $$(date +%s) - start )); \
	    if [ $$status -eq 124 ]; then \
	        verdict="timed out after $(FRAG_REASSEMBLY_TIMEOUT) s"; \
	    elif grep -q "^VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -A3 "^Violated property" "$$output" | grep -q "unwinding assertion"; then \
	        verdict="unwinding assertion failed"; \
	    elif grep -q "^VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    else \
	        verdict="FAILED (exit $$status)"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then cat "$$output"; failed=1; fi; \
	    echo "$$harness $$verdict"; \
	    echo "  unwind $$unwind $$extra; $$(grep -E 'properties (failed|passed)|of [0-9]+ properties' "$$output" | head -1 | sed 's/^ *//'); $$seconds s"; \
	done; \
	exit $$failed
