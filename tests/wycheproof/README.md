# tests/wycheproof/

Vendored Wycheproof known-answer vectors, exactly the primitives rocketnet
calls through the vendored sodium subset:

- `chacha20_poly1305_test.json` — ChaCha20-Poly1305 (IETF RFC 8439), the
  seal `rocketnet_write_sealed_packet` and the open
  `rocketnet_open_sealed_packet` perform (sdk/rocketnet_protocol.cpp).
- `x25519_test.json` — X25519, the key derivation
  `rocketnet_derive_connection_keys` performs (sdk/rocketnet_keys.cpp).
- `ed25519_test.json` — Ed25519, the verification
  `rocketnet_verify_signed_connect_data` performs
  (sdk/rocketnet_connect_token.cpp).

Source: C2SP/wycheproof, commit `3fa63dd0344abb611f1fb1d77e119938603ea230`
(2026-09-02). License: Apache-2.0 (beside these files, `LICENSE`).

The fetch (run from `tests/wycheproof/`):

    curl -fsSL -o chacha20_poly1305_test.json \
      https://raw.githubusercontent.com/C2SP/wycheproof/3fa63dd0344abb611f1fb1d77e119938603ea230/testvectors_v1/chacha20_poly1305_test.json
    curl -fsSL -o x25519_test.json \
      https://raw.githubusercontent.com/C2SP/wycheproof/3fa63dd0344abb611f1fb1d77e119938603ea230/testvectors_v1/x25519_test.json
    curl -fsSL -o ed25519_test.json \
      https://raw.githubusercontent.com/C2SP/wycheproof/3fa63dd0344abb611f1fb1d77e119938603ea230/testvectors_v1/ed25519_test.json
    curl -fsSL -o LICENSE \
      https://raw.githubusercontent.com/C2SP/wycheproof/3fa63dd0344abb611f1fb1d77e119938603ea230/LICENSE

sha256 checksums (verify with `sha256sum -c SHA256SUMS`):

    fe61d25f90e1bde4461d00eafe61049e5f29bd999f36b766df9cda90906ad53d  chacha20_poly1305_test.json
    35c3f5231cf25cc640b524d403461deee9e49441d5d915a3a25b2c8ff5adbe7d  x25519_test.json
    752d2ea7d7c6cf4736381b6cbacb61f8182b126ab7cd9b058f00c50084975536  ed25519_test.json
    58d1e17ffe5109a7ae296caafcadfdbe6a7d176f0bc4ab01e12a689b0499d8bd  LICENSE

These files are never edited; a pin move re-fetches them and rewrites
SHA256SUMS. They are never read at run time: the unit build compiles the three
`*_test.json` files into `./bin/test` (cmake/EmbedTestFixtures.cmake, rebuilt
when a file changes), and tests/wycheproof.h parses the embedded bytes, so the
`test_wycheproof_*` cases are unit cases that open no file.
