#!/usr/bin/env python3
#
#   RocketNet. Copyright 2026 Más Bandwidth LLC.
#   Licensed under the GNU Affero General Public License v3.0
#
# The packet-open path's checks against the committed fuzz corpora.
#
# Each mutant below removes one admission or integrity check on the path a
# datagram takes from the wire to the message layer (the replay window, the
# AEAD tag, the zero sequence, the slot gate, ...). For each, this tool
# compiles the one sdk/ file with that check removed, exactly as the fuzz
# build compiled it (build-fuzz/compile_commands.json), links it ahead of the
# instrumented library into each named harness's replay driver (the replay
# target's own link line), and replays that harness's committed corpus, as
# the per-commit fuzz-smoke step does. A mutant must be KILLED: some replay
# exits nonzero. A mutant the corpora cannot kill is a missing corpus input,
# and the run fails naming it, unless the table marks it EQUIVALENT with the
# reason no input can tell it from the library (a check its callers already
# made); an EQUIVALENT mutant that some replay kills is stale and fails too.
# A mutant whose source text is gone fails: review the table.
#
#   python3 tools/check_fuzz_mutants.py [--build build-fuzz] [--only ID[,ID]] [--jobs N]
#   python3 tools/check_fuzz_mutants.py --list
#   python3 tools/check_fuzz_mutants.py --find-inputs ID --seconds S --out DIR
#       (by hand: fuzz the mutant's session harness from its corpus for S
#       seconds; every input that kills the mutant and replays clean on the
#       library is written to DIR, ready to commit as a corpus input)
#
# Needs the fuzz-smoke configure (clang, -fsanitize=fuzzer-no-link,address,
# undefined, CMAKE_EXPORT_COMPILE_COMMANDS=ON) and the replay targets built.

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# the harnesses whose inputs reach the open path: a whole session over the
# loopback (session), the server and client transports fed raw datagrams,
# and the seal/open round trip; session_raw is the session with the client
# off its I/O-thread decrypt copy, so the client protocol's own replay check
# and open refusal (the RAW verdict) are reached
OPEN_PATH_HARNESSES = ("rocketnet_fuzz_session", "rocketnet_fuzz_session_raw", "rocketnet_fuzz_server_transport",
                       "rocketnet_fuzz_client_transport", "rocketnet_fuzz_round_trip")


