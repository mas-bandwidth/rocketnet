# Connect-token reader proofs. Included by proofs/cbmc/Makefile through the
# nightly scaffold; this file alone puts the readers on the nightly list.

NIGHTLY += proofs-connect_token

CONNECT_TOKEN_SOURCE = connect_token/proof_connect_token.c
CONNECT_TOKEN_FLAGS = --object-bits 12 -DNDEBUG -I$(SDK) -I$(SDK)/../sodium

.PHONY: proofs-connect_token

proofs-connect_token:
	@echo "proofs-connect_token"; \
	set -e; \
	for harness in \
	    harness_read_connect_token \
	    harness_read_signed_connect_data \
	    harness_read_connect_token_private \
	    harness_verify_signed_connect_data \
	    harness_open_connect_token_private \
	    harness_private_consistent \
	    harness_connect_token_identifier; do \
	    output=$$(mktemp); \
	    status=0; \
	    $(CBMC) $(CBMC_FLAGS) --unwind 1 $(CONNECT_TOKEN_FLAGS) --function $$harness \
	        $(CONNECT_TOKEN_SOURCE) > "$$output" 2>&1 || status=$$?; \
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
