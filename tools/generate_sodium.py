#!/usr/bin/env python3
"""Extend a checksum-verified netcode baseline with pinned upstream sources.

This deliberately does not claim to reconstruct netcode's historical local
patches from upstream. Ordinary builds consume the committed pair offline.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import posixpath
import re
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"[^\n]*$', re.M)
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)', re.M)


# Warning adaptations (sodium/README.md, "Warning adaptations"). The SDK
# builds this source with warnings as errors at -Wall -Wextra -Wpedantic and
# /W4, as every target in the tree is built, so each warning the pinned
# inputs raise is fixed here, at the input, by one exact replacement that must
# match once. None touches cryptographic arithmetic: each removes code the
# subset never calls, spells an existing value explicitly, renames a local
# that shadows a file-scope name, or marks a compiler extension as one.
BASELINE_ADAPTATIONS = {
    'sodium.h': [
        # The baseline turned four MSVC warnings off for every file that
        # includes sodium.h, the SDK's own included; the warnings are fixed
        # at their sites instead.
        ('#if defined( _MSC_VER )\n#pragma warning(disable:4127)\n#pragma warning(disable:4244)\n'
         '#pragma warning(disable:4668)\n#pragma warning(disable:4456)\n#endif\n\n', ''),
        # -Wpedantic: __int128 is a GCC/Clang extension; __extension__ is the
        # compilers' own marker for a deliberate one.
        ('typedef unsigned __int128 uint128_t;',
         '__extension__ typedef unsigned __int128 uint128_t;'),
        # -Wtsan (GCC): ThreadSanitizer does not model a standalone fence and
        # GCC says so for every use. The acquire fence is a speculation
        # barrier, not synchronisation, so a ThreadSanitizer build (a test
        # build only) takes upstream's no-fence fallback.
        ('#if !defined(HAVE_GCC_MEMORY_FENCES) && (defined(__GNUC__) || defined(__clang__))\n',
         '#if !defined(HAVE_GCC_MEMORY_FENCES) && (defined(__GNUC__) || defined(__clang__)) && \\\n    !defined(__SANITIZE_THREAD__)\n'),
    ],
    'sodium.c': [
        # -Wunknown-pragmas (clang): clang defines __GNUC__ but has no GCC
        # target pragma. The block is compiled only when __SSE2__ is already
        # defined, so the pragma is GCC's alone.
        ('# ifdef __GNUC__\n#  pragma GCC target("sse2")\n# endif\n# include <emmintrin.h>\n\nstatic inline int\ncrypto_verify_n(',
         '# if defined(__GNUC__) && !defined(__clang__)\n#  pragma GCC target("sse2")\n# endif\n# include <emmintrin.h>\n\nstatic inline int\ncrypto_verify_n('),
    ],
}
# MemorySanitizer adaptations (sodium/README.md, "MemorySanitizer"). Two
# baseline writes reach memory through calls MSan cannot see: the raw
# syscall(SYS_getrandom) fill and libc's explicit_bzero wipe (the build's
# HAVE_EXPLICIT_BZERO on Linux). Both leave the written bytes poisoned, so
# every value derived from a random fill or read after a wipe is reported.
# Under MSan only, each write is followed by __msan_unpoison of exactly the
# bytes it wrote; every other build preprocesses the macro to nothing.
SANITIZER_ADAPTATIONS = {
    'sodium.c': [
        ('void\nsodium_memzero(void *const pnt, const size_t len)\n{\n',
         '#if defined(__has_feature)\n# if __has_feature(memory_sanitizer)\n#  include <sanitizer/msan_interface.h>\n'
         '#  define SODIUM_MSAN_UNPOISON(p, n) __msan_unpoison((p), (n))\n# endif\n#endif\n'
         '#ifndef SODIUM_MSAN_UNPOISON\n# define SODIUM_MSAN_UNPOISON(p, n) ((void) 0)\n#endif\n\n'
         'void\nsodium_memzero(void *const pnt, const size_t len)\n{\n'),
        ('    explicit_bzero(pnt, len);\n',
         '    explicit_bzero(pnt, len);\n    SODIUM_MSAN_UNPOISON(pnt, len);\n'),
        ('    } while (readnb < 0 && (errno == EINTR || errno == EAGAIN));\n\n    return (readnb == (int) size) - 1;\n',
         '    } while (readnb < 0 && (errno == EINTR || errno == EAGAIN));\n    if (readnb > 0) {\n'
         '        SODIUM_MSAN_UNPOISON(buf, (size_t) readnb);\n    }\n\n    return (readnb == (int) size) - 1;\n'),
    ],
}
UPSTREAM_ADAPTATIONS = {
    'crypto_core/ed25519/ref10/ed25519_ref10.c': [
        # -Wunused-function: the subset never converts a point to precomp.
        ('static void\nge25519_p3_to_precomp(ge25519_precomp *pi, const ge25519_p3 *p)\n{\n'
         '    fe25519 recip;\n    fe25519 x;\n    fe25519 y;\n    fe25519 xy;\n\n'
         '    fe25519_invert(recip, p->Z);\n    fe25519_mul(x, p->X, recip);\n    fe25519_mul(y, p->Y, recip);\n'
         '    fe25519_add(pi->yplusx, y, x);\n    fe25519_sub(pi->yminusx, y, x);\n    fe25519_mul(xy, x, y);\n'
         '    fe25519_mul(pi->xy2d, xy, d2);\n}\n\n', ''),
        # C4459: the local d of the two canonical checks hides the file-scope
        # curve constant d (fe_*/constants.h); the local is renamed d_.
        ('    unsigned char c;\n    unsigned char d;\n    unsigned int  i;\n\n'
         '    c = (s[31] & 0x7f) ^ 0x7f;\n    for (i = 30; i > 0; i--) {\n        c |= s[i] ^ 0xff;\n    }\n'
         '    c = (((unsigned int) c) - 1U) >> 8;\n    d = (0xed - 1U - (unsigned int) s[0]) >> 8;\n\n'
         '    return 1 - (c & d & 1);\n',
         '    unsigned char c;\n    unsigned char d_;\n    unsigned int  i;\n\n'
         '    c = (s[31] & 0x7f) ^ 0x7f;\n    for (i = 30; i > 0; i--) {\n        c |= s[i] ^ 0xff;\n    }\n'
         '    c = (((unsigned int) c) - 1U) >> 8;\n    d_ = (0xed - 1U - (unsigned int) s[0]) >> 8;\n\n'
         '    return 1 - (c & d_ & 1);\n'),
        ('    unsigned char c;\n    unsigned char d;\n    unsigned char e;\n    unsigned int  i;\n\n'
         '    c = (s[31] & 0x7f) ^ 0x7f;\n    for (i = 30; i > 0; i--) {\n        c |= s[i] ^ 0xff;\n    }\n'
         '    c = (((unsigned int) c) - 1U) >> 8;\n    d = (0xed - 1U - (unsigned int) s[0]) >> 8;\n'
         '    e = ((s[31] >> 5) ^ optblocker_u8) >> 2;\n\n'
         '    return 1 - (((c & d) | e | s[0]) & 1);\n',
         '    unsigned char c;\n    unsigned char d_;\n    unsigned char e;\n    unsigned int  i;\n\n'
         '    c = (s[31] & 0x7f) ^ 0x7f;\n    for (i = 30; i > 0; i--) {\n        c |= s[i] ^ 0xff;\n    }\n'
         '    c = (((unsigned int) c) - 1U) >> 8;\n    d_ = (0xed - 1U - (unsigned int) s[0]) >> 8;\n'
         '    e = ((s[31] >> 5) ^ optblocker_u8) >> 2;\n\n'
         '    return 1 - (((c & d_) | e | s[0]) & 1);\n'),
    ],
    'crypto_generichash/blake2b/ref/blake2b-compress-avx2.h': [
        # -Wunused-function: the AVX2 compressor loads through LOADU, never LOADU64.
        ('static inline uint64_t\nLOADU64(const void *p)\n{\n    uint64_t v;\n    memcpy(&v, p, sizeof v);\n    return v;\n}\n\n', ''),
    ],
    'crypto_generichash/blake2b/ref/blake2b-ref.c': [
        # C4245: f[] is uint64_t; the all-ones flag is spelled as one.
        ('    S->f[1] = -1;\n', '    S->f[1] = (uint64_t) -1;\n'),
        ('    S->f[0] = -1;\n', '    S->f[0] = (uint64_t) -1;\n'),
    ],
}


# C4244 (MSVC /W4): the ref10 byte encoders store 64- and 32-bit limbs into
# unsigned char outputs through the implicit narrowing conversion. Inside each
# named function body, every `out[k] = expr;` store is written with the cast
# that conversion already performs, `(type) (expr)`: the same value, by
# definition of the conversion. (function, output array, its element type)
NARROWING_STORES = {
    'crypto_core/ed25519/ref10/fe_25_5/fe.h': [('fe25519_tobytes(', 's', 'unsigned char', r'.*>>.*')],
    'crypto_core/ed25519/ref10/ed25519_ref10.c': [
        ('sc25519_mul(', 's', 'unsigned char', r'.*>>.*'), ('sc25519_muladd(', 's', 'unsigned char', r'.*>>.*'),
        ('sc25519_reduce(', 's', 'unsigned char', r'.*>>.*'), ('slide_vartime(', 'r', 'signed char', r'cmp')],
    'crypto_stream/salsa20/ref/salsa20_ref.c': [
        ('stream_ref(', 'in', 'unsigned char', r'u'), ('stream_ref_xor_ic(', 'in', 'unsigned char', r'u')],
}


def cast_narrowing_stores(text, stores, where):
    """Casts each `array[k] = value;` store whose value matches the pattern, in the function's body."""
    for function, array, element, value in stores:
        if text.count('\n' + function) != 1:
            raise SystemExit(f"narrowing-store function does not match once in {where}: {function}")
        start = text.index('\n' + function)
        end = text.index('\n}\n', start)
        body = text[start:end]
        store = re.compile(r'^(\s+' + re.escape(array) + r'\[[^\]]+\]\s*= )(' + value + r');$', re.M)
        cast, count = store.subn(lambda m: f'{m[1]}({element}) ({m[2]});', body)
        if count == 0:
            raise SystemExit(f"no narrowing store in {where}: {function}")
        text = text[:start] + cast + text[end:]
    return text


