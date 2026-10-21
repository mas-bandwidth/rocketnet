# CBMC proofs. `make -C proofs/cbmc` runs the proofs that finish:
# the arithmetic leaves below, then the bounded readers, the message reader,
# and the XDP free list. fragment-bounds stays a target but leaves `all`:
# proofs/cbmc/fragment_reassembly proves the real reassembly code on ESBMC
# and supersedes the copy that target checks.
#
# Every target prints the bound it proves beside the deployed constant the
# bound is measured against, so a bounded proof cannot read as the whole
# range.
#
# Arithmetic leaves:
#   replay-delta-1-3   proved: harness_replay_near in proof_replay_advance.c,
#                      unwind 65, 4096-bit (64-word) window, admitted deltas
#                      1, 2, and 3 (sequence > highest_opened and
#                      sequence - highest_opened < 4); one arbitrary word
#                      stands for every word. Deltas 4..4095 are not proved.
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
# `make -C proofs/cbmc slow-unproved` runs replay-near-4096 and ack-run-bound-8.
# Neither is proved, and neither is in the default target or CI:
#   replay-near-4096   not proved: harness_replay_near with REPLAY_DELTA_BOUND
#                      4096 and unwind 4096 (deltas 1..4095).
#   ack-run-bound-8    not proved: proof_ack_apply_run.c with ACK_RUN_BOUND 8
#                      and unwind 8 (runs of length 1..8 inside the 4096-record
#                      sent window); the full 4096 bound is also not proved.

CBMC ?= cbmc

CBMC_FLAGS = --bounds-check --pointer-check --signed-overflow-check \
             --unsigned-overflow-check --div-by-zero-check \
             --unwinding-assertions

SDK = ../../sdk

# The deployed constants each bound is printed beside, read from the sdk
# headers. A macro written as an arithmetic expression carries its deployed
# value in a trailing comment.
deployed_literal = $(shell sed -n 's/^#define $(1)[[:space:]]*\([0-9][0-9]*\).*/\1/p' $(SDK)/$(2) | head -1)
deployed_comment = $(shell sed -n 's/^#define $(1).*\/\/ \([0-9][0-9]*\).*/\1/p' $(SDK)/$(2) | head -1)

REPLAY_WINDOW_BITS := $(call deployed_literal,ROCKETNET_HISTORY_PACKETS,rocketnet_history.h)
MAX_PAYLOAD_BYTES := $(call deployed_literal,ROCKETNET_MAX_PACKET_PAYLOAD_BYTES,rocketnet.h)
MAX_FRAGMENT_BYTES := $(call deployed_comment,ROCKETNET_MAX_FRAGMENT_BYTES,rocketnet_fragment.h)
MAX_FRAGMENTS := $(call deployed_literal,ROCKETNET_MAX_FRAGMENTS,rocketnet_fragment.h)
MAX_FRAGMENTED_PAYLOAD_BYTES := $(shell expr $(MAX_FRAGMENTS) \* $(MAX_FRAGMENT_BYTES))
XDP_NUM_FRAMES := $(call deployed_literal,ROCKETNET_XDP_NUM_FRAMES,rocketnet_constants.h)
XDP_FRAME_SIZE := $(call deployed_literal,ROCKETNET_XDP_FRAME_SIZE,rocketnet_constants.h)

# Bounded readers, the message reader, and the XDP free
# list. Same flags as the arithmetic leaves. Each proof is a bounded
# instance, in the same sense as replay-delta-1-3:
#   read-write         proved: proof_read_write.c, unwind 9. Symbolic
#                      buffers of 8 bytes. Writers and readers round-trip;
#                      the bounded readers, including the variable-length
#                      byte reader, refuse a read past the end pointer and
#                      leave the cursor alone. rocketnet_copy_string with
#                      dest_size 0 writes nothing and returns 0; a positive
#                      dest_size up to 8 keeps the copy inside the buffer
#                      and terminates it. The backing object for a bounded
#                      read is 8 bytes of slack past the end pointer, so the
#                      guard's pointer add stays inside the object.
#   fragment-bounds    superseded: proof_fragment_bounds.c copies the
#                      reassembly function, and fragment_reassembly/ proves
#                      the real one on ESBMC. It is no longer in `all`.
#                      Its bounds: unwind 64, one connection and one slot.
#                      Fragment payloads are symbolic up to 8 bytes, or
#                      outside the legal size window (refused, no read).
#                      max_payload_bytes is symbolic from 1 to 16, below
#                      every deployed maximum. A reconstructed total above
#                      that maximum is refused with no eviction and no
#                      payload write. A final that arrives while the stride
#                      is 0 is staged, and the payload region is untouched.
#                      Counts above 8-byte fragments, payload maxima above
#                      16, and the uint32 id-rollover half of the stale
#                      check, are not proved.
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

