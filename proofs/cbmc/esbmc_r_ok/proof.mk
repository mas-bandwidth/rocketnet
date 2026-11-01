# The ESBMC r_ok shim's controls. Every __CPROVER_r_ok / __CPROVER_w_ok in
# the ESBMC readers resolves to rocketnet_esbmc_r_ok in esbmc_shim.h, so the
# shim is checked on its own every night: repro_r_ok_symbolic.c must verify
# (in-bounds slices of a symbolic malloc and a stack tail are readable), and
# planted_oob.c must fail exactly one property, the planted one-byte overrun.
# Both run in C and in the C++17 mode the SDK readers use.

NIGHTLY += proofs-esbmc_r_ok

ESBMC_R_OK_FLAGS = --z3 -I$(CURDIR) --force-malloc-success

.PHONY: proofs-esbmc_r_ok

proofs-esbmc_r_ok:
	@echo "proofs-esbmc_r_ok"; \
	command -v $(ESBMC) > /dev/null || [ -x "$(ESBMC)" ] || { echo "proofs-esbmc_r_ok: $(ESBMC) not found" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	failed=0; \
	for mode in c cpp; do \
	    for control in repro_r_ok_symbolic planted_oob; do \
	        source=$(CURDIR)/esbmc_r_ok/$$control.c; flags=""; \
	        if [ $$mode = cpp ]; then \
	            printf '#include "%s"\n' "$$source" > "$$work/$$control.cpp"; \
	            source="$$work/$$control.cpp"; flags="--std c++17"; \
	        fi; \
	        output="$$work/$$control-$$mode.log"; status=0; \
	        $(ESBMC) $(ESBMC_R_OK_FLAGS) $$flags "$$source" > "$$output" 2>&1 || status=$$?; \
	        summary=$$(grep -E '\*\* [0-9]+ of [0-9]+ properties failed' "$$output" | head -1); \
	        if [ $$control = repro_r_ok_symbolic ]; then \
	            if [ $$status -eq 0 ] && grep -q '^VERIFICATION SUCCESSFUL' "$$output"; then \
	                verdict="VERIFICATION SUCCESSFUL"; \
	            else \
	                verdict="HOLD exit=$$status"; cat "$$output"; failed=1; \
	            fi; \
	        else \
	            if [ $$status -eq 1 ] && grep -q '^VERIFICATION FAILED' "$$output" && \
	               grep -Eq '\*\* 1 of [0-9]+ properties failed' "$$output" && \
	               grep -E '^ *FAILED ' "$$output" | grep -q 'planted 1-byte OOB past symbolic malloc'; then \
	                verdict="EXPECTED FAILURE"; \
	            else \
	                verdict="HOLD: the planted overrun did not fail alone (exit=$$status)"; cat "$$output"; failed=1; \
	            fi; \
	        fi; \
	        echo "$$control ($$mode) $$verdict; $$summary"; \
	    done; \
	done; \
	exit $$failed
