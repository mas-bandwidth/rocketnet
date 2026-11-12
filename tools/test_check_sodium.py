#!/usr/bin/env python3
"""Tests for tools/check_sodium.py: a vendor library whose output differs
from the reference is refused, two libraries that agree pass, and an empty
file given as the libraries is refused.

check_sodium.py loads two shared libraries with ctypes. These tests build
none: a subprocess runs check_sodium.main() with check_sodium.load replaced by
FakeLibrary, a small stand-in written with hashlib that keeps the properties
the tool checks (the published X25519 base-point vector, refusal of the zero
point, of a flipped signature bit and of a corrupt ciphertext, and opening of
the other side's sealed box). The planted break makes the vendor's SHA-512
differ in one bit. The empty-file case runs the real tool as a process."""

import ctypes as c
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TOOL = TOOLS / "check_sodium.py"
PREFIX = "rocketnet_sodium_"
BASE_SCALAR = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
BASE_PUBLIC = bytes.fromhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")

RUNNER = """import sys
sys.path.insert(0, {tools!r})
import check_sodium, test_check_sodium
check_sodium.load = test_check_sodium.loader(sys.argv.pop(1))
check_sodium.main()
"""


def h(*parts):
    digest = hashlib.sha512()
    for part in parts:
        digest.update(bytes(part))
    return digest.digest()


def stream(key, nonce, count):
    out = b""
    block = 0
    while len(out) < count:
        out += h(b"ks", key, nonce, block.to_bytes(8, "little"))
        block += 1
    return out[:count]


def xor(data, key, nonce):
    return bytes(a ^ b for a, b in zip(data, stream(key, nonce, len(data))))


def put(buffer, data):
    if data:
        c.memmove(buffer, data, len(data))


class FakeLibrary:
    """Exposes each function under its plain and its rocketnet_sodium_ name."""

    def __init__(self, differs):
        self.differs = differs

    def __getattr__(self, name):
        method = getattr(self, "_" + (name[len(PREFIX):] if name.startswith(PREFIX) else name))

        def function(*args):
            return method(*args)
        return function

    def _sodium_init(self):
        return 0

    def _sodium_version_string(self):
        return b"fake"

    def _crypto_hash_sha512(self, out, m, mlen):
        digest = hashlib.sha512(m[:mlen]).digest()
        if self.differs:
            digest = bytes([digest[0] ^ 1]) + digest[1:]
        put(out, digest)
        return 0

    def _crypto_shorthash(self, out, m, mlen, k):
        put(out, h(b"short", k, m[:mlen])[:8])
        return 0

    def _crypto_generichash(self, out, outlen, m, mlen, key, keylen):
        put(out, hashlib.blake2b(m[:mlen], digest_size=outlen, key=key[:keylen]).digest())
        return 0

    def _crypto_scalarmult_base(self, out, n):
        put(out, BASE_PUBLIC if bytes(n) == BASE_SCALAR else h(b"base", n)[:32])
        return 0

    def _crypto_scalarmult(self, out, n, p):
        if bytes(p) == bytes(32):
            return -1
        put(out, h(b"dh", n, p)[:32])
        return 0

    def _crypto_sign_seed_keypair(self, pk, sk, seed):
        public = h(b"sign", seed)[:32]
        put(pk, public)
        put(sk, bytes(seed) + public)
        return 0

    def _crypto_sign_detached(self, sig, siglen, m, mlen, sk):
        put(sig, h(b"sig", bytes(sk)[32:], m[:mlen]))
        return 0

    def _crypto_sign_verify_detached(self, sig, m, mlen, pk):
        return 0 if bytes(sig) == h(b"sig", pk, m[:mlen]) else -1

    def _crypto_box_seed_keypair(self, pk, sk, seed):
        secret = h(b"box", seed)[:32]
        put(pk, h(b"boxpk", secret)[:32])
        put(sk, secret)
        return 0

    def _crypto_box_seal(self, out, m, mlen, pk):
        ephemeral = h(b"eph", m[:mlen], pk)[:32]
        body = xor(m[:mlen], pk, ephemeral)
        put(out, ephemeral + body + h(b"tag", pk, ephemeral, body)[:16])
        return 0

    def _crypto_box_seal_open(self, out, sealed, clen, pk, sk):
        sealed = bytes(sealed)[:clen]
        ephemeral, body, tag = sealed[:32], sealed[32:-16], sealed[-16:]
        if bytes(pk) != h(b"boxpk", sk)[:32] or tag != h(b"tag", pk, ephemeral, body)[:16]:
            return -1
        put(out, xor(body, pk, ephemeral))
        return 0

    def _crypto_aead_chacha20poly1305_ietf_encrypt(self, out, clen, m, mlen, ad, adlen, nsec, npub, k):
        body = xor(m[:mlen], k, npub)
        put(out, body + h(b"aead", k, npub, ad[:adlen], body)[:16])
        return 0

    def _crypto_aead_chacha20poly1305_ietf_decrypt(self, out, mlen, nsec, sealed, clen, ad, adlen, npub, k):
        sealed = bytes(sealed)[:clen]
        body, tag = sealed[:-16], sealed[-16:]
        if tag != h(b"aead", k, npub, ad[:adlen], body)[:16]:
            return -1
        put(out, xor(body, k, npub))
        mlen._obj.value = len(body)
        return 0


def loader(mode):
    """mode "agree": both libraries agree; "vendor-differs": the vendor's SHA-512 is wrong."""
    def load(path):
        return FakeLibrary(mode == "vendor-differs" and Path(path).name == "vendor.so")
    return load


class CheckSodiumTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.directory = Path(self.tmp.name)
        self.vendor = self.directory / "vendor.so"
        self.reference = self.directory / "reference.so"
        self.vendor.write_bytes(b"")
        self.reference.write_bytes(b"")

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, mode):
        return subprocess.run([sys.executable, "-c", RUNNER.format(tools=str(TOOLS)), mode,
                               str(self.vendor), "--reference", str(self.reference)],
                              capture_output=True, text=True, cwd=self.directory)

    def test_agreeing_libraries_pass(self):
        result = self.run_main("agree")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("test_sodium_independent_parity_and_cross_open", result.stdout)

    def test_a_vendor_that_differs_is_refused(self):
        result = self.run_main("vendor-differs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("crypto_hash_sha512", result.stderr)

    def test_an_empty_library_file_is_refused(self):
        result = subprocess.run([sys.executable, str(TOOL), str(self.vendor),
                                 "--reference", str(self.reference)],
                                capture_output=True, text=True, cwd=self.directory)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
