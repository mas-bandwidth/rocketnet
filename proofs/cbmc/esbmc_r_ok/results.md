# ESBMC 8.5 `__CPROVER_r_ok` / `__CPROVER_w_ok` on Symbolic-Size Allocations (`esbmc_shim.h`)

## Root Cause

All RocketNet ESBMC harnesses that include SDK `.cpp` translation units (`message_read_max`, `client_packets`, `server_packets`) run ESBMC 8.5.0 in C++17 mode (`--std c++17` via a `.cpp` wrapper).

In ESBMC 8.5.0 (`64-bit x86_64 linux`, Z3 v4.13.3):

1. **Built-in `__ESBMC_r_ok(void *, unsigned long)` on symbolic-size `malloc(n)` in C++17 mode**:
   When `uint8_t *buf = (uint8_t *)malloc(n)` is executed for symbolic `n` in `1..64`, ESBMC creates a dynamic array symbol `dynamic_1_array` whose type expression holds the C++ local symbol `n`. In C++17 mode (`c:file@F@main#@n`), `symex_malloc` does not SSA-rename the size symbol inside `dynamic_1_array`'s array type, leaving it at SSA generation `#0` (`n?1!0&0#0`, uninitialized) instead of the active SSA generation `#1` (`n?1!0&0#1`). Lowering `__ESBMC_r_ok(buf, n)` therefore emits the verification condition:
   ```
   (unsigned long int)n?1!0&0#1 <= (unsigned long int)n?1!0&0#0
   ```
   which Z3 falsifies by choosing `n#0 = 0, n#1 = 64`, failing both `__ESBMC_r_ok(buf, n)` and `__ESBMC_r_ok(buf + k, n - k)` on valid in-bounds pointers.
2. **ESBMC libc `__CPROVER_r_ok` (`/esbmc-vfs/libc/library/builtin_libs.c`) and `__ESBMC_get_object_size(p)`**:
   ESBMC's C library `__CPROVER_r_ok` calls `__ESBMC_get_object_size(p)`, which extracts the size from `dynamic_1_array`'s type expression and hits the identical unrenamed SSA `#0` symbol in C++17 mode (`n?1!0&0#1 <= n?1!0&0#0`), or lowers to `sizeof(void *)` (`8`) when the pointer type is not a resolvable array.
3. **Fix in `proofs/cbmc/esbmc_shim.h` (`rocketnet_esbmc_r_ok`)**:
   Inside ESBMC's `/esbmc-vfs/libc/library/stdlib.c`, `malloc(size_t size)` explicitly writes the SSA-renamed allocation size into the runtime intrinsic table `__ESBMC_alloc_size[__ESBMC_POINTER_OBJECT(res)] = size;` and marks `__ESBMC_is_dynamic[obj] = 1` and `__ESBMC_alloc[obj] = 1`.
   `rocketnet_esbmc_r_ok(const void *p, size_t n)` in `proofs/cbmc/esbmc_shim.h`:
   - Returns `true` when `n == 0`, and `false` when `p == NULL` or `obj == 0`.
   - Rejects unallocated/freed dynamic objects (`__ESBMC_is_dynamic[obj] && !__ESBMC_alloc[obj]`).
   - Rejects negative pointer offsets (`off = __ESBMC_POINTER_OFFSET(p) < 0`).
   - Reads `sz = __ESBMC_is_dynamic[obj] ? __ESBMC_alloc_size[obj] : __ESBMC_get_object_size(p)`, resolving dynamic heap allocations via `__ESBMC_alloc_size[obj]` and stack/static buffers via `__ESBMC_get_object_size(p)`.
   - Checks `uoff <= sz && n <= sz - uoff` so neither `sz - uoff` nor `uoff + n` can wrap or overflow `size_t`.

---

## 1. Standalone Reproducer (`proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c`)

### Before (`__ESBMC_r_ok` in C++17 mode, as used by SDK-including ESBMC harnesses)

```
$ printf '#include "%s/proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c"\n' "$PWD" > /tmp/repro_before.cpp
$ ~/sdk/bin/esbmc --std c++17 --z3 -I proofs/cbmc --force-malloc-success -DTEST_PRE_FIX_ESBMC_R_OK /tmp/repro_before.cpp
Violated property:
  file proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c line 59 column 5 function main
  full symbolic malloc extent is readable
  return_value$___ESBMC_r_ok$2

** Results:
proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c, function main
  NOT CHECKED  [main.invalidated-dynamic-object.1]        line 59  dereference failure: invalidated dynamic object
  FAILED       [main.assertion.1]                         line 59  full symbolic malloc extent is readable
  NOT CHECKED  [main.invalidated-dynamic-object.2]        line 61  dereference failure: invalidated dynamic object
  FAILED       [main.assertion.2]                         line 61  full symbolic malloc extent is writable
  NOT CHECKED  [main.invalidated-dynamic-object.3]        line 67  dereference failure: invalidated dynamic object
  FAILED       [main.assertion.3]                         line 67  symbolic suffix slice [k, n) of malloc(n) is readable
  NOT CHECKED  [main.invalidated-dynamic-object.4]        line 69  dereference failure: invalidated dynamic object
  FAILED       [main.assertion.4]                         line 69  symbolic suffix slice [k, n) of malloc(n) is writable
  PASSED       [main.assertion.5]                         line 76  symbolic tail slice of stack buffer is readable
  PASSED       [main.free-non-zero-offset.1]              line 79  Operand of free must have zero pointer offset

** 4 of 12 properties failed, 2 passed, 6 not checked
VERIFICATION FAILED
```