# The strict set (-Wconversion, -Wsign-conversion, -Wcast-qual, -Wundef,
# -Wshadow; sodium/README.md, "Warning adaptations"). Each rule spells, at
# the site, the conversion the language already performs there implicitly:
# `x op= e` on a narrower or differently signed x is `x = (T) (x op e)`, and
# a signed operand that the usual arithmetic conversions make unsigned is
# written `(U) operand`. The value is the same by definition of those
# conversions; no arithmetic changes. A rule is a pattern applied inside one
# function body (or the whole input when the scope is None) and must match
# exactly the recorded number of times, so a re-vendor that changes the text
# fails loudly. (scope, pattern, replacement, count)
def carry_rule(signed, unsigned):
    """`h -= carry * ((U) 1L << k);` on signed h and carry: both operands become U, the result is stored as S."""
    return (r'^(\s+)(\w+) -= (\w+) \* \(\(' + unsigned + r'\) 1L << (\d+)\);$',
            rf'\1\2 = ({signed}) (({unsigned}) \2 - ({unsigned}) \3 * (({unsigned}) 1L << \4));')


def parenthesised(expr):
    """True when the whole expression is one parenthesised group."""
    if not expr.startswith('(') or not expr.endswith(')'):
        return False
    depth = 0
    for i, ch in enumerate(expr):
        depth += (ch == '(') - (ch == ')')
        if depth == 0 and i != len(expr) - 1:
            return False
    return True


