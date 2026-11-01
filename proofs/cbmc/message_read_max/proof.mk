# Message reader and config body reader proofs at symbolic length bounds on the
# real SDK code (sdk/rocketnet_message.cpp). Included by proofs/cbmc/Makefile
# through the nightly scaffold; never part of `all`.

NIGHTLY += proofs-message_read_max

MESSAGE_READ_MAX_SOURCE = $(CURDIR)/message_read_max/proof_message_read_max.c
MESSAGE_READ_MAX_FLAGS = --std c++17 --overflow-check --ub-shift-check \
                         -DROCKETNET_ESBMC -DNDEBUG -I$(SDK) --include-file $(CURDIR)/esbmc_shim.h \
                         --unwindsetname __memset_impl:0:64
MESSAGE_READ_MAX_TIMEOUT = 1800

# harness:unwind:extra flags (comma for space)
MESSAGE_READ_MAX_HARNESSES = \
    harness_message_reader:7:--bitwuzla,-DW_PARTITION=1 \
    harness_message_reader:7:--bitwuzla,-DW_PARTITION=2 \
    harness_message_reader_single_max:2:--z3 \
    harness_message_read_config_body:1:--z3

.PHONY: proofs-message_read_max

proofs-message_read_max:
	@echo "proofs-message_read_max"; \
	command -v $(ESBMC) > /dev/null || [ -x "$(ESBMC)" ] || { echo "proofs-message_read_max: $(ESBMC) not found" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	printf '#include "%s"\n' "$(MESSAGE_READ_MAX_SOURCE)" > "$$work/proof.cpp"; \
	failed=0; \
	for entry in $(MESSAGE_READ_MAX_HARNESSES); do \
	    harness=$${entry%%:*}; rest=$${entry#*:}; \
	    unwind=$${rest%%:*}; extra=$$(echo "$${rest#*:}" | tr ',' ' '); \
	    output="$$work/$$harness.log"; \
	    start=$$(date +%s); status=0; \
	    timeout $(MESSAGE_READ_MAX_TIMEOUT) $(ESBMC) $(MESSAGE_READ_MAX_FLAGS) --unwind $$unwind $$extra \
	        --function $$harness "$$work/proof.cpp" > "$$output" 2>&1 || status=$$?; \
	    seconds=$$(( $$(date +%s) - start )); \
	    if [ $$status -eq 124 ]; then \
	        verdict="timed out after $(MESSAGE_READ_MAX_TIMEOUT) s"; \
	    elif grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    elif grep -A3 "^Violated property" "$$output" | grep -q "unwinding assertion"; then \
	        verdict="unwinding assertion failed"; \
	    elif grep -q "VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    else \
	        verdict="FAILED (exit $$status)"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then cat "$$output"; failed=1; fi; \
	    echo "$$harness $$verdict"; \
	    echo "  unwind $$unwind $$extra; $$(grep -E 'properties (failed|passed)|of [0-9]+ properties' "$$output" | head -1 | sed 's/^ *//'); $$seconds s"; \
	done; \
	exit $$failed
