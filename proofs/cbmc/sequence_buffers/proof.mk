# Sequence-buffer proofs on the real SDK code (sdk/rocketnet_replay.cpp,
# sdk/rocketnet_ack.cpp, sdk/rocketnet_window_receive.cpp). Included by
# proofs/cbmc/Makefile through the nightly scaffold; never part of `all`.

NIGHTLY += proofs-sequence_buffers

ESBMC ?= $(shell command -v esbmc 2>/dev/null || echo $(HOME)/sdk/bin/esbmc)

SEQUENCE_BUFFERS_SOURCE = $(CURDIR)/sequence_buffers/proof_sequence_buffers.c
# The only memset these readers reach is the far replay advance's clear of
# the 512-byte window; ESBMC 8.5 has no memset-limit option, so the libc
# memset loop gets its own explicit unwind.
SEQUENCE_BUFFERS_FLAGS = --std c++17 --overflow-check --ub-shift-check \
                         -DROCKETNET_ESBMC -DNDEBUG -I$(SDK) -I$(CURDIR) \
                         --unwindsetname __memset_impl:0:513 --multi-property
SEQUENCE_BUFFERS_TIMEOUT = 1800
SEQUENCE_BUFFERS_MEMLIMIT = 12g

# harness:unwind:solver and extra flags (comma for space); every entry names
# its solver. Z3 is the default solver;
# Bitwuzla is named where it is used instead (results.md, Solver). Every harness in
# SEQUENCE_BUFFERS_HARNESSES proves its claim, so the nightly target is green
# when the sdk is. The two loops are proved by their step harnesses plus the
# induction argument in results.md.
SEQUENCE_BUFFERS_HARNESSES = \
    harness_replay_already_received:65:--z3 \
    harness_replay_advance_step:65:--z3 \
    harness_replay_advance_near_bounded:257:--bitwuzla \
    harness_replay_advance_far:65:--z3 \
    harness_ack_record_received:2:--bitwuzla \
    harness_ack_mark_keep_alive:2:--z3 \
    harness_window_receive_init:2:--z3 \
    harness_window_receive_accept_step:4:--bitwuzla \
    harness_window_receive_accept_bounded:65:--bitwuzla \
    harness_window_receive_release:2:--z3

# Owed: claims no solver finishes inside the time and memory limits
# (results.md). They run only from proofs-sequence_buffers-owed, never from
# the nightly, so an unproved bound does not turn the nightly red; the
# follow-up card rocketnet-cbmc-sequence-buffers-owed-bounds closes them.
SEQUENCE_BUFFERS_OWED = \
    harness_replay_advance:4097:--z3 \
    harness_ack_record_sent_classified:2:--bitwuzla \
    harness_ack_mark_keep_alive_pinned:2:--z3 \
    harness_window_receive_accept:1048577:--z3

# The recipe of both targets: each runs its own SEQUENCE_BUFFERS_LIST.
define sequence_buffers_recipe
	@echo "$@"; \
	command -v $(ESBMC) > /dev/null || [ -x "$(ESBMC)" ] || { echo "$@: $(ESBMC) not found" >&2; exit 1; }; \
	work=$$(mktemp -d); \
	trap 'rm -rf "$$work"' EXIT HUP INT TERM; \
	printf '#include "%s"\n' "$(SEQUENCE_BUFFERS_SOURCE)" > "$$work/proof.cpp"; \
	failed=0; \
	[ -n "$(strip $(SEQUENCE_BUFFERS_LIST))" ] || { echo "$@: no harnesses listed" >&2; exit 1; }; \
	for entry in $(SEQUENCE_BUFFERS_LIST); do \
	    harness=$${entry%%:*}; rest=$${entry#*:}; \
	    unwind=$${rest%%:*}; extra=$$(echo "$${rest#*:}" | tr ',' ' '); \
	    output="$$work/$$harness.log"; \
	    start=$$(date +%s); status=0; \
	    timeout $(SEQUENCE_BUFFERS_TIMEOUT) $(ESBMC) $(SEQUENCE_BUFFERS_FLAGS) \
	        --memlimit $(SEQUENCE_BUFFERS_MEMLIMIT) --unwind $$unwind $$extra \
	        --function $$harness "$$work/proof.cpp" > "$$output" 2>&1 || status=$$?; \
	    seconds=$$(( $$(date +%s) - start )); \
	    if [ $$status -eq 124 ]; then \
	        verdict="timed out after $(SEQUENCE_BUFFERS_TIMEOUT) s"; \
	    elif grep -Eq "^ *FAILED .*unwinding assertion" "$$output"; then \
	        verdict="unwinding assertion failed"; \
	    elif grep -q "^VERIFICATION FAILED" "$$output"; then \
	        verdict="VERIFICATION FAILED"; \
	    elif grep -q "Out of memory" "$$output"; then \
	        verdict="out of memory at $(SEQUENCE_BUFFERS_MEMLIMIT)"; \
	    elif [ $$status -eq 0 ] && grep -q "^VERIFICATION SUCCESSFUL" "$$output" && \
	         ! grep -Eq "^ *FAILED|not checked" "$$output"; then \
	        verdict="VERIFICATION SUCCESSFUL"; \
	    else \
	        verdict="FAILED (exit $$status)"; \
	    fi; \
	    if [ "$$verdict" != "VERIFICATION SUCCESSFUL" ]; then grep -v "^Unwinding loop" "$$output" | tail -n 40; failed=1; fi; \
	    echo "$$harness $$verdict"; \
	    echo "  unwind $$unwind $$extra; $$(grep -E 'of [0-9]+ properties' "$$output" | tail -1 | sed 's/^[* ]*//'); $$seconds s"; \
	done; \
	exit $$failed
endef

.PHONY: proofs-sequence_buffers proofs-sequence_buffers-owed

proofs-sequence_buffers: SEQUENCE_BUFFERS_LIST = $(SEQUENCE_BUFFERS_HARNESSES)
proofs-sequence_buffers:
	$(sequence_buffers_recipe)

proofs-sequence_buffers-owed: SEQUENCE_BUFFERS_LIST = $(SEQUENCE_BUFFERS_OWED)
proofs-sequence_buffers-owed:
	$(sequence_buffers_recipe)