MUTANTS = [
    {
        "id": "replay-window-bit",
        "file": "sdk/rocketnet_replay.cpp",
        "what": "the replay window never reports a sequence inside it as already received",
        "find": "    return rocketnet_replay_bit( replay, sequence );\n}",
        "replace": "    return 0 * rocketnet_replay_bit( replay, sequence );\n}",
    },
    {
        "id": "replay-window-too-old",
        "file": "sdk/rocketnet_replay.cpp",
        "what": "a sequence older than the window is not refused",
        "find": "         sequence <= replay->highest_opened - ROCKETNET_REPLAY_WINDOW_BITS )\n    {\n        return 1;\n    }",
        "replace": "         sequence <= replay->highest_opened - ROCKETNET_REPLAY_WINDOW_BITS )\n    {\n        return 0;\n    }",
        "uncovered": ("a datagram held back while more than ROCKETNET_HISTORY_PACKETS (4096) newer ones open; the "
                      "session harness runs at most 1024 ops and its rings release or drop every update, and 180 s "
                      "of libFuzzer found no input; covered by the unit cases test_replay_protection, "
                      "test_standard_replay_vectors, test_replay_window_model_trace and "
                      "test_witness_replay_retained_depth_exact, each red with this mutant (2026-11-12, hetzner1)"),
    },
    {
        "id": "aead-tag-ignored",
        "file": "sdk/rocketnet_protocol.cpp",
        "what": "a datagram whose AEAD tag fails is accepted, its sealed bytes taken as the plaintext",
        "find": ("                                                    nonce, key ) != 0 )\n    {\n        return -1;\n    }\n\n"
                 "    return (int) opened_bytes;"),
        "replace": ("                                                    nonce, key ) != 0 )\n    {\n"
                    "        opened_bytes = (unsigned long long) ( packet_bytes - header_bytes - ROCKETNET_PACKET_TAG_BYTES );\n"
                    "        memmove( out_data, packet_data + header_bytes, (size_t) opened_bytes );\n    }\n\n"
                    "    return (int) opened_bytes;"),
    },
    {
        "id": "open-zero-sequence",
        "file": "sdk/rocketnet_protocol.cpp",
        "what": "the open accepts sequence 0",
        "find": "    if ( sequence == 0 )\n    {\n        return -1;\n    }\n\n    if ( rocketnet_packet_type_has_session_nonce",
        "replace": "    if ( sequence == 0 && 0 )\n    {\n        return -1;\n    }\n\n    if ( rocketnet_packet_type_has_session_nonce",
        "equivalent": ("both callers refuse sequence 0 before they open (server_protocol.cpp 'zero sequence', "
                       "client_protocol.cpp 'zero sequence'), so no datagram reaches the open with it"),
    },
    {
        "id": "server-zero-sequence",
        "file": "sdk/rocketnet_server_protocol.cpp",
        "what": "the server opens a sealed datagram with sequence 0",
        "find": "    if ( sequence == 0 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. zero sequence\" );",
        "replace": "    if ( sequence == 0 && 0 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. zero sequence\" );",
    },
    {
        "id": "server-replay-check",
        "file": "sdk/rocketnet_server_protocol.cpp",
        "what": "the server does not ask the replay window before it opens a session datagram",
        "find": "    if ( session_phase && rocketnet_replay_already_received( replay, sequence ) )",
        "replace": "    if ( session_phase && 0 * rocketnet_replay_already_received( replay, sequence ) )",
    },
    {
        "id": "server-slot-gate",
        "file": "sdk/rocketnet_server_protocol.cpp",
        "what": "the server opens session datagrams for a mapping with no slot",
        "find": "    if ( session_phase && mapping->control->client_index == -1 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. not connected\" );",
        "replace": "    if ( session_phase && mapping->control->client_index == -1 && 0 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. not connected\" );",
    },
    {
        "id": "server-did-not-open",
        "file": "sdk/rocketnet_server_protocol.cpp",
        "what": "the server goes on with a datagram that did not open",
        "find": "    if ( opened_bytes < 0 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. did not open\" );",
        "replace": "    if ( opened_bytes < 0 && 0 )\n    {\n        rocketnet_spam( \"server ignored sealed packet. did not open\" );",
    },
    {
        "id": "client-zero-sequence",
        "file": "sdk/rocketnet_client_protocol.cpp",
        "what": "the client opens a session datagram with sequence 0",
        "find": "        if ( sequence == 0 )\n        {\n            rocketnet_spam( \"client ignored session packet. zero sequence\" );",
        "replace": "        if ( sequence == 0 && 0 )\n        {\n            rocketnet_spam( \"client ignored session packet. zero sequence\" );",
    },
    {
        "id": "client-replay-check",
        "file": "sdk/rocketnet_client_protocol.cpp",
        "what": "the client does not ask the replay window before it opens a session datagram",
        "find": "        if ( rocketnet_replay_already_received( &client->replay_session, sequence ) )\n        {\n            rocketnet_spam( \"client ignored session packet. already received\" );\n            counters[type_counters->already_received]++;\n            return -1;\n        }\n    }",
        "replace": "        if ( 0 * rocketnet_replay_already_received( &client->replay_session, sequence ) )\n        {\n            rocketnet_spam( \"client ignored session packet. already received\" );\n            counters[type_counters->already_received]++;\n            return -1;\n        }\n    }",
    },
    {
        "id": "client-did-not-open",
        "file": "sdk/rocketnet_client_protocol.cpp",
        "what": "the client goes on with a session datagram that did not open",
        "find": "            if ( opened_bytes < 0 )\n            {\n                rocketnet_spam( \"client ignored session packet. did not open\" );",
        "replace": "            if ( opened_bytes < 0 && 0 )\n            {\n                rocketnet_spam( \"client ignored session packet. did not open\" );",
    },
]


def fail(message):
    print("check_fuzz_mutants: " + message, file=sys.stderr)
    sys.exit(1)


def compile_command(build, source):
    """The fuzz build's compile line for source into the development library, as an argv."""
    database = build / "compile_commands.json"
    if not database.is_file():
        fail("%s is missing: configure the fuzz build with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" % database)
    entries = json.loads(database.read_text())
    wanted = str((ROOT / source).resolve())
    for entry in entries:
        if str(Path(entry["directory"], entry["file"]).resolve()) != wanted:
            continue
        argv = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
        if any("rocketnet_dev.dir" in a for a in argv):
            return entry["directory"], argv
    fail("no compile line for %s into rocketnet_dev in %s" % (source, database))


