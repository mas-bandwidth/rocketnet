# Server packet reader ESBMC results

Verdict: incomplete. No full server-reader proof is claimed. The harness includes the real server protocol, protocol, and connect-token C++ implementations. Libsodium and external SDK helpers are modeled. The one-slot server is assembled in the harness instead of through the transport create function. The static sealed-reader harness restricts type bytes to the six values dispatched to it. Solver success therefore has only the stated modeled scope.

ESBMC 8.5.0 uses C++17, Z3, --overflow-check, default pointer/bounds/division checks, -DROCKETNET_ESBMC -DNDEBUG, and the canonical shared shim. The .c harness is copied to a temporary .cpp file because ESBMC otherwise selects its C frontend. Live attempts use one core at low priority, an 8 GiB address-space limit, and a 1800-second timeout. The nightly target uses per-harness unwind 64/32/256/256 and named loop bounds for libc memcmp 65, memcpy 577, memset 1905, and shim byte havoc 385.

| Harness | Bound and result on this attempt | Property status |
| --- | --- | --- |
| harness_connection_request | Symbolic packet length 0..1472; prior source revision completed in 651.933 s using numeric loop bounds. Mapping zero-initialization changed afterward. | Prior revision: 616 passed, 0 failed. Final revision: unrun. |
| harness_connection_response | One live mapping at index 0, with fresh zeroed replay state; exact current source completed. | 355 passed, 0 failed; VERIFICATION SUCCESSFUL. |
| harness_sealed_packet | Symbolic packet length 0..1472 and six dispatcher-admitted sealed types; exact current source exited 2 after about 95 s with ERROR: Out of memory under the 8 GiB cap, before a property verdict. | 0 proved on this attempt; no SDK defect established. |
| harness_process_packet | Symbolic packet length 0..1472 and arbitrary type byte. Attempt stopped at startup because host exceeded solver-load threshold. | Unrun; no property verdict. |

The first request attempt on an older source reported one libc memset unwinding failure, nine passed properties, and four unchecked; it was a harness-bound failure. A later 409/409 request result was invalidated because a missing used-token-history helper body cut off the accepted path. That helper now has a model. The first response run found four failures because the model allowed a fresh mapping to emit an arbitrary multi-range ACK block. The real map-add code zeroes that mapping; mirroring its fresh replay state made the exact-source response run pass. No SDK defect is established from those model traces.

The request needs an exact-source rerun. The sealed reader needs a tractable full-bound model or a clearly labeled smaller proved bound. The dispatcher needs a capacity-compliant run. The static sealed type domain and manually assembled server also prevent the literal card claim. The per-commit all target remains unchanged and is not exercised by this attempt. The nightly target is wired through this reader's proof.mk; its four-verdict run has not completed.