def cast_rule(prefix, value, element):
    """`<prefix><value>;` with the value cast to element: `(element) (value)`."""
    def cast(m):
        expr = m[2]
        return f'{m[1]}({element}) {expr if parenthesised(expr) else "(" + expr + ")"};'
    return (r'^(' + prefix + r')(' + value + r');$', cast)


# `int64_t h = <uint64_t loaded from bytes>;`: the loaded value is below 2^32.
LOADED_LIMB = cast_rule(r'\s+int64_t\s+\w+\s*=\s*', r'.*\bload_[34]\(.*', 'int64_t')
# `out[k] = (unsigned char) ((lo >> j) | (hi * ((U) 1 << i)));` on signed limbs:
# the usual arithmetic conversions make both limbs U.
SPLIT_LIMB = (r'\((\w+(?:\[\d+\])?) >> (\d+)\) \| \((\w+(?:\[\d+\])?) \* \(\((uint\d+_t)\) 1 << (\d+)\)\)',
              r'(\4) (\1 >> \2) | ((\4) \3 * ((\4) 1 << \5))')
UPSTREAM_CONVERSIONS = {
    'crypto_core/ed25519/ref10/ed25519_ref10.c': [
        ('sc25519_mul(', *LOADED_LIMB, 24), ('sc25519_mul(', *carry_rule('int64_t', 'uint64_t'), 69),
        ('sc25519_mul(', SPLIT_LIMB[0], SPLIT_LIMB[1], 10),
        ('sc25519_muladd(', *LOADED_LIMB, 36), ('sc25519_muladd(', *carry_rule('int64_t', 'uint64_t'), 69),
        ('sc25519_muladd(', SPLIT_LIMB[0], SPLIT_LIMB[1], 10),
        ('sc25519_reduce(', *LOADED_LIMB, 24), ('sc25519_reduce(', *carry_rule('int64_t', 'uint64_t'), 46),
        ('sc25519_reduce(', SPLIT_LIMB[0], SPLIT_LIMB[1], 10),
        # An int 0/1 flag into fe25519_cmov/fe25519_cneg's unsigned int.
        ('fe25519_abs(', r'fe25519_cneg\(h, f, fe25519_isnegative\(f\)\)',
         'fe25519_cneg(h, f, (unsigned int) fe25519_isnegative(f))', 1),
        ('ge25519_frombytes(', r'fe25519_cmov\(h->X, x_sqrtm1, 1 - has_m_root\)',
         'fe25519_cmov(h->X, x_sqrtm1, (unsigned int) (1 - has_m_root))', 1),
        ('ge25519_frombytes(', r'fe25519_cmov\(h->X, negx, (fe25519_isnegative\(h->X\) \^ \(\(\(s\[31\] >> 5\) \^ optblocker_u8\) >> 2\))\)',
         r'fe25519_cmov(h->X, negx, (unsigned int) (\1))', 1),
        ('ristretto255_sqrt_ratio_m1(', r'fe25519_cmov\(x, x_sqrtm1, has_p_root \| has_f_root\)',
         'fe25519_cmov(x, x_sqrtm1, (unsigned int) (has_p_root | has_f_root))', 1),
        ('ristretto255_p3_tobytes(', r'fe25519_cmov\((\w+), (\w+), rotate\)', r'fe25519_cmov(\1, \2, (unsigned int) rotate)', 3),
        ('ristretto255_p3_tobytes(', r'fe25519_cneg\(y_, y_, fe25519_isnegative\(x_z_inv\)\)',
         'fe25519_cneg(y_, y_, (unsigned int) fe25519_isnegative(x_z_inv))', 1),
        ('ristretto255_elligator(', r'fe25519_cmov\((\w+), (\w+), wasnt_square\)', r'fe25519_cmov(\1, \2, (unsigned int) wasnt_square)', 2),
        # The x86-64 asm leaves 0 or 1 in the signed char it returns as unsigned char.
        ('negative(', r'^    return b;$', '    return (unsigned char) b;', 1),
        # The portable equal() and negative() (no inline asm: s390x, for one)
        # return a 0/1 computed in uint32_t or int as unsigned char.
        ('equal(', r'^    return (\(\(y >> 29\) \^ optblocker_u8\) >> 2);', r'    return (unsigned char) (\1);', 1),
        ('negative(', r'^    return (\(\(x >> 5\) \^ optblocker_u8\) >> 2);', r'    return (unsigned char) (\1);', 1),
        # babs = |b| for b in -8..8, and equal() takes signed char.
        (None, r'(const unsigned char babs      = )(b - \(\(\(-bnegative\) & b\) \* \(\(signed char\) 1 << 1\)\));',
         r'\1(unsigned char) (\2);', 2),
        (None, r'^(\s+)s\[31\] \^= (fe25519_isnegative\(x\) << 7);$', r'\1s[31] = (unsigned char) (s[31] ^ (\2));', 2),
        (None, r'equal\(babs, (\d)\)', r'equal((signed char) babs, \1)', 16),
        (None, r'^(\s+)e\[i\] -= (carry \* \(\(signed char\) 1 << 4\));$', r'\1e[i] = (signed char) (e[i] - \2);', 2),
        # The canonical checks keep 0/1 flags in unsigned char.
        (None, r'^(\s+)c = (\(\(\(unsigned int\) c\) - 1U\) >> 8);$', r'\1c = (unsigned char) (\2);', 2),
        (None, r'^(\s+)d_ = (\(0xed - 1U - \(unsigned int\) s\[0\]\) >> 8);$', r'\1d_ = (unsigned char) (\2);', 2),
        ('sc25519_is_canonical(', r'^(\s+)c \|= (\(\(s\[i\] - L\[i\]\) >> 8\) & n);$', r'\1c = (unsigned char) (c | (\2));', 1),
        ('sc25519_is_canonical(', r'^(\s+)n &= (\(\(s\[i\] \^ L\[i\]\) - 1\) >> 8);$', r'\1n = (unsigned char) (n & (\2));', 1),
        # A = 486662 into fe25519_mul32's uint32_t.
        ('ge25519_elligator2(', r'fe25519_mul32\((\w+), (\w+), curve25519_A\[0\]\)',
         r'fe25519_mul32(\1, \2, (uint32_t) curve25519_A[0])', 2),
    ],
    'crypto_core/ed25519/ref10/fe_25_5/fe.h': [
        ('fe25519_frombytes(', *LOADED_LIMB, 10), ('fe25519_frombytes(', *carry_rule('int64_t', 'uint64_t'), 10),
        ('fe25519_reduce(', r'^(\s+)q = \(19 \* h9 \+ (\(\(uint32_t\) 1L << 24\))\) >> 25;$',
         r'\1q = (int32_t) (((uint32_t) (19 * h9) + \2) >> 25);', 1),
        ('fe25519_reduce(', *carry_rule('int32_t', 'uint32_t'), 10),
        ('fe25519_tobytes(', SPLIT_LIMB[0], SPLIT_LIMB[1], 8),
    ],
    'crypto_core/ed25519/ref10/fe_51/fe.h': [
        # Each limb is below 2^51 here.
        ('fe25519_reduce(', r'^(\s+)h\[(\d)\] = t\[\2\];$', r'\1h[\2] = (uint64_t) t[\2];', 5),
    ],
    'include/sodium/private/ed25519_ref10_fe_25_5.h': [
        ('fe25519_cmov(', r'^(\s+)(x\d) &= mask;$', r'\1\2 = (int32_t) ((uint32_t) \2 & mask);', 10),
        ('fe25519_cswap(', r'^(\s+)(x\d) &= mask;$', r'\1\2 = (int32_t) ((uint32_t) \2 & mask);', 10),
        ('fe25519_mul(', *carry_rule('int64_t', 'uint64_t'), 12),
        ('fe25519_sq(', *carry_rule('int64_t', 'uint64_t'), 12),
        ('fe25519_sq2(', *carry_rule('int64_t', 'uint64_t'), 12),
    ],
    'include/sodium/private/ed25519_ref10_fe_51.h': [
        ('fe25519_mul32(', r'^(\s+)h0 \+= (\(a >> 51\) \* 19ULL);$', r'\1h0 = (uint64_t) (h0 + \2);', 1),
    ],
    'crypto_generichash/blake2b/ref/blake2b-compress-ref.c': [
        (None, r'LOAD64_LE\(block \+ i \* sizeof m\[i\]\)', 'LOAD64_LE(block + (size_t) i * sizeof m[i])', 1),
    ],
    'crypto_generichash/blake2b/ref/blake2b-compress-ssse3.c': [
        # The message words feed only _mm_set_epi64x, whose arguments are long long.
        ('blake2b_compress_ssse3(', r'^(\s+)const uint64_t (m\d+)(\s*)= (\(\(const uint64_t \*\) block\)\[\d+\]);$',
         r'\1const int64_t \2\3= (int64_t) \4;', 16),
    ],
    'crypto_generichash/blake2b/ref/blake2b-compress-avx2.c': [
        (None, r'BLAKE2B_COMPRESS_V1\(a, b, block, S->t\[0\], S->t\[1\], S->f\[0\], S->f\[1\]\)',
         'BLAKE2B_COMPRESS_V1(a, b, block, (int64_t) S->t[0], (int64_t) S->t[1], (int64_t) S->f[0], (int64_t) S->f[1])', 1),
    ],
    'crypto_scalarmult/curve25519/ref10/x25519_ref10.c': [
        # One bit of the clamped scalar byte: the promoted int is 0 or 1.
        ('crypto_scalarmult_curve25519_ref10(', r'^(\s+)b = (t\[pos / 8\] >> \(pos & 7\));$', r'\1b = (unsigned int) (\2);', 1),
    ],
    'crypto_stream/salsa20/stream_salsa20.c': [
        (None, r'^#if HAVE_AMD64_ASM$', '#if defined(HAVE_AMD64_ASM) && HAVE_AMD64_ASM', 1),
    ],
}