def link_command(build, harness):
    """The replay driver's link line, as an argv run from the build directory."""
    link = build / "CMakeFiles" / ("fuzz_replay_" + harness + ".dir") / "link.txt"
    if not link.is_file():
        fail("%s is missing: build the replay targets first" % link)
    return shlex.split(link.read_text().strip())


def mutated_object(build, mutant, work):
    text = (ROOT / mutant["file"]).read_text()
    count = text.count(mutant["find"])
    if count != 1:
        fail("mutant %s: its source text occurs %d times in %s (want 1): review the table" % (mutant["id"], count, mutant["file"]))
    source = work / (mutant["id"] + "-" + Path(mutant["file"]).name)
    source.write_text(text.replace(mutant["find"], mutant["replace"]))
    directory, argv = compile_command(build, mutant["file"])
    out = work / (mutant["id"] + ".o")
    wanted = str((ROOT / mutant["file"]).resolve())
    new = []
    skip = False
    for i, arg in enumerate(argv):
        if skip:
            skip = False
            continue
        if arg == "-o":
            new += ["-o", str(out)]
            skip = True
        elif arg == "-c":
            continue
        elif str(Path(directory, arg).resolve()) == wanted:
            continue
        else:
            new.append(arg)
    # quoted includes resolve beside the original file
    new += ["-iquote", str(ROOT / Path(mutant["file"]).parent), "-c", str(source)]
    result = subprocess.run(new, cwd=directory, capture_output=True, text=True)
    if result.returncode != 0:
        fail("mutant %s does not compile:\n%s" % (mutant["id"], result.stderr[-4000:]))
    return out


def linked(build, harness, obj, out):
    argv = link_command(build, harness)
    new = []
    skip = False
    placed = False
    for arg in argv:
        if skip:
            skip = False
            continue
        if arg == "-o":
            new += ["-o", str(out)]
            skip = True
            continue
        if not placed and arg.endswith("librocketnet_dev.a"):
            new.append(str(obj))
            placed = True
        new.append(arg)
    if not placed:
        fail("the link line of fuzz_replay_%s names no librocketnet_dev.a" % harness)
    result = subprocess.run(new, cwd=build, capture_output=True, text=True)
    if result.returncode != 0:
        fail("mutant link for %s failed:\n%s" % (harness, result.stderr[-4000:]))
    return out


def corpus(harness):
    directory = ROOT / "tests" / "fuzz" / ("corpus" + harness[len("rocketnet_fuzz"):])
    files = sorted(p for p in directory.iterdir() if p.is_file())
    if not files:
        fail("corpus %s is empty" % directory)
    return files


def replay(binary, files):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
    result = subprocess.run([str(binary)] + [str(f) for f in files], env=env, capture_output=True, text=True, timeout=600)
    reason = ""
    for line in (result.stderr + result.stdout).splitlines():
        if "fuzz oracle failed" in line or "ERROR: AddressSanitizer" in line or "runtime error:" in line or "check failed" in line:
            reason = line.strip()
            break
    if not reason and result.returncode != 0:
        lines = [l.strip() for l in (result.stderr + result.stdout).splitlines() if l.strip()]
        reason = "(no oracle line; last output: %s)" % (lines[-1][:200] if lines else "none")
    return result.returncode, reason


def judge(build, mutant, work):
    obj = mutated_object(build, mutant, work)
    kills = []
    for harness in OPEN_PATH_HARNESSES:
        binary = linked(build, harness, obj, work / ("%s-%s" % (mutant["id"], harness)))
        rc, reason = replay(binary, corpus(harness))
        if rc != 0:
            kills.append("%s rc %d: %s" % (harness, rc, reason or "(no oracle line)"))
    return kills


