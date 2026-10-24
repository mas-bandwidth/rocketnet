RESULT: rn-linux-soak-30min-per-transport-a sha=d1df52eab254b4d814de8f6a42a0b6d80f5cc847 tier: heavy

Head: d1df52eab254b4d814de8f6a42a0b6d80f5cc847
Branch: sprint/rn-linux-soak-30min-per-transport-a.w3.g1.e1
Verdict: not-done

The commit on this head returns free glibc arenas with malloc_trim(0) before the VmRSS read in tests/process_resources.h. That is the only change since a0aa005d.

120 s cells on hulk, cores 0,16-31, head d1df52ea, impaired, 2026-10-24T06:40:27Z to 06:48:27Z:

- portable FAIL. fds 7/7/9, threads 3/3/4, resident_bytes 18890752/18898944/19443712. --assert-flat FAILED: allowed growth 4096 bytes over 90 s. The end is 8192 bytes over the start.
- gso PASS. verified=215385, fds 9/9/11, threads 6/6/7, resident_bytes 198131712/198131712/198664192.
- gso-plain PASS. verified=215881, fds 9/9/11, threads 6/6/7, resident_bytes 198090752/198090752/198623232.
- io_uring PASS. verified=214597, fds 9/9/11, threads 1/1/2, resident_bytes 100515840/100515840/101052416.

Portable only, same head, no extra environment:

- 45 s PASS. resident_bytes 19001344/19001344/19542016. 2 connects.
- 240 s FAIL. resident_bytes 18980864/18993152/19546112. The end is 12288 bytes over the start, allowed 4096 over 210 s. fds 7/7/9, threads 3/3/4. 8 connects, 7 reconnects.

A later 120 s portable run with a tracer (not the record; the tracer adds its own mappings) split RSS at the idle samples. The brk heap returns to 60 KB, stacks stay 60 KB, file mappings stay 4876 KB, and anonymous RSS moves from 13468 KB to 13480 KB and then holds. The pages over the allowance are anonymous, not the heap malloc_trim gives back. An earlier mallinfo trace showed live allocated bytes at those idle samples moving by about 1.5 KB, not by the resident delta.

Not done:

- portable 120 s does not pass --assert-flat. The allowance was not loosened.
- the xdp cell inside virtme-ng was not run.
- soak.exe --assert-flat --seconds 60 on captain was not run.
- make ROCKETNET_DEV=OFF was not run on this commit.
- ./bin/test and ./bin/functional were green at a0aa005d and were not re-run after d1df52ea. Only soak.c includes the header.
- the 1800 s nightly step is in .github/workflows/nightly-soak.yml and was not run. The 30-minute record is the later card.
- docs/roadmap.sexp was not edited.