### After (`proofs/cbmc/esbmc_shim.h`, C mode and C++17 mode)

```
$ ~/sdk/bin/esbmc --z3 -I proofs/cbmc --force-malloc-success proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c
** Results:
proofs/cbmc/esbmc_r_ok/repro_r_ok_symbolic.c, function main
  PASSED       [main.assertion.1]                                   line 59  full symbolic malloc extent is readable
  PASSED       [main.assertion.2]                                   line 61  full symbolic malloc extent is writable
  PASSED       [main.assertion.3]                                   line 67  symbolic suffix slice [k, n) of malloc(n) is readable
  PASSED       [main.assertion.4]                                   line 69  symbolic suffix slice [k, n) of malloc(n) is writable
  PASSED       [main.assertion.5]                                   line 76  symbolic tail slice of stack buffer is readable
  PASSED       [main.free-non-zero-offset.1]                        line 79  Operand of free must have zero pointer offset
  PASSED       [main.invalidated-dynamic-object.1]                  line 79  dereference failure: invalidated dynamic object
  PASSED       [main.invalidated-dynamic-object-freed.1]            line 79  dereference failure: invalidated dynamic object freed
proofs/cbmc/esbmc_shim.h, function rocketnet_esbmc_r_ok
  PASSED       [rocketnet_esbmc_r_ok.invalidated-dynamic-object.1]  line 77  dereference failure: invalidated dynamic object

** 0 of 9 properties failed, 9 passed
VERIFICATION SUCCESSFUL
```

---

## 2. Planted 1-Byte OOB (`proofs/cbmc/esbmc_r_ok/planted_oob.c`)

```
$ ~/sdk/bin/esbmc --z3 -I proofs/cbmc --force-malloc-success proofs/cbmc/esbmc_r_ok/planted_oob.c
Violated property:
  file proofs/cbmc/esbmc_r_ok/planted_oob.c line 29 column 5 function main
  planted 1-byte OOB past symbolic malloc(n) must fail
  return_value$_rocketnet_esbmc_r_ok$4

** Results:
proofs/cbmc/esbmc_r_ok/planted_oob.c, function main
  FAILED       [main.assertion.1]                                   line 29  planted 1-byte OOB past symbolic malloc(n) must fail
  PASSED       [main.free-non-zero-offset.1]                        line 32  Operand of free must have zero pointer offset
  NOT CHECKED  [main.invalidated-dynamic-object.1]                  line 32  dereference failure: invalidated dynamic object
  NOT CHECKED  [main.invalidated-dynamic-object-freed.1]            line 32  dereference failure: invalidated dynamic object freed
proofs/cbmc/esbmc_shim.h, function rocketnet_esbmc_r_ok
  NOT CHECKED  [rocketnet_esbmc_r_ok.invalidated-dynamic-object.1]  line 77  dereference failure: invalidated dynamic object

** 1 of 5 properties failed, 1 passed, 3 not checked
VERIFICATION FAILED
```

---

## 3. Existing Harnesses Using `esbmc_shim.h` (Unplanted vs Planted 1-Byte OOB)

| Target / Harness | Unplanted Verdict | Properties | Time | Planted 1-Byte OOB | Planted Verdict |
| --- | --- | ---: | ---: | --- | --- |
| `message_read_max` / `harness_message_reader` | `VERIFICATION SUCCESSFUL` | 211 / 211 | 33 s | `__CPROVER_r_ok(record.data, (size_t)record.bytes + 2)` | `VERIFICATION FAILED` |
| `message_read_max` / `harness_message_reader_single_max` | `VERIFICATION SUCCESSFUL` | 199 / 199 | 9 s | `__CPROVER_r_ok(record.data, (size_t)record.bytes + 2)` | `VERIFICATION FAILED` (`1 of 199 failed`) |
| `message_read_max` / `harness_message_read_config_body` | `VERIFICATION SUCCESSFUL` | 70 / 70 | 1 s | `buf[CONFIG_BOUND - 1]` | `VERIFICATION FAILED` |
| `packet_open` (all 6 harnesses, CBMC 6.6.0) | `VERIFICATION SUCCESSFUL` | 8553 / 8553 | 3 s | `malloc((size_t)(packet_bytes - 1))` | `VERIFICATION FAILED` |
| `connect_token` / `harness_verify_signed_connect_data` | `VERIFICATION SUCCESSFUL` | 12 / 12 | 2 s | `PROOF_R_OK(m, mlen + 1)` | `VERIFICATION FAILED` (`1 of 12 failed`) |
| `connect_token` / `harness_private_consistent` | `VERIFICATION SUCCESSFUL` | 36 / 36 | 2 s | `private_data[ROCKETNET_PRIVATE_BLOCK_BYTES - 1]` | `VERIFICATION FAILED` |
| `connect_token` / `harness_connect_token_identifier` | `VERIFICATION SUCCESSFUL` | 10 / 10 | 2 s | `PROOF_W_OK(out, outlen + 1)` | `VERIFICATION FAILED` |