def run_all(build, only, jobs):
    chosen = [m for m in MUTANTS if not only or m["id"] in only]
    if only and len(chosen) != len(only):
        fail("unknown mutant in --only: %s" % ", ".join(sorted(set(only) - {m["id"] for m in MUTANTS})))
    if not chosen:
        fail("no mutant to judge")
    problems = 0
    with tempfile.TemporaryDirectory(prefix="fuzz-mutants-", dir=build) as directory:
        work = Path(directory)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {m["id"]: pool.submit(judge, build, m, work) for m in chosen}
            for mutant in chosen:
                kills = futures[mutant["id"]].result()
                equivalent = mutant.get("equivalent")
                uncovered = mutant.get("uncovered")
                if kills and uncovered:
                    print("STALE-UNCOVERED %s: marked out of the corpora's reach, but %s; drop the mark" %
                          (mutant["id"], kills[0]))
                    problems += 1
                elif uncovered:
                    print("UNCOVERED BY FUZZ %s: %s (%s)" % (mutant["id"], mutant["what"], uncovered))
                elif kills and not equivalent:
                    print("KILLED %s by %s" % (mutant["id"], kills[0]))
                elif not kills and equivalent:
                    print("EQUIVALENT %s (%s)" % (mutant["id"], equivalent))
                elif kills and equivalent:
                    print("STALE-EQUIVALENT %s: marked equivalent, but %s" % (mutant["id"], kills[0]))
                    problems += 1
                else:
                    print("SURVIVED %s: %s; no committed input of %s tells it from the library" %
                          (mutant["id"], mutant["what"], ", ".join(OPEN_PATH_HARNESSES)))
                    problems += 1
    if problems:
        print("check_fuzz_mutants: FAILED, %d of %d mutants" % (problems, len(chosen)))
        return 1
    print("check_fuzz_mutants: ok, %d mutants judged" % len(chosen))
    return 0


def find_inputs(build, mutant_id, seconds, out_dir):
    mutant = next((m for m in MUTANTS if m["id"] == mutant_id), None)
    if mutant is None:
        fail("unknown mutant %s" % mutant_id)
    out_dir = Path(out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    harness = "rocketnet_fuzz_session"
    with tempfile.TemporaryDirectory(prefix="fuzz-mutant-find-", dir=build) as directory:
        work = Path(directory)
        obj = mutated_object(build, mutant, work)
        clean = build.parent / "bin" / ("fuzz_replay_" + harness)
        if not clean.is_file():
            fail("%s is missing" % clean)
        # the fuzzing binary: the replay link line with libFuzzer's main
        # instead of the replay driver's
        argv = link_command(build, harness)
        fuzz_argv = []
        skip = False
        for arg in argv:
            if skip:
                skip = False
                continue
            if arg == "-o":
                fuzz_argv += ["-o", str(work / "fuzzer")]
                skip = True
                continue
            if "replay_main.cpp" in arg:
                continue
            if arg.endswith("librocketnet_dev.a"):
                fuzz_argv.append(str(obj))
            fuzz_argv.append(arg)
        fuzz_argv.append("-fsanitize=fuzzer")
        result = subprocess.run(fuzz_argv, cwd=build, capture_output=True, text=True)
        if result.returncode != 0:
            fail("fuzzer link failed:\n" + result.stderr[-4000:])
        seeds = work / "seeds"
        seeds.mkdir()
        for f in corpus(harness):
            (seeds / f.name).write_bytes(f.read_bytes())
        artifacts = work / "artifacts"
        artifacts.mkdir()
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        subprocess.run([str(work / "fuzzer"), "-max_total_time=%d" % seconds, "-artifact_prefix=%s/" % artifacts,
                        "-fork=4", "-ignore_crashes=1", str(seeds)], env=env, capture_output=True, text=True)
        found = 0
        for artifact in sorted(artifacts.iterdir()):
            rc, _ = replay(clean, [artifact])
            if rc != 0:
                continue
            target = out_dir / ("mutant_%s_%s" % (mutant_id.replace("-", "_"), artifact.name.split("-")[-1][:12]))
            target.write_bytes(artifact.read_bytes())
            found += 1
            print("input %s (%d bytes): kills %s, replays clean on the library" % (target, artifact.stat().st_size, mutant_id))
        print("find-inputs %s: %d inputs" % (mutant_id, found))
        return 0 if found else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build-fuzz")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--find-inputs", default="")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    if args.list:
        for m in MUTANTS:
            print("%-24s %-36s %s%s" % (m["id"], m["file"], m["what"], " (EQUIVALENT)" if m.get("equivalent") else (" (UNCOVERED)" if m.get("uncovered") else "")))
        return 0
    build = (ROOT / args.build).resolve() if not os.path.isabs(args.build) else Path(args.build)
    if args.find_inputs:
        if not args.out:
            fail("--find-inputs needs --out DIR")
        return find_inputs(build, args.find_inputs, args.seconds, args.out)
    only = [x for x in args.only.split(",") if x]
    return run_all(build, only, args.jobs)


if __name__ == "__main__":
    sys.exit(main())
