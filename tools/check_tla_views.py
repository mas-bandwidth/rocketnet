#!/usr/bin/env python3
"""Every TLC VIEW is used only where its justification holds.

A VIEW makes TLC treat two states as one when they agree on the view. That is
sound only when the dropped variables cannot change what happens next.
RNChannel's SafetyView drops the Link send counters, which decide nothing
under the "arbitrary" loss pattern but decide which send is lost under any
other: MCRNChannelNotReceived.cfg used it with the "alternate" pattern, the
one config checking NotReceivedEndsConnection, and saw 8,630 distinct states
where the model has 93,774 (VERIFY-6 E62).

So every VIEW named in proofs/tla/*.cfg must be one this file classifies:
  SafetyView    the config sets FwdPattern and RevPattern to "arbitrary"
  MCSafetyView  its definition in MCRNEndpoint.tla falls back to `vars`
                unless ABPattern and BAPattern are both "arbitrary"
  View          a layout module's dead-value view; the config declares no
                loss pattern
A new view is refused until it is classified here with its reason.

Usage: check_tla_views.py [--self-test | --root DIR]
Prints one line per offence and a census line; exits 1 on any offence, and
when no config uses a VIEW (a moved tree is never "nothing to check").
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VIEW_LINE = re.compile(r"^\s*VIEW\s+(\S+)", re.M)
CONSTANT = re.compile(r"^\s*(\w+)\s*=\s*(\S+)\s*$", re.M)
PATTERN = re.compile(r"Pattern$")
ARBITRARY = '"arbitrary"'

# view -> the loss-pattern constants a config must set to "arbitrary"
CONFIG_BOUND = {"SafetyView": ("FwdPattern", "RevPattern")}

# view -> (module, the guard its definition must carry before the view applies)
MODULE_GUARDED = {
    "MCSafetyView": (
        "MCRNEndpoint.tla",
        re.compile(
            r'^MCSafetyView\s*==\s*\n\s*IF\s+ABPattern\s*=\s*"arbitrary"\s*/\\\s*'
            r'BAPattern\s*=\s*"arbitrary"\s*\n\s*THEN[^\n]*\n\s*ELSE\s+vars\s*$',
            re.M,
        ),
    ),
}

# dead-value views of layout modules: valid only where no loss pattern exists
PATTERN_FREE = {"View"}


def check(tla_dir):
    """Return (offences, census) for the configs under tla_dir."""
    offences = []
    counts = {"bound": 0, "guarded": 0, "pattern-free": 0}
    names = sorted(n for n in os.listdir(tla_dir) if n.endswith(".cfg"))
    for name in names:
        with open(os.path.join(tla_dir, name), encoding="utf-8") as f:
            text = f.read()
        views = VIEW_LINE.findall(text)
        if not views:
            continue
        constants = dict(CONSTANT.findall(text))
        for view in views:
            if view in CONFIG_BOUND:
                wrong = [
                    "%s=%s" % (c, constants.get(c, "unset"))
                    for c in CONFIG_BOUND[view]
                    if constants.get(c) != ARBITRARY
                ]
                if wrong:
                    offences.append(
                        "%s: VIEW %s needs every loss pattern \"arbitrary\"; has %s"
                        % (name, view, " ".join(wrong))
                    )
                else:
                    counts["bound"] += 1
            elif view in MODULE_GUARDED:
                module, guard = MODULE_GUARDED[view]
                path = os.path.join(tla_dir, module)
                body = ""
                if os.path.exists(path):
                    with open(path, encoding="utf-8") as f:
                        body = f.read()
                if not guard.search(body):
                    offences.append(
                        "%s: VIEW %s: %s no longer falls back to vars unless "
                        "both loss patterns are \"arbitrary\"" % (name, view, module)
                    )
                else:
                    counts["guarded"] += 1
            elif view in PATTERN_FREE:
                patterns = sorted(c for c in constants if PATTERN.search(c))
                if patterns:
                    offences.append(
                        "%s: VIEW %s is a dead-value view but the config sets %s"
                        % (name, view, ", ".join(patterns))
                    )
                else:
                    counts["pattern-free"] += 1
            else:
                offences.append(
                    "%s: VIEW %s is not classified in tools/check_tla_views.py"
                    % (name, view)
                )
    total = sum(counts.values()) + len(offences)
    census = (
        "check_tla_views: %d views: pattern-bound %d, module-guarded %d, "
        "pattern-free %d, refused %d"
        % (total, counts["bound"], counts["guarded"], counts["pattern-free"], len(offences))
    )
    if total == 0:
        offences.append("no config under %s uses a VIEW: nothing was checked" % tla_dir)
    return offences, census


GUARD_TEXT = (
    "MCSafetyView ==\n"
    '    IF ABPattern = "arbitrary" /\\ BAPattern = "arbitrary"\n'
    "      THEN <<epVars>>\n"
    "      ELSE vars\n"
)


def write(tla_dir, files):
    os.makedirs(tla_dir, exist_ok=True)
    for name, text in files.items():
        with open(os.path.join(tla_dir, name), "w", encoding="utf-8") as f:
            f.write(text)


def cfg(view, **constants):
    lines = ["SPECIFICATION Spec", "CONSTANTS"]
    lines += [" %s = %s" % (k, v) for k, v in constants.items()]
    if view:
        lines.append("VIEW %s" % view)
    return "\n".join(lines) + "\n"


GOOD = {
    "MCSafe.cfg": cfg("SafetyView", FwdPattern=ARBITRARY, RevPattern=ARBITRARY),
    "MCEndpoint.cfg": cfg("MCSafetyView", ABPattern='"alternate"', BAPattern=ARBITRARY),
    "MCRNEndpoint.tla": "---- MODULE MCRNEndpoint ----\n" + GUARD_TEXT + "====\n",
    "MCLayout.cfg": cfg("View", Capacity="3"),
    "MCPlain.cfg": cfg(None, FwdPattern='"alternate"'),
}

CASES = [
    ("a sound tree", {}, None),
    (
        "SafetyView with alternate patterns",
        {"MCSafe.cfg": cfg("SafetyView", FwdPattern='"alternate"', RevPattern='"alternate"')},
        'MCSafe.cfg: VIEW SafetyView needs every loss pattern "arbitrary"; '
        'has FwdPattern="alternate" RevPattern="alternate"',
    ),
    (
        "SafetyView with one pattern alternate",
        {"MCSafe.cfg": cfg("SafetyView", FwdPattern=ARBITRARY, RevPattern='"burst"')},
        'has RevPattern="burst"',
    ),
    (
        "SafetyView with a pattern unset",
        {"MCSafe.cfg": cfg("SafetyView", FwdPattern=ARBITRARY)},
        "has RevPattern=unset",
    ),
    (
        "MCSafetyView whose guard is gone",
        {"MCRNEndpoint.tla": "MCSafetyView == <<epVars>>\n"},
        "MCEndpoint.cfg: VIEW MCSafetyView: MCRNEndpoint.tla no longer falls back",
    ),
    (
        "MCSafetyView guarded on one pattern only",
        {"MCRNEndpoint.tla": GUARD_TEXT.replace(' /\\ BAPattern = "arbitrary"', "")},
        "VIEW MCSafetyView: MCRNEndpoint.tla no longer falls back",
    ),
    (
        "a layout View in a config with a loss pattern",
        {"MCLayout.cfg": cfg("View", Capacity="3", FwdPattern='"alternate"')},
        "MCLayout.cfg: VIEW View is a dead-value view but the config sets FwdPattern",
    ),
    (
        "an unclassified view",
        {"MCPlain.cfg": cfg("CheapView")},
        "MCPlain.cfg: VIEW CheapView is not classified",
    ),
    (
        "no view anywhere",
        {n: cfg(None) for n in GOOD if n.endswith(".cfg")},
        "nothing was checked",
    ),
]


def self_test():
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, (label, change, want) in enumerate(CASES):
            tla_dir = os.path.join(tmp, str(i))
            files = dict(GOOD)
            files.update(change)
            write(tla_dir, files)
            offences, _ = check(tla_dir)
            if want is None:
                ok = not offences
            else:
                ok = any(want in o for o in offences)
            if not ok:
                failed += 1
                print("self-test FAIL: %s: got %r" % (label, offences))
        # the command line, on a planted tree, as CI runs it
        root = os.path.join(tmp, "root")
        files = dict(GOOD)
        files["MCSafe.cfg"] = CASES[1][1]["MCSafe.cfg"]
        write(os.path.join(root, "proofs", "tla"), files)
        run = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "--root", root],
            capture_output=True,
            text=True,
        )
        if run.returncode != 1 or "MCSafe.cfg: VIEW SafetyView" not in run.stdout:
            failed += 1
            print("self-test FAIL: the command line passed a planted tree: %r" % run.stdout)
    if failed:
        return 1
    print("check_tla_views self-test: %d cases and the command line ok" % len(CASES))
    return 0


def main(argv):
    root = ROOT
    if argv[1:2] == ["--self-test"]:
        return self_test()
    if argv[1:2] == ["--root"] and len(argv) == 3:
        root = argv[2]
    elif len(argv) != 1:
        print("usage: check_tla_views.py [--self-test | --root DIR]")
        return 2
    offences, census = check(os.path.join(root, "proofs", "tla"))
    for o in offences:
        print(o)
    print(census)
    return 1 if offences else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