.PHONY: all replay-delta-1-3 replay-far reconstruct \
        read-write fragment-bounds message-read xdp-free-list \
        slow-unproved replay-near-4096 ack-run-bound-8 clean

all: replay-delta-1-3 replay-far reconstruct \
     read-write message-read xdp-free-list

# delta = sequence - highest_opened is at least 1 and strictly less than 4,
# so the admitted deltas are 1, 2, and 3. Unwind 65 initializes all 64
# symbolic window words; the clear loop runs at most twice.
replay-delta-1-3: proof_replay_advance.c
	@echo "replay-delta-1-3 bound delta 1..3 deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS); deltas 4..$(shell expr $(REPLAY_WINDOW_BITS) - 1) are not proved by this target"
	$(CBMC) $(CBMC_FLAGS) --unwind 65 -I$(SDK) \
	    -DREPLAY_DELTA_BOUND=4 \
	    --function harness_replay_near proof_replay_advance.c

replay-far: proof_replay_advance.c
	@echo "replay-far bound delta >= $(REPLAY_WINDOW_BITS) deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS)"
	$(CBMC) $(CBMC_FLAGS) --unwind 65 -I$(SDK) \
	    --function harness_replay_far proof_replay_advance.c

reconstruct: proof_reconstruct_sequence.c
	@echo "reconstruct bound 32-bit and 24-bit wire half-ranges deployed ROCKETNET_REPLAY_WINDOW_BITS=$(REPLAY_WINDOW_BITS)"
	$(CBMC) $(CBMC_FLAGS) --unwind 1 proof_reconstruct_sequence.c

# Not proved. Not part of `all`.
slow-unproved: replay-near-4096 ack-run-bound-8

replay-near-4096: proof_replay_advance.c
	$(CBMC) $(CBMC_FLAGS) --unwind 4096 -I$(SDK) \
	    -DREPLAY_DELTA_BOUND=4096 \
	    --function harness_replay_near proof_replay_advance.c

ack-run-bound-8: proof_ack_apply_run.c
	$(CBMC) $(CBMC_FLAGS) --unwind 8 \
	    -DACK_RUN_BOUND=8 \
	    proof_ack_apply_run.c

read-write: proof_read_write.c
	@echo "read-write bound buffers 8 bytes deployed ROCKETNET_MAX_FRAGMENT_BYTES=$(MAX_FRAGMENT_BYTES)"
	$(CBMC) $(CBMC_FLAGS) --unwind 9 -I$(SDK) proof_read_write.c

# Superseded by fragment_reassembly/. Not part of `all`.
fragment-bounds: proof_fragment_bounds.c
	@echo "fragment-bounds bound payload max 16 bytes, fragment 8 bytes deployed ROCKETNET_MAX_PACKET_PAYLOAD_BYTES=$(MAX_PAYLOAD_BYTES) ROCKETNET_MAX_FRAGMENT_BYTES=$(MAX_FRAGMENT_BYTES) ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES=$(MAX_FRAGMENTED_PAYLOAD_BYTES); not proved above 16"
	$(CBMC) $(CBMC_FLAGS) --unwind 64 -I$(SDK) proof_fragment_bounds.c

message-read: proof_message_read.c
	@echo "message-read bound payload 24 bytes deployed ROCKETNET_MAX_PACKET_PAYLOAD_BYTES=$(MAX_PAYLOAD_BYTES)"
	$(CBMC) $(CBMC_FLAGS) --unwind 8 -DNDEBUG -I$(SDK) proof_message_read.c

xdp-free-list: proof_xdp_free_list.c
	@echo "xdp-free-list bound capacity 3 frames deployed ROCKETNET_XDP_NUM_FRAMES=$(XDP_NUM_FRAMES) frames of $(XDP_FRAME_SIZE) bytes"
	$(CBMC) $(CBMC_FLAGS) --unwind 5 -I$(SDK) proof_xdp_free_list.c

clean:
	rm -f *.goto *.output

# Nightly proofs. Each reader directory holds a proof.mk that appends its
# target to NIGHTLY, so a CBMC card adds a reader without editing a shared
# file. The `all` target above is the per-commit set and is unchanged; the
# nightly target runs the longer proofs that are not part of CI.

NIGHTLY :=

-include $(wildcard */proof.mk)

.PHONY: nightly

nightly: $(NIGHTLY)