BASELINE_CONVERSIONS = {
    'sodium.h': [
        # -Wundef: an unset override macro reads as 0, as #if already evaluates it.
        (None, r'^(     )NETCODE_X64 \|\| NETCODE_AVX \|\| NETCODE_AVX2$',
         r'\1(defined(NETCODE_X64) && NETCODE_X64) || (defined(NETCODE_AVX) && NETCODE_AVX) || \\\n'
         r'\1(defined(NETCODE_AVX2) && NETCODE_AVX2)', 1),
        (None, r'^# if defined\(__AVX__\) \|\| NETCODE_AVX \|\| NETCODE_AVX2$',
         '# if defined(__AVX__) || (defined(NETCODE_AVX) && NETCODE_AVX) || (defined(NETCODE_AVX2) && NETCODE_AVX2)', 1),
        (None, r'^# if \(defined\(__AVX2__\) \|\| NETCODE_AVX2\) ',
         '# if (defined(__AVX2__) || (defined(NETCODE_AVX2) && NETCODE_AVX2)) ', 1),
    ],
    'sodium.c': [
        (None, r'^(\s*)#(if|elif) (HAVE_WEAK_SYMBOLS|HAVE_ALLOCA|NETCODE_CRYPTO_LOGS)$', r'\1#\2 defined(\3) && \3', 11),
        # The 0/1 flags of the constant-time comparison are unsigned char.
        ('sodium_compare(', r'^(\s+)gt \|= (\(\(x2 - x1\) >> 8\) & eq);$', r'\1gt = (unsigned char) (gt | (\2));', 1),
        ('sodium_compare(', r'^(\s+)eq &= (\(\(x2 \^ x1\) - 1\) >> 8);$', r'\1eq = (unsigned char) (eq & (\2));', 1),
        # uint_fast16_t may be 16 bits wide, where the sum is an int.
        ('sodium_add(', r'^(\s+)c \+= (\(uint_fast16_t\) a\[i\] \+ \(uint_fast16_t\) b\[i\]);$',
         r'\1c = (uint_fast16_t) (c + \2);', 1),
        ('sodium_sub(', r'^(\s+)c = (\(uint_fast16_t\) a\[i\] - \(uint_fast16_t\) b\[i\] - c);$',
         r'\1c = (uint_fast16_t) (\2);', 1),
        # -Wcast-qual: the loads go through _mm_loadu_si128's plain pointer, so
        # the volatile on the pointed-to data was cast away at every use; the
        # pointers themselves stay volatile.
        (None, r'const volatile __m128i \*volatile (\w) =\n(\s+)\(const volatile __m128i \*volatile\) ',
         r'const __m128i *volatile \1 =\n\2(const __m128i *volatile) ', 2),
        # MSVC's __cpuid takes the leaf as int.
        ('_cpuid(', r'__cpuid\(\(int \*\) cpu_info, cpu_info_type\)', '__cpuid((int *) cpu_info, (int) cpu_info_type)', 1),
        # At most 256 bytes are requested (asserted), so the count fits an int.
        ('_randombytes_linux_getrandom(', r'readnb = getrandom\(buf, size, 0\);', 'readnb = (int) getrandom(buf, size, 0);', 1),
        # ChaCha20 state words and the 64-bit counter into the intrinsics' signed arguments.
        (None, r'(_mm(?:256)?_set1_epi32)\(x\[(\d+)\]\)', r'\1((int) x[\2])', 42),
        (None, r'_mm_set1_epi64x\(in1213\)', '_mm_set1_epi64x((int64_t) in1213)', 4),
        (None, r'_mm_cvtsi64_si128\(in1213\)', '_mm_cvtsi64_si128((int64_t) in1213)', 1),
        (None, r'^(\s+)x\[13\] = (\(in1213 >> 32\) & 0xFFFFFFFF);$', r'\1x[13] = (uint32_t) (\2);', 3),
        # Poly1305: the block mask is the int ~(16 - 1) converted to the length's type.
        (None, r'\(bytes & ~\(poly1305_block_size - 1\)\)', '(bytes & (unsigned long long) ~(poly1305_block_size - 1))', 2),
        (None, r'blocks = inlen & ~31;', 'blocks = inlen & (unsigned long long) ~31;', 1),
        (None, r'_mm_cvtsi32_si128\((st->R\d?\[4\])\)', r'_mm_cvtsi32_si128((int) \1)', 5),
        (None, r'^(\s+)(t\d) = _mm_cvtsi128_si32\((T\d)\)', r'\1\2 = (uint32_t) _mm_cvtsi128_si32(\3)', 5),
    ],
}


