#!/usr/bin/env python3
"""Check whether the XDP server transport is compiled into a Linux library.

    check_xdp_compiled_out.py --absent  <shared> <archive>
    check_xdp_compiled_out.py --present <shared> <archive>

--absent is the build without XDP (CMakeLists.txt: no clang with the BPF
target, no libbpf or libxdp, or ROCKETNET_XDP=OFF): the archive defines no XDP
transport, netlink gateway or embedded program symbol, references no libbpf or
libxdp function, and the shared library does not link libbpf or libxdp. The
only xdp names left are the registry row's refusal (rocketnet_xdp_transport,
its operations table and its xdp_not_built_ functions) and the public
rocketnet_forbid_xdp_commit, a no-op there.

--present is the build with XDP, the reversed witness: the transport, the
gateway, the embedded program and the libbpf and libxdp links are all there.
"""

import argparse
import re
import subprocess
import sys

# a defined symbol of the XDP transport, its netlink gateway or its embedded
# BPF object (demangled names, any binding)
XDP_DEFINED = re.compile(r"(xdp|gateway_netlink|rocketnet_server_socket_)", re.I)

# what a build without XDP keeps: the registry row's refusal and the public no-op
ALLOWED_DEFINED = re.compile(r"^(rocketnet_xdp_transport|xdp_operations|xdp_not_built_\w+|rocketnet_forbid_xdp_commit)\b")

# a reference into libbpf or libxdp
XDP_IMPORT = re.compile(r"^(bpf_|libbpf_|btf_|xsk_|libxdp_|xdp_program__)")

XDP_LIBRARIES = re.compile(r"\blib(bpf|xdp)\.so")

# names the XDP build must define
REQUIRED_DEFINED = ("rocketnet_server_socket_create", "rocketnet_gateway_netlink_route", "rocketnet_server_xdp_o")
REQUIRED_IMPORTS = ("bpf_object__open_mem", "xsk_socket__create")


def symbol_name(line):
    """The demangled identifier of one `nm -C` line, without its arguments."""
    parts = line.split(None, 2)
    if len(parts) == 2 and parts[0] == "U":
        name = parts[1]
    elif len(parts) == 3:
        name = parts[2]
    else:
        return None, None
    kind = parts[0] if len(parts) == 2 else parts[1]
    return kind, re.split(r"[(\s\[]", name, maxsplit=1)[0]


def classify(nm_lines, needed_lines):
    """The defined XDP symbols, the libbpf/libxdp imports and the linked XDP libraries."""
    defined, imports = set(), set()
    for line in nm_lines:
        if not line.strip() or line.endswith(":"):
            continue
        kind, name = symbol_name(line.strip())
        if not name:
            continue
        if kind == "U":
            if XDP_IMPORT.match(name):
                imports.add(name)
        elif XDP_DEFINED.search(name):
            defined.add(name)
    libraries = {match.group(0) for line in needed_lines for match in [XDP_LIBRARIES.search(line)] if match}
    return defined, imports, libraries


def absent_problems(defined, imports, libraries):
    problems = [f"XDP symbol defined: {name}" for name in sorted(defined) if not ALLOWED_DEFINED.match(name)]
    problems += [f"libbpf/libxdp referenced: {name}" for name in sorted(imports)]
    problems += [f"links {name}" for name in sorted(libraries)]
    return problems


def present_problems(defined, imports, libraries):
    problems = [f"XDP symbol missing: {name}" for name in REQUIRED_DEFINED if name not in defined]
    problems += [f"libbpf/libxdp reference missing: {name}" for name in REQUIRED_IMPORTS if name not in imports]
    problems += [f"does not link lib{name}.so" for name in ("bpf", "xdp") if f"lib{name}.so" not in libraries]
    return problems


def run(*command):
    return subprocess.check_output(command, text=True, errors="replace").splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--absent", action="store_true", help="the build without XDP")
    mode.add_argument("--present", action="store_true", help="the build with XDP")
    parser.add_argument("shared")
    parser.add_argument("archive")
    args = parser.parse_args()
    nm_lines = run("nm", "-C", args.archive) + run("nm", "-C", "-D", args.shared)
    needed = [line for line in run("readelf", "-d", args.shared) if "NEEDED" in line]
    defined, imports, libraries = classify(nm_lines, needed)
    problems = absent_problems(defined, imports, libraries) if args.absent else present_problems(defined, imports, libraries)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    if args.absent:
        print("XDP compiled out: no XDP transport, gateway or program symbol, no libbpf or libxdp reference or link")
    else:
        print("XDP compiled in: transport, gateway, program and libbpf/libxdp links present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
