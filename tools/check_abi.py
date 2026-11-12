#!/usr/bin/env python3
"""Check the 1.x C ABI against its recorded baseline, tools/abi_v1.txt.

The owner's rule: work on the library must not change compatibility or the
ABI. So the baseline records every way sdk/rocketnet.h can change it:

  export <name>                      every symbol in tools/exports.txt
  prototype <name> <declaration>     every function declared in the header
                                     (exported or static inline): return
                                     type, name, parameter types in order,
                                     with parameter names removed
  typedef <name> <declaration>       every typedef (callback signatures)
  struct <name> <size>               every public struct's size
  field <struct> <field> <off> <size>   each field's offset and size
  fieldtype <struct> <field> <type>  each field's declared type
  enum <enum> <enumerator> <value>   every enumerator's value, as compiled
  macro <name> <value>               every macro the header defines: an
                                     integer one as the compiler evaluates it,
                                     a string or initializer one as text

on a 64-bit target with the default build (ROCKETNET_DEV 1, the default
ROCKETNET_MAX_CLIENTS and ROCKETNET_MAX_CHANNELS). The check compiles a small
C program against the current header that prints the compiled lines, reads
the declarations from the header text, and fails when a recorded line is
missing or different (ABI BREAK) or when a line is not recorded (NOT
RECORDED: a 1.x addition is written into the baseline in the change that adds
it, so it is reviewed). Within 1.x the baseline only grows.

Macros not recorded, with the reason: ROCKETNET_H (the include guard),
ROCKETNET_VERSION, ROCKETNET_VERSION_FULL, _MAJOR, _MINOR, _PATCH (change at
every release by design; tools/release_version.py and release-check.yml hold
them), ROCKETNET_EXPORT (the platform's visibility attribute, not a value),
ROCKETNET_DEV (the build mode a consumer selects).

  python3 tools/check_abi.py                 check the current header
  python3 tools/check_abi.py --record OUT    write the lines for --header
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "sdk" / "rocketnet.h"
EXPORTS = ROOT / "tools" / "exports.txt"
BASELINE = ROOT / "tools" / "abi_v1.txt"

MACROS_NOT_RECORDED = {"ROCKETNET_H", "ROCKETNET_VERSION", "ROCKETNET_VERSION_FULL", "ROCKETNET_VERSION_MAJOR",
                       "ROCKETNET_VERSION_MINOR", "ROCKETNET_VERSION_PATCH", "ROCKETNET_EXPORT", "ROCKETNET_DEV"}
TYPE_WORDS = {"void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "_Bool", "bool",
              "const", "volatile", "struct", "enum", "union", "restrict", "*"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def collapse(text):
    text = " ".join(text.split())
    text = re.sub(r"\s*([(),\[\]*])\s*", r"\1", text)
    return text.replace(",", ", ")


def joined_lines(header_text):
    """The header without comments, continuation lines joined."""
    return strip_comments(header_text).replace("\\\n", " ").splitlines()


def public_structs(header_text):
    """[(struct, [(field, type), ...])] for every struct defined at file scope."""
    text = strip_comments(header_text)
    structs = []
    for match in re.finditer(r"^struct\s+(rocketnet_\w+_t)\s*\{(.*?)^\};", text, flags=re.S | re.M):
        fields = []
        for declaration in match.group(2).split(";"):
            lines = [line for line in declaration.splitlines() if not line.strip().startswith("#")]
            declaration = " ".join(lines).strip()
            if not declaration:
                continue
            pointer = re.search(r"\(\s*\*\s*(\w+)\s*\)\s*\(", declaration)
            name = pointer or re.search(r"(\w+)\s*(?:\[[^\]]*\]\s*)*$", declaration)
            if not name:
                raise RuntimeError("cannot read a field of %s: %s" % (match.group(1), declaration))
            field = name.group(1)
            if pointer:
                kind = declaration[:pointer.start(1)] + declaration[pointer.end(1):]
            else:
                kind = declaration[:name.start(1)] + declaration[name.end(1):]
            fields.append((field, collapse(kind)))
        structs.append((match.group(1), fields))
    return structs


def macros(header_text):
    """[(name, kind, value)]: kind is 'int', 'string' or 'text'."""
    found = []
    for line in joined_lines(header_text):
        match = re.match(r"^\s*#\s*define\s+(\w+)(\([^)]*\))?\s*(.*)$", line)
        if not match or match.group(1) in MACROS_NOT_RECORDED:
            continue
        name, arguments, value = match.group(1), match.group(2), " ".join(match.group(3).split())
        if arguments is not None:
            found.append((name, "text", collapse(arguments) + " " + value))
        elif value.startswith('"'):
            found.append((name, "string", value))
        elif value == "" or value.startswith("{"):
            found.append((name, "text", value or "<empty>"))
        else:
            found.append((name, "int", value))
    return found


def statements(header_text):
    """File-scope declarations, preprocessor lines removed, extern "C" opened,
    function bodies dropped: each a whitespace-collapsed string."""
    lines = [line for line in joined_lines(header_text) if not line.strip().startswith("#")]
    text = " ".join(lines)
    text = re.sub(r'extern\s+"C"\s*\{', " ", text)
    out, current, depth, index = [], [], 0, 0
    while index < len(text):
        char = text[index]
        if char == "{":
            prefix = "".join(current).strip()
            if depth == 0 and prefix.endswith(")"):
                # a function definition: keep the signature, skip the body
                out.append(prefix + ";")
                current, level = [], 0
                while index < len(text):
                    if text[index] == "{":
                        level += 1
                    elif text[index] == "}":
                        level -= 1
                        if level == 0:
                            break
                    index += 1
                index += 1
                continue
            depth += 1
        elif char == "}":
            if depth == 0:
                # the close of extern "C"
                index += 1
                continue
            depth -= 1
        current.append(char)
        if char == ";" and depth == 0:
            out.append(" ".join("".join(current).split()))
            current = []
        index += 1
    return out


def strip_parameter_names(parameters):
    """'const char * name, int (*f)(int x), uint8_t k[32]' -> types only."""
    if parameters.strip() in ("", "void"):
        return parameters.strip()
    pieces, depth, current = [], 0, ""
    for char in parameters:
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        if char == "," and depth == 0:
            pieces.append(current)
            current = ""
        else:
            current += char
    pieces.append(current)
    kept = []
    for piece in pieces:
        piece = piece.strip()
        pointer = re.search(r"\(\s*\*\s*(\w+)\s*\)", piece)
        if pointer:
            piece = piece[:pointer.start(1)] + piece[pointer.end(1):]
        else:
            array = re.match(r"^(.*?)(\w+)(\s*\[[^\]]*\]\s*)$", piece)
            if array and array.group(1).strip():
                piece = array.group(1) + array.group(3)
            else:
                tokens = re.findall(r"\w+|\*|\.\.\.", piece)
                if len(tokens) >= 2 and tokens[-1] not in TYPE_WORDS and tokens[-2] not in ("struct", "enum", "union"):
                    piece = piece[:piece.rfind(tokens[-1])]
        kept.append(collapse(piece))
    return ", ".join(kept)


def declarations(header_text):
    """(prototypes, typedefs, enums): prototypes and typedefs as {name: {text}},
    enums as [(enum, [enumerator, ...])]."""
    prototypes, typedefs, enums = {}, {}, []
    for statement in statements(header_text):
        enum = re.match(r"^enum\s+(\w+)\s*(?::\s*\w+\s*)?(?:enum\s+\1\s*)?\{(.*)\}\s*;$", statement)
        if enum:
            names = [re.match(r"\s*(\w+)", part).group(1) for part in enum.group(2).split(",") if part.strip()]
            enums.append((enum.group(1), names))
            continue
        if statement.startswith("struct ") and "{" in statement:
            continue
        if statement.startswith("typedef "):
            pointer = re.match(r"^typedef\s+(.*?)\(\s*\*\s*(\w+)\s*\)\s*\((.*)\)\s*;$", statement)
            if pointer:
                name = pointer.group(2)
                text = "typedef %s(*%s)(%s)" % (pointer.group(1), name, strip_parameter_names(pointer.group(3)))
            else:
                name = re.findall(r"(\w+)\s*;$", statement)[0]
                text = statement.rstrip(";")
            typedefs.setdefault(name, set()).add(collapse(text))
            continue
        function = re.match(r"^(.*?)\b(rocketnet_\w+)\s*\((.*)\)\s*;$", statement)
        if function and "(" not in function.group(1):
            returns = collapse(function.group(1).replace("ROCKETNET_EXPORT", " "))
            text = "%s %s(%s)" % (returns, function.group(2), strip_parameter_names(function.group(3)))
            prototypes.setdefault(function.group(2), set()).add(collapse(text))
    return prototypes, typedefs, enums


def layout_program(structs, enums, integer_macros, string_macros):
    out = ["#include <stddef.h>", "#include <stdio.h>", '#include "rocketnet.h"', "int main( void )", "{",
           '    printf( "pointer %zu\\n", sizeof( void * ) );']
    for struct, fields in structs:
        out.append('    printf( "struct %s %%zu\\n", sizeof( struct %s ) );' % (struct, struct))
        for field, _ in fields:
            out.append('    printf( "field %s %s %%zu %%zu\\n", offsetof( struct %s, %s ), sizeof( ( (struct %s *) 0 )->%s ) );'
                       % (struct, field, struct, field, struct, field))
    for enum, names in enums:
        for name in names:
            out.append('    printf( "enum %s %s %%lld\\n", (long long) %s );' % (enum, name, name))
    for name in integer_macros:
        out += ["#ifdef %s" % name,
                '    printf( "macro %s %%llu\\n", (unsigned long long) ( %s ) );' % (name, name),
                "#endif"]
    for name in string_macros:
        out += ["#ifdef %s" % name, '    printf( "macro %s \\"%%s\\"\\n", %s );' % (name, name), "#endif"]
    out += ["    return 0;", "}", ""]
    return "\n".join(out)


def read_exports(path):
    return sorted(line.strip() for line in Path(path).read_text().splitlines()
                  if line.strip() and not line.startswith("#"))


def measured_lines(header, exports, compiler):
    header_text = Path(header).read_text()
    structs = public_structs(header_text)
    prototypes, typedefs, enums = declarations(header_text)
    found_macros = macros(header_text)
    integer_macros = sorted({name for name, kind, _ in found_macros if kind == "int"})
    string_macros = sorted({name for name, kind, _ in found_macros if kind == "string"})
    enum_lines = {}
    for enum, names in enums:
        enum_lines.setdefault(enum, [])
        for name in names:
            if name not in enum_lines[enum]:
                enum_lines[enum].append(name)
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "abi.c"
        binary = Path(tmp) / "abi"
        source.write_text(layout_program(structs, sorted(enum_lines.items()), integer_macros, string_macros))
        subprocess.run([compiler, "-std=c11", "-ffp-contract=off", "-I", str(Path(header).parent), "-o", str(binary), str(source)],
                       check=True)
        compiled = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.splitlines()
    lines = ["export %s" % name for name in read_exports(exports)]
    for name in sorted(prototypes):
        lines += ["prototype %s %s" % (name, text) for text in sorted(prototypes[name])]
    for name in sorted(typedefs):
        lines += ["typedef %s %s" % (name, text) for text in sorted(typedefs[name])]
    for struct, fields in structs:
        lines += ["fieldtype %s %s %s" % (struct, field, kind) for field, kind in fields]
    lines += sorted({"macro %s %s" % (name, value) for name, kind, value in found_macros if kind == "text"})
    lines += compiled
    # what the readers could not read is never a silent pass: every enum and
    # struct the header defines must have been read, and every exported
    # symbol must have a prototype in the header
    plain = strip_comments(header_text)
    for name in sorted(set(re.findall(r"^\s*enum\s+(rocketnet_\w+)\s*(?::\s*\w+\s*)?(?=\{|$)", plain, flags=re.M))
                       - set(enum_lines)):
        lines.append("enum %s <not read from %s>" % (name, Path(header).name))
    for name in sorted(set(re.findall(r"^\s*struct\s+(rocketnet_\w+)\s*\{", plain, flags=re.M))
                       - {struct for struct, _ in structs}):
        lines.append("struct %s <not read from %s>" % (name, Path(header).name))
    for name in read_exports(exports):
        if name not in prototypes:
            lines.append("prototype %s <not found in %s>" % (name, Path(header).name))
    return lines


def compare(baseline, current):
    """Returns the problems: recorded lines missing or changed, and lines not recorded."""
    recorded, now = set(baseline), set(current)
    problems = ["ABI BREAK: %s (recorded in %s)" % (line, BASELINE.name) for line in baseline if line not in now]
    problems += ["NOT RECORDED: %s (a 1.x addition is added to %s)" % (line, BASELINE.name)
                 for line in current if line not in recorded]
    return problems


def read_baseline(path):
    return [line.strip() for line in Path(path).read_text().splitlines()
            if line.strip() and not line.startswith("#")]


def counts(lines):
    kinds = {}
    for line in lines:
        kind = line.split(" ", 1)[0]
        kinds[kind] = kinds.get(kind, 0) + 1
    return ", ".join("%d %s" % (kinds[kind], kind) for kind in sorted(kinds))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", default=str(HEADER))
    parser.add_argument("--exports", default=str(EXPORTS))
    parser.add_argument("--baseline", default=str(BASELINE))
    parser.add_argument("--record", metavar="OUT", help="write the measured lines to OUT instead of checking")
    args = parser.parse_args()
    current = measured_lines(args.header, args.exports, os.environ.get("CC", "cc"))
    if args.record:
        Path(args.record).write_text(
            "# The 1.x C ABI of sdk/rocketnet.h on 64-bit targets, default build: exports,\n"
            "# prototypes, typedefs, struct sizes, field offsets, sizes and types, enumerator\n"
            "# values and macro values. Checked by tools/check_abi.py; within 1.x lines are\n"
            "# only added.\n" + "\n".join(current) + "\n")
        return 0
    problems = compare(read_baseline(args.baseline), current)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    if not any(line.startswith("prototype ") for line in current) or not any(line.startswith("enum ") for line in current):
        print("check_abi: no prototype or no enumerator read from %s: the reader failed" % args.header, file=sys.stderr)
        return 1
    print("ABI matches %s: %s" % (Path(args.baseline).name, counts(current)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
