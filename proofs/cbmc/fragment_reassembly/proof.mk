# Fragment reassembly proofs on the real sdk code. Included by
# proofs/cbmc/Makefile through the nightly scaffold; this file alone puts
# the reader on the nightly list. Nightly only: never part of `all`.
#
# The checker is ESBMC (8.5 or later on PATH), not CBMC: the harness
# includes sdk/rocketnet_fragment.cpp, which is C++ the CBMC front end does
# not parse. The harness is built as C++17 through a one-line wrapper, with
# esbmc_cprover.h mapping the __CPROVER_ names onto ESBMC. One line of
# output per harness, then its unwinding, bound, property count and time.
#
# proofs-fragment_reassembly runs each harness at the largest bound that
# finishes (results.md). proofs-fragment_reassembly-full runs the full
# production bounds, which do not finish; it is not on the nightly list.
# Each harness runs under a 30-minute limit and a 16 GB address-space limit.

NIGHTLY += proofs-fragment_reassembly

ESBMC ?= esbmc

FRAG_REASSEMBLY_SOURCE = $(CURDIR)/fragment_reassembly/proof_fragment_reassembly.c
FRAG_REASSEMBLY_SHIM = $(CURDIR)/esbmc_shim.h
FRAG_REASSEMBLY_FLAGS = --std c++17 --overflow-check --ub-shift-check \
                        -I$(SDK) --include-file $(FRAG_REASSEMBLY_SHIM)
FRAG_REASSEMBLY_TIMEOUT = 1800
FRAG_REASSEMBLY_MEMORY_KB = 16000000

# The proved bounds. The process harness takes fragments at the full
# receive bound and max_payload_bytes up to 40; the sizing harness takes
# every argument the transports can pass. The full target empties the first
# (the harness defaults to ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES) and sets
# PROOF_SIZING_FULL on the second.
FRAG_REASSEMBLY_PROCESS_BOUND = ,-DPROOF_MAX_PAYLOAD=40
FRAG_REASSEMBLY_SIZING_BOUND =

# harness:unwind:extra flags (comma for space). Unwind 33 covers the
# harness's loops over the 32-byte received bitmap; the library memset loop
# gets 64, past the 56-byte slot record the reader clears. The memory
# sizing harness adds the unsigned overflow check: its claim is that no
# sizing sum or product wraps. The process and sizing harnesses run on
# Boolector: since ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES became 1413
# (c613a6a8), Bitwuzla 0.9.0 runs out of the 16 GB limit on both within a
# minute, with the code and the harness unchanged (results.md).
FRAG_REASSEMBLY_HARNESSES = \
    harness_header_and_plan:16:--z3 \
    harness_process_fragment:33:--boolector,--unwindsetname,__memset_impl:0:64$(FRAG_REASSEMBLY_PROCESS_BOUND) \
    harness_memory_sizing:16:--boolector,--unsigned-overflow-check$(FRAG_REASSEMBLY_SIZING_BOUND)

.PHONY: proofs-fragment_reassembly proofs-fragment_reassembly-full

proofs-fragment_reassembly-full:
	$(MAKE) proofs-fragment_reassembly FRAG_REASSEMBLY_PROCESS_BOUND= FRAG_REASSEMBLY_SIZING_BOUND=,-DPROOF_SIZING_FULL

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
	    ( ulimit -v $(FRAG_REASSEMBLY_MEMORY_KB); \
	      timeout $(FRAG_REASSEMBLY_TIMEOUT) $(ESBMC) $(FRAG_REASSEMBLY_FLAGS) --unwind $$unwind $$extra \
	        --function $$harness "$$work/proof.cpp" ) > "$$output" 2>&1 || status=$$?; \
	    seconds=$$(( $$(date +%s) - start )); \
	    if [ $$status -eq 124 ]; then \
	        verdict="timed out after $(FRAG_REASSEMBLY_TIMEOUT) s"; \
	    elif grep -q "^VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -A3 "^Violated property" "$$output" | grep -q "unwinding assertion"; then \
	        verdict="unwinding assertion failed"; \
	    elif grep -q "^VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    elif grep -q "Out of memory" "$$output"; then \
	        verdict="out of memory (solver)"; \
	    else \
	        verdict="FAILED (exit $$status)"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then grep -v "^WARNING" "$$output" | tail -n 40; failed=1; fi; \
	    echo "$$harness $$verdict"; \
	    echo "  unwind $$unwind $$extra; $$(grep -E 'of [0-9]+ properties' "$$output" | head -1 | sed 's/^ *//'); $$seconds s"; \
	done; \
	exit $$failed
