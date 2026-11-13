# Connect-token reader proofs on the real SDK. Nightly only. After the seven
# harnesses verify, two controls must fail: harness_is_zero_overread_control
# (the sodium_is_zero model refuses a one-byte overread of a stack block) and
# the pad-overread mutant (the opener on a copy of the SDK unit whose pad
# check reads 4 KiB too far), both on "is_zero reads its buffer".
NIGHTLY += proofs-connect_token

CONNECT_TOKEN_SOURCE = connect_token/proof_connect_token.cpp
CONNECT_TOKEN_FLAGS = --std c++17 --z3 --overflow-check -DROCKETNET_ESBMC -DNDEBUG -I$(SDK) -I$(SDK)/../sodium
# the is_zero control runs with ESBMC's own bounds and pointer checks off, so
# the model's buffer assertion is the one property that can fail
CONNECT_TOKEN_CONTROL_FLAGS = --no-bounds-check --no-pointer-check
# the opener's pad check made to read 4 KiB past the 384-byte stack block:
# one sed over a copy of sdk/rocketnet_connect_token.cpp that the opener
# harness must fail on the sodium_is_zero model (B025-F2's plant)
CONNECT_TOKEN_MUTANT_pad-overread = s/( ROCKETNET_PRIVATE_BLOCK_BYTES - pad_offset ) ) == 0/( ROCKETNET_PRIVATE_BLOCK_BYTES - pad_offset + 4096 ) ) == 0/
CONNECT_TOKEN_TIMEOUT = 1800s
CONNECT_TOKEN_AS_LIMIT = 8589934592

.PHONY: proofs-connect_token

proofs-connect_token:
	@set -e; \
	for harness in \
	    harness_read_connect_token \
	    harness_read_signed_connect_data \
	    harness_read_connect_token_private \
	    harness_verify_signed_connect_data \
	    harness_open_connect_token_private \
	    harness_private_consistent \
	    harness_connect_token_identifier; do \
	    case "$$harness" in \
	      harness_read_connect_token) unwind=577 ;; \
	      harness_read_signed_connect_data) unwind=433 ;; \
	      harness_read_connect_token_private) unwind=257 ;; \
	      harness_open_connect_token_private) unwind=385 ;; \
	      harness_connect_token_identifier) unwind=17 ;; \
	      *) unwind=1 ;; \
	    esac; \
	    if [ -n "$$CONNECT_TOKEN_LOG_DIR" ]; then \
	      mkdir -p "$$CONNECT_TOKEN_LOG_DIR"; output="$$CONNECT_TOKEN_LOG_DIR/$$harness.log"; \
	    else \
	      output=$$(mktemp); \
	    fi; \
	    status=0; \
	    $(CURDIR)/run_with_timeout.sh $(CONNECT_TOKEN_TIMEOUT) prlimit --as=$(CONNECT_TOKEN_AS_LIMIT) -- \
	      $(ESBMC) $(CONNECT_TOKEN_FLAGS) --unwind $$unwind --function $$harness \
	      $(CONNECT_TOKEN_SOURCE) > "$$output" 2>&1 || status=$$?; \
	    if [ $$status -eq 0 ] && grep -q "VERIFICATION SUCCESSFUL" "$$output"; then \
	      echo "$$harness VERIFICATION SUCCESSFUL (unwind $$unwind)"; \
	    else \
	      if grep -q "VERIFICATION FAILED" "$$output"; then verdict="VERIFICATION FAILED"; \
	      elif grep -q "unwinding assertion" "$$output"; then verdict="unwinding assertion failed"; \
	      elif [ $$status -eq 124 ]; then verdict="timed out"; \
	      else verdict="unproved (exit $$status)"; fi; \
	      echo "$$harness $$verdict (unwind $$unwind)"; \
	      cat "$$output"; \
	      exit 1; \
	    fi; \
	    if [ -z "$$CONNECT_TOKEN_LOG_DIR" ]; then rm -f "$$output"; fi; \
	done; \
	harness=harness_is_zero_overread_control; \
	output=$$(mktemp); status=0; \
	$(CURDIR)/run_with_timeout.sh $(CONNECT_TOKEN_TIMEOUT) prlimit --as=$(CONNECT_TOKEN_AS_LIMIT) -- \
	  $(ESBMC) $(CONNECT_TOKEN_FLAGS) $(CONNECT_TOKEN_CONTROL_FLAGS) --unwind 42 --function $$harness \
	  $(CONNECT_TOKEN_SOURCE) > "$$output" 2>&1 || status=$$?; \
	if [ $$status -eq 1 ] && grep -q '^VERIFICATION FAILED' "$$output" && \
	   grep -A4 '^Violated property:' "$$output" | grep -q 'is_zero reads its buffer'; then \
	  echo "$$harness EXPECTED FAILURE (the is_zero model refuses a one-byte overread)"; \
	  rm -f "$$output"; \
	else \
	  echo "$$harness HOLD: the planted overread did not fail on the is_zero model (exit $$status)"; \
	  cat "$$output"; rm -f "$$output"; exit 1; \
	fi; \
	mutant=$$(mktemp -d); \
	sed '$(CONNECT_TOKEN_MUTANT_pad-overread)' $(SDK)/rocketnet_connect_token.cpp > "$$mutant/rocketnet_connect_token.cpp"; \
	if cmp -s $(SDK)/rocketnet_connect_token.cpp "$$mutant/rocketnet_connect_token.cpp"; then \
	  echo "connect_token mutant pad-overread HOLD: the mutant no longer applies to sdk/rocketnet_connect_token.cpp"; \
	  rm -rf "$$mutant"; exit 1; \
	fi; \
	output="$$mutant/output.log"; status=0; \
	$(CURDIR)/run_with_timeout.sh $(CONNECT_TOKEN_TIMEOUT) prlimit --as=$(CONNECT_TOKEN_AS_LIMIT) -- \
	  $(ESBMC) -I"$$mutant" $(CONNECT_TOKEN_FLAGS) --unwind 385 --function harness_open_connect_token_private \
	  $(CONNECT_TOKEN_SOURCE) > "$$output" 2>&1 || status=$$?; \
	if [ $$status -eq 1 ] && grep -q '^VERIFICATION FAILED' "$$output" && \
	   grep -A4 '^Violated property:' "$$output" | grep -q 'is_zero reads its buffer'; then \
	  echo "connect_token mutant pad-overread EXPECTED FAILURE (a 4 KiB pad overread is refused)"; \
	  rm -rf "$$mutant"; \
	else \
	  echo "connect_token mutant pad-overread HOLD: the overread verified (exit $$status)"; \
	  cat "$$output"; rm -rf "$$mutant"; exit 1; \
	fi
