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
    manifest = json.loads((ROOT / 'sodium/manifest.json').read_text())
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
            candidates = [posixpath.normpath(str(Path(path).parent / name)), public_root + name]
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
                attributes[declaration[1]] = attribute[0] if attribute else ''

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
