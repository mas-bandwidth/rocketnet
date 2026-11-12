#!/usr/bin/env python3
"""Compare the namespaced two-file vendor with independently linked libsodium."""
import argparse
import ctypes as c
import ctypes.util
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vendor', type=Path, help='test-only sodium oracle shared library')
    parser.add_argument('--reference', help='system/pinned libsodium shared library')
    args = parser.parse_args()
    reference = args.reference or ctypes.util.find_library('sodium')
    if not reference:
        raise SystemExit('independent libsodium reference not found')
    libraries = [c.CDLL(str(args.vendor.resolve())), c.CDLL(reference)]
    prefixes = ['rocketnet_sodium_', '']
    ptr, size, ull = c.c_void_p, c.c_size_t, c.c_ulonglong
    signatures = {
        'sodium_init': [], 'sodium_version_string': [],
        'crypto_generichash': [ptr, size, ptr, ull, ptr, size],
        'crypto_hash_sha512': [ptr, ptr, ull],
        'crypto_shorthash': [ptr, ptr, ull, ptr],
        'crypto_scalarmult_base': [ptr, ptr], 'crypto_scalarmult': [ptr, ptr, ptr],
        'crypto_sign_seed_keypair': [ptr, ptr, ptr],
        'crypto_sign_detached': [ptr, ptr, ptr, ull, ptr],
        'crypto_sign_verify_detached': [ptr, ptr, ull, ptr],
        'crypto_box_seed_keypair': [ptr, ptr, ptr],
        'crypto_box_seal': [ptr, ptr, ull, ptr],
        'crypto_box_seal_open': [ptr, ptr, ull, ptr, ptr],
        'crypto_aead_chacha20poly1305_ietf_encrypt': [ptr, ptr, ptr, ull, ptr, ull, ptr, ptr, ptr],
        'crypto_aead_chacha20poly1305_ietf_decrypt': [ptr, ptr, ptr, ptr, ull, ptr, ull, ptr, ptr],
    }
    api = []
    for library, prefix in zip(libraries, prefixes):
        functions = {}
        for name, parameters in signatures.items():
            fn = getattr(library, prefix + name)
            fn.argtypes = parameters
            fn.restype = c.c_char_p if name == 'sodium_version_string' else c.c_int
            functions[name] = fn
        assert functions['sodium_init']() >= 0
        api.append(functions)
    print('vendor:', api[0]['sodium_version_string']().decode(),
          'reference:', api[1]['sodium_version_string']().decode())
    # the vendor's selection report (sodium/README.md, "AEAD dispatch and AArch64")
    report = []
    for name in ['aead_chacha20poly1305_ietf', 'chacha20', 'poly1305']:
        fn = getattr(libraries[0], f'rocketnet_sodium_sodium_runtime_{name}_implementation')
        fn.argtypes = []
        fn.restype = c.c_char_p
        report.append(f'{name}={fn().decode()}')
    print('vendor picks:', ' '.join(report))

    def call(side, name, *values):
        return api[side][name](*values)

    def output(side, name, count, *values):
        out = c.create_string_buffer(count)
        assert call(side, name, out, *values) == 0, name
        return out.raw

    def same(name, count, *values):
        left = output(0, name, count, *values)
        right = output(1, name, count, *values)
        assert left == right, name
        return left

    seed = bytes(range(32))
    scalar = bytes.fromhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
    public = same('crypto_scalarmult_base', 32, scalar)
    assert public.hex() == '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a'
    same('crypto_scalarmult', 32, seed, public)
    for side in range(2):
        assert call(side, 'crypto_scalarmult', c.create_string_buffer(32), seed, bytes(32)) == -1

    keypairs = {}
    for family, secret_bytes in [('sign', 64), ('box', 32)]:
        pairs = []
        for side in range(2):
            pk, sk = c.create_string_buffer(32), c.create_string_buffer(secret_bytes)
            assert call(side, f'crypto_{family}_seed_keypair', pk, sk, seed) == 0
            pairs.append((pk.raw, sk.raw))
        assert pairs[0] == pairs[1], family + ' keypair'
        keypairs[family] = pairs[0]

    for length in [0, 1, 15, 16, 31, 32, 63, 64, 65, 127, 128, 129, 191, 192, 193, 255, 256, 257,
                   511, 512, 513, 1024, 1200, 1400, 4096]:
        message = bytes((i * 37 + 11) % 256 for i in range(length))
        same('crypto_hash_sha512', 64, message, length)
        same('crypto_shorthash', 8, message, length, seed[:16])
        for key in [b'', seed]:
            for digest_bytes in [16, 32, 64]:
                same('crypto_generichash', digest_bytes, digest_bytes, message, length, key, len(key))
        pk, sk = keypairs['sign']
        signature = same('crypto_sign_detached', 64, None, message, length, sk)
        bad = bytes([signature[0] ^ 1]) + signature[1:]
        for side in range(2):
            assert call(side, 'crypto_sign_verify_detached', signature, message, length, pk) == 0
            assert call(side, 'crypto_sign_verify_detached', bad, message, length, pk) == -1
        for associated in [b'', bytes(range(37))]:
            nonce = bytes(range(12))
            encrypted = same('crypto_aead_chacha20poly1305_ietf_encrypt', length + 16,
                             None, message, length, associated, len(associated), None, nonce, seed)
            corrupt = encrypted[:-1] + bytes([encrypted[-1] ^ 1])
            for side in range(2):
                plain = c.create_string_buffer(max(1, length))
                written = ull(0)
                assert call(side, 'crypto_aead_chacha20poly1305_ietf_decrypt', plain, c.byref(written),
                            None, encrypted, len(encrypted), associated, len(associated), nonce, seed) == 0
                assert written.value == length and plain.raw[:length] == message
                # a forgery writes no plaintext: the reference (and the
                # vendor's composed AEAD) zero the output, the vendor's one
                # pass leaves it as it was (sodium/local/dispatch.h.in)
                poisoned = c.create_string_buffer(b'\xa5' * max(1, length), max(1, length))
                assert call(side, 'crypto_aead_chacha20poly1305_ietf_decrypt', poisoned, c.byref(written),
                            None, corrupt, len(corrupt), associated, len(associated), nonce, seed) == -1
                left = poisoned.raw[:length]
                allowed = [bytes(length)] + ([b'\xa5' * length] if side == 0 else [])
                assert written.value == 0 and left in allowed
        # Sealed boxes intentionally use fresh ephemeral keys. Compare by
        # opening each side's ciphertext on the other implementation.
        pk, sk = keypairs['box']
        for side in range(2):
            sealed = output(side, 'crypto_box_seal', length + 48, message, length, pk)
            plain = output(1 - side, 'crypto_box_seal_open', max(1, length), sealed, len(sealed), pk, sk)
            assert plain[:length] == message
            corrupt = sealed[:-1] + bytes([sealed[-1] ^ 1])
            assert call(1 - side, 'crypto_box_seal_open', c.create_string_buffer(max(1, length)),
                        corrupt, len(corrupt), pk, sk) == -1
    print('test_sodium_independent_parity_and_cross_open')


if __name__ == '__main__':
    main()