def convert(text, rules, where):
    """Applies each rule inside its scope; the number of matches must be the recorded count."""
    for scope, pattern, replacement, expected in rules:
        start, end = 0, len(text)
        if scope is not None:
            if text.count('\n' + scope) != 1:
                raise SystemExit(f"conversion scope does not match once in {where}: {scope}")
            start = text.index('\n' + scope)
            end = text.index('\n}\n', start)
        body, count = re.subn(pattern, replacement, text[start:end], flags=re.M)
        if count != expected:
            raise SystemExit(f"conversion matched {count} times, not {expected}, in {where}: {scope} {pattern[:60]!r}")
        text = text[:start] + body + text[end:]
    return text


def adapt(text, replacements, where):
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"warning adaptation does not match once in {where}: {old[:60]!r}")
        text = text.replace(old, new)
    return text


def checked(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f"SHA256 mismatch: {path.name}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True,
                        help='directory containing netcode/sodium.{c,h} and libsodium-1.0.22.tar.gz')
    parser.add_argument('--output', type=Path, default=ROOT / 'sodium')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--fetch', action='store_true', help='fetch missing pinned inputs over HTTPS')
    args = parser.parse_args()
    # Explicit UTF-8: read_text() otherwise decodes with the locale's code
    # page, which on Windows is not UTF-8.
    manifest = json.loads((ROOT / 'sodium/manifest.json').read_text(encoding='utf-8'))
    if args.fetch:
        downloads = {
            **{f'netcode/{name}': (f"https://raw.githubusercontent.com/mas-bandwidth/netcode/{manifest['baseline_commit']}/sodium/{name}", digest)
               for name, digest in manifest['baseline_sha256'].items()},
            f"libsodium-{manifest['upstream_version']}.tar.gz":
                (f"https://github.com/jedisct1/libsodium/releases/download/{manifest['upstream_version']}-RELEASE/libsodium-{manifest['upstream_version']}.tar.gz", manifest['archive_sha256'])}
        for name, (url, digest) in downloads.items():
            path = args.inputs / name
            if not path.exists():
                with urllib.request.urlopen(url, timeout=60) as response:
                    data = response.read()
                if hashlib.sha256(data).hexdigest() != digest:
                    raise SystemExit(f"download SHA256 mismatch: {name}")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
    baseline = {name: checked(args.inputs / 'netcode' / name, digest).decode()
                for name, digest in manifest['baseline_sha256'].items()}
    archive = checked(args.inputs / f"libsodium-{manifest['upstream_version']}.tar.gz",
                      manifest['archive_sha256'])
    prefix = f"libsodium-{manifest['upstream_version']}/src/libsodium/"
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        files = {m.name[len(prefix):]: tar.extractfile(m).read().decode()
                 for m in tar.getmembers() if m.isfile() and m.name.startswith(prefix)}
    for path, replacements in UPSTREAM_ADAPTATIONS.items():
        files[path] = adapt(files[path], replacements, path)
    for path, stores in NARROWING_STORES.items():
        files[path] = cast_narrowing_stores(files[path], stores, path)
    for path, rules in UPSTREAM_CONVERSIONS.items():
        files[path] = convert(files[path], rules, path)
    for name, replacements in BASELINE_ADAPTATIONS.items():
        baseline[name] = adapt(baseline[name], replacements, name)
    for name, replacements in SANITIZER_ADAPTATIONS.items():
        baseline[name] = adapt(baseline[name], replacements, name)
    for name, rules in BASELINE_CONVERSIONS.items():
        baseline[name] = convert(baseline[name], rules, name)
    baseline_guards = set(re.findall(r'^#ifndef (\w+)', baseline['sodium.h'], re.M))
    public_root = 'include/sodium/'
    used = set()
    local_macros = set()

    def expand(path, parents=()):
        if path in parents:
            raise SystemExit(f"recursive include: {path}")
        source = files[path]
        guard = re.search(r'^#ifndef (\w+)', source, re.M)
        if path.endswith('.h') and guard and guard[1] in baseline_guards:
            return f'/* {path}: provided by the pinned baseline. */\n'
        used.add(path)
        if path in manifest['local_macro_headers']:
            local_macros.update(DEFINE.findall(source))

        def inline(match):
            name = match[1]
            # Archive member names are POSIX on every host; pathlib would join
            # them with backslashes on Windows and miss every relative include.
            candidates = [posixpath.normpath(posixpath.join(posixpath.dirname(path), name)), public_root + name]
            target = next((x for x in candidates if x in files), None)
            if target is None:
                raise SystemExit(f"unresolved include {name} from {path}")
            return expand(target, (*parents, path))

        return f'\n/* ===== upstream {path} ===== */\n' + INCLUDE.sub(inline, source)

    # netcode's baseline predates upstream's precise nullable/zero-length
    # annotations. Refresh only existing nonnull attributes from the pinned
    # declarations; do not suppress sanitizer diagnostics for valid calls.
    prototype = re.compile(r'^\w[^;{}]*?\b((?:crypto_|sodium_|randombytes_)\w+)\s*\([^;{}]*;', re.M)
    nonnull = re.compile(r'__attribute__\s*\(\(\s*nonnull(?:\s*\([^)]*\))?\s*\)\)')
    attributes = {}
    for path, text in files.items():
        if path.startswith(public_root) and path.endswith('.h'):
            for declaration in prototype.finditer(text):
                attribute = nonnull.search(declaration[0])
                name = declaration[1]
                value = attribute[0] if attribute else ''
                if name in attributes and attributes[name] != value:
                    raise SystemExit(f"conflicting upstream nonnull declarations: {name} in {path}")
                attributes[name] = value

    def refresh(declaration):
        name = declaration[1]
        if name not in attributes:
            return declaration[0]
        return nonnull.sub(lambda _: attributes[name], declaration[0])

    baseline['sodium.h'] = prototype.sub(refresh, baseline['sodium.h'])

    # Public headers retain their upstream guards and C++ linkage. Private
    # declarations/body tables are expanded at the original include sites,
    # preserving all architecture conditions and inclusion scopes.
    extension_header = '\n#define SODIUM_EXPORT\n#define SODIUM_EXPORT_WEAK\n'
    extension_header += ''.join(expand(public_root + name) for name in manifest['public_headers'])
    extension_header += '''
/* Initialization of the newly included upstream implementation families. */
int _crypto_generichash_blake2b_pick_best_implementation(void);
int _crypto_scalarmult_curve25519_pick_best_implementation(void);
int _crypto_stream_salsa20_pick_best_implementation(void);
'''
    header = baseline['sodium.h'].replace('#endif /* SODIUM_AMALGAM_H */',
                                       extension_header + '\n#endif /* SODIUM_AMALGAM_H */')
    anchor = '    _crypto_stream_chacha20_pick_best_implementation();'
    assert baseline['sodium.c'].count(anchor) == 1
    source = baseline['sodium.c'].replace(anchor, anchor + '''
    _crypto_generichash_blake2b_pick_best_implementation();
    _crypto_scalarmult_curve25519_pick_best_implementation();
    _crypto_stream_salsa20_pick_best_implementation();''')
    # GCC's target pragma persists across concatenated C files. Preserve
    # the baseline's architecture dispatch while restoring options at every
    # original translation-unit boundary, just as a separate build would.
    push = '#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#endif\n'
    pop = '\n#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif\n'
    source = source.replace('#include "sodium.h"', '#include "sodium.h"\n#if defined(HAVE_AVXINTRIN_H)\n#include <immintrin.h>\n#endif')
    sections = re.split(r'(?=/\* =+ sodium_[^\n]+\.c =+ \*/)', source)
    source = sections[0] + ''.join(push + unit + pop for unit in sections[1:])
    # This standard feature-test macro belongs to the whole translation
    # unit; undefining it is itself diagnosed by GCC.
    source = source.replace('#undef __STDC_WANT_LIB_EXT1__\n', '')
    for path in manifest['sources']:
        names = manifest['renames'].get(path, [])
        tag = re.sub(r'\W', '_', path)
        disabled = manifest.get('disabled_features', {}).get(path, [])
        source += ''.join(f'#pragma push_macro("{name}")\n#undef {name}\n' for name in disabled)
        local_macros.clear()
        source += '\n' + ''.join(f'#define {name} rn_{tag}_{name}\n' for name in names)
        source += '#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#endif\n'
        source += expand(path)
        source += '\n#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif\n'
        # A C file's local macros must not leak into the next translation
        # unit. Header guards and public constants keep their usual lifetime.
        macros = set(DEFINE.findall(files[path])) | set(names) | local_macros
        source += '\n' + ''.join(f'#undef {name}\n' for name in sorted(macros))
        source += ''.join(f'#pragma pop_macro("{name}")\n' for name in reversed(disabled))

    # Namespace both public and internal symbols so a static consumer may
    # link its own libsodium. Constants and function-like macros stay as
    # upstream wrote them; all algorithm source text remains unchanged.
    macro_names = set(DEFINE.findall(header + source))
    symbols = sorted(set(re.findall(
        r'\b(?:(?:_?crypto_|_?sodium_|randombytes_|blake2b_|fe25519_|ge25519_|sc25519_|ristretto255_|chacha20_dolbeau_|poly1305_sse2_)[a-z0-9_]+|randombytes|blake2b)\b',
        header + source)) - macro_names)
    namespace = '/* Private RocketNet namespace; do not combine with a system sodium header. */\n'
    namespace += ''.join(f'#define {name} rocketnet_sodium_{name}\n' for name in symbols)
    header = header.replace('#define SODIUM_AMALGAM_H', '#define SODIUM_AMALGAM_H\n\n' + namespace)
    banner = ('/* Generated by tools/generate_sodium.py; pinned inputs and source list\n'
              ' * are in sodium/manifest.json. See sodium/README.md for provenance. */\n')
    outputs = {'sodium.h': banner + header, 'sodium.c': banner + source}
    for name, value in outputs.items():
        path = args.output / name
        if args.check:
            if not path.is_file() or path.read_bytes() != value.encode():
                raise SystemExit(f"generated output differs: {name}")
        else:
            args.output.mkdir(parents=True, exist_ok=True)
            path.write_bytes(value.encode())
    print(f"sodium: {'verified' if args.check else 'generated'} two files; "
          f"{len(manifest['sources'])} added source files, {len(used)} upstream inputs")


if __name__ == '__main__':
    main()
