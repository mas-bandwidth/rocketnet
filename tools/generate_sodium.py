#!/usr/bin/env python3
"""Build the two-file sodium subset from the pinned libsodium release archive.

Every bundled family -- the ChaCha20/Poly1305 AEAD, random, core, runtime and
utilities as much as the extension -- is amalgamated from the pinned upstream
release recorded in sodium/manifest.json, so the committed pair is traceable to
that release. The archive carries no generated config.h, so the amalgamation
derives the handful of build macros that matter for the bundled slice from the
compiler and target. Ordinary builds consume the committed pair offline.
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
PUBLIC_ROOT = 'include/sodium/'

# libsodium normally gets these from autoconf's config.h; the pinned release
# archive ships no generated config.h, so derive them from the compiler.
CONFIG = r'''
/* Build configuration for the pinned upstream release. libsodium normally gets
 * these from autoconf's config.h; the release archive ships no generated
 * config.h, so the amalgamation derives the ones the bundled slice needs from
 * the compiler and target. See sodium/README.md. */
#define CONFIGURED 1
#if !defined(HAVE_TI_MODE) && defined(__SIZEOF_INT128__)
# define HAVE_TI_MODE 1
#endif
#if !defined(HAVE_GCC_MEMORY_FENCES) && (defined(__GNUC__) || defined(__clang__))
# define HAVE_GCC_MEMORY_FENCES 1
#endif
#if !defined(HAVE_INLINE_ASM) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
# define HAVE_INLINE_ASM 1
#endif
#if defined(__clang__) || defined(__GNUC__)
# if defined(__x86_64__) || defined(__amd64__) || defined(__i386__) || defined(__i686__)
#  define HAVE_MMINTRIN_H   1
#  define HAVE_EMMINTRIN_H  1
#  define HAVE_PMMINTRIN_H  1
#  define HAVE_TMMINTRIN_H  1
#  define HAVE_SMMINTRIN_H  1
#  define HAVE_AVXINTRIN_H  1
#  define HAVE_WMMINTRIN_H  1
#  define HAVE_AVX2INTRIN_H 1
#  define HAVE_CPUID        1
# endif
#endif
'''


def checked(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f"SHA256 mismatch: {path.name}")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True,
                        help='directory holding libsodium-<version>.tar.gz')
    parser.add_argument('--output', type=Path, default=ROOT / 'sodium')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--fetch', action='store_true', help='fetch the pinned archive over HTTPS')
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'sodium/manifest.json').read_text())
    if args.fetch:
        name = f"libsodium-{manifest['upstream_version']}.tar.gz"
        path = args.inputs / name
        if not path.exists():
            url = (f"https://github.com/jedisct1/libsodium/releases/download/"
                   f"{manifest['upstream_version']}-RELEASE/{name}")
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != manifest['archive_sha256']:
                raise SystemExit(f"download SHA256 mismatch: {name}")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    archive = checked(args.inputs / f"libsodium-{manifest['upstream_version']}.tar.gz",
                      manifest['archive_sha256'])
    prefix = f"libsodium-{manifest['upstream_version']}/src/libsodium/"
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        files = {m.name[len(prefix):]: tar.extractfile(m).read().decode()
                 for m in tar.getmembers() if m.isfile() and m.name.startswith(prefix)}
    # The version header ships as a configure template; instantiate the pin.
    version = files['include/sodium/version.h.in']
    version = version.replace('@VERSION@', manifest['upstream_version'])
    version = version.replace('@SODIUM_LIBRARY_VERSION_MAJOR@', str(manifest['library_version_major']))
    version = version.replace('@SODIUM_LIBRARY_VERSION_MINOR@', str(manifest['library_version_minor']))
    version = version.replace('@SODIUM_LIBRARY_MINIMAL_DEF@', '')
    files['include/sodium/version.h'] = version
    # Families this slice does not carry: drop only their initialization calls.
    for path, drops in manifest.get('prune', {}).items():
        for drop in drops:
            if drop not in files[path]:
                raise SystemExit(f"prune string absent from {path}: {drop!r}")
            files[path] = files[path].replace(drop, '')

    provided = set()
    for path in manifest['headers'] + [PUBLIC_ROOT + name for name in manifest['public_headers']]:
        guard = re.search(r'^#ifndef (\w+)', files[path], re.M)
        if guard:
            provided.add(guard[1])
    used = set()
    local_macros = set()

    def emit(path, parents=()):
        if path in parents:
            raise SystemExit(f"recursive include: {path}")
        source = files[path]
        used.add(path)
        if path in manifest['local_macro_headers']:
            local_macros.update(DEFINE.findall(source))

        def inline(match):
            name = match[1]
            candidates = [posixpath.normpath(str(Path(path).parent / name)), PUBLIC_ROOT + name]
            target = next((x for x in candidates if x in files), None)
            if target is None:
                return match[0]  # inactive optional include: leave to the preprocessor
            return expand(target, (*parents, path))

        return f'\n/* ===== upstream {path} ===== */\n' + INCLUDE.sub(inline, source)

    def expand(path, parents=()):
        guard = re.search(r'^#ifndef (\w+)', files[path], re.M)
        if path.endswith('.h') and guard and guard[1] in provided:
            return f'/* {path}: provided earlier in this amalgamation. */\n'
        return emit(path, parents)

    extension = ''.join(emit(PUBLIC_ROOT + name) for name in manifest['public_headers'])
    extension += '''
/* Initialization of the upstream implementation families. */
int _crypto_generichash_blake2b_pick_best_implementation(void);
int _crypto_scalarmult_curve25519_pick_best_implementation(void);
int _crypto_stream_salsa20_pick_best_implementation(void);
'''
    header = ('#ifndef SODIUM_AMALGAM_H\n#define SODIUM_AMALGAM_H\n\n'
              + CONFIG
              + ''.join(emit(path) for path in manifest['headers'])
              + extension
              + '\n#endif /* SODIUM_AMALGAM_H */\n')

    push = '#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#endif\n'
    pop = '\n#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif\n'
    source = '#include "sodium.h"\n#if defined(HAVE_AVXINTRIN_H)\n#include <immintrin.h>\n#endif\n'
    for path in manifest['sources']:
        names = manifest['renames'].get(path, [])
        tag = re.sub(r'\W', '_', path)
        disabled = manifest.get('disabled_features', {}).get(path, [])
        source += ''.join(f'#pragma push_macro("{name}")\n#undef {name}\n' for name in disabled)
        local_macros.clear()
        source += '\n' + ''.join(f'#define {name} rn_{tag}_{name}\n' for name in names)
        source += push
        source += expand(path)
        source += pop
        macros = set(DEFINE.findall(files[path])) | set(names) | local_macros
        source += '\n' + ''.join(f'#undef {name}\n' for name in sorted(macros))
        source += ''.join(f'#pragma pop_macro("{name}")\n' for name in reversed(disabled))
    # This standard feature-test macro belongs to the whole translation unit.
    source = source.replace('#undef __STDC_WANT_LIB_EXT1__\n', '')

    # Namespace both public and internal symbols so a static consumer may link
    # its own libsodium. Constants and function-like macros stay as upstream.
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
          f"{len(manifest['sources'])} source files, {len(used)} upstream inputs")


if __name__ == '__main__':
    main()
