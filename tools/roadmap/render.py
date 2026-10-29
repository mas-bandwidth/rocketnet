#!/usr/bin/env python3
# RocketNet. Copyright 2026 Mas Bandwidth LLC.
#
# Renders docs/ROADMAP.md from docs/roadmap.sexp, and docs/TESTING.md from
# docs/testing.sexp: one section per area, a table of features with a mark per
# column, and a closing "complete" row.
#
#   python3 tools/roadmap/render.py docs/roadmap.sexp docs/ROADMAP.md          write the page
#   python3 tools/roadmap/render.py --check docs/roadmap.sexp docs/ROADMAP.md  exit 1 if it differs
#
# The page names its data file as given on the command line, and the make
# target is that file's name without .sexp (make roadmap, make testing).
#
# An area declares :columns transports (a column per server transport, in the
# order the automatic ladder ranks them in sdk/rocketnet_drivers.inc) or
# :columns platforms (Linux, macOS, Windows); a list naming one whole set, in
# any order, is the same declaration with its columns in that order. Any other
# column is refused. :label names the first column (default "feature").
# :columns release is the one column "status", for a test that is neither per
# platform nor per transport (a model check, a review): rendered test | status.
#
# In a platforms area a cell may instead name transports: a list of
# (transport mark) pairs, each transport one that builds on that column's
# platform (PLATFORM_TRANSPORTS), shown as "portable ✅, iocp ❌". Each pair
# counts once in the complete row, and a y pair needs evidence for the column.
#
# :pending is a list of (column "text") pairs, each on a column whose cell has
# an n: what stands behind the ❌ (a card, or "no card"). The page lists them
# under the table.
#
# An area may carry :milestone "next": its heading is marked (next milestone).
# A cell is y (done and tested), n (red, or not yet), o (owed: a run deferred
# until the final deliverable), p (planned: a test with no green run yet that
# is not a known red and not deferred), both shown as "planned for the final
# deliverable", or
# - (does not apply). Every y
# cell needs evidence: :evidence is one string whose labels name each column
# it covers ("Linux, macOS and Windows: ..." at the start or after "; "), or a
# list of (column "evidence") pairs. A column named only in passing is not
# covered. The renderer refuses the
# data otherwise, naming the area and the row. Evidence stays in the data,
# not on the page.
#
# The complete row gives, per column, round(100 * y / (y + n + o)) followed
# by %; - cells are left out, and a column that is - throughout shows —.
#
# An o or p cell needs a :pending entry naming the card that owns it, and a
# page with any o cell carries :owed,
# the note printed under each table that has one ("Planned for the final
# deliverable: ...").
#
# A page may carry :all-cells-apply t: then no cell is -, so no table shows —.
#
# A page may carry :rows-name-source t: then every row carries :source, one
# string naming the file or the card that owns the test; it stays in the data.
#
# A page may carry :tables-only t: then the page is its headers and tables and
# nothing else. Under each area's heading is one line of links, to the data
# file and to this renderer; the page's and the areas' :text, the :pending
# lists and the :owed note stay in the data and are not printed.
#
# An area with :specific t holds tests that apply to only part of its column
# set: one row per (test, column), (:feature "f" :on Windows :cell y
# :evidence "Windows: ..." :pending "text"), rendered as test | platform (or
# transport) | status. A test is in a specific area or in a full area of the
# same set, never both, and a test whose specific rows cover the whole set
# belongs in the full area; the renderer refuses either. Its complete row
# counts its own rows.

import re
import sys

PLANNED = "planned for the final deliverable"
CELL = { "y": "✅", "n": "❌", "o": PLANNED, "p": PLANNED, "-": "—" }
OPEN = ( "n", "o", "p" )

# The server transports in the order the automatic ladder ranks them (server
# preference in sdk/rocketnet_drivers.inc, lower first; ties in registry
# order; loopback is pin-only and last). test_render.py holds this list to the
# registry.
TRANSPORTS = [ "xdp", "msgx", "gso", "io_uring", "portable", "iocp", "rio", "loopback" ]
PLATFORMS = [ "Linux", "macOS", "Windows" ]
# The transports each platform builds (the platforms column of
# sdk/rocketnet_drivers.inc); test_render.py holds this to the registry.
PLATFORM_TRANSPORTS = {
    "Linux": [ "xdp", "gso", "io_uring", "portable", "loopback" ],
    "macOS": [ "msgx", "portable", "loopback" ],
    "Windows": [ "portable", "iocp", "rio", "loopback" ],
}
RELEASE = [ "status" ]
COLUMN_SETS = { "transports": TRANSPORTS, "platforms": PLATFORMS, "release": RELEASE }
SET_NOUN = { "transports": "transport", "platforms": "platform", "release": "status" }

def tokenize( text ):
    i, n = 0, len( text )
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c in "()":
            yield c
            i += 1
        elif c == '"':
            i += 1
            out = []
            while True:
                if i >= n:
                    raise ValueError( "unterminated string" )
                if text[i] == "\\":
                    out.append( text[i+1] )
                    i += 2
                elif text[i] == '"':
                    i += 1
                    break
                else:
                    out.append( text[i] )
                    i += 1
            yield ( "str", "".join( out ) )
        else:
            j = i
            while j < n and text[j] not in " \t\r\n()\";":
                j += 1
            yield ( "sym", text[i:j] )
            i = j

def parse( text ):
    stack = [[]]
    for tok in tokenize( text ):
        if tok == "(":
            stack.append( [] )
        elif tok == ")":
            if len( stack ) < 2:
                raise ValueError( "unbalanced )" )
            done = stack.pop()
            stack[-1].append( done )
        else:
            stack[-1].append( tok[1] if tok[0] == "str" else ( "sym", tok[1] ) )
    if len( stack ) != 1 or len( stack[0] ) != 1:
        raise ValueError( "expected exactly one top-level form" )
    return stack[0][0]

def sym( x ):
    return x[1] if isinstance( x, tuple ) else x

def plist( form, where ):
    if not isinstance( form, list ) or len( form ) % 2:
        raise ValueError( "%s: expected a property list" % where )
    out = {}
    for k, v in zip( form[0::2], form[1::2] ):
        key = sym( k )
        if not isinstance( k, tuple ) or not key.startswith( ":" ):
            raise ValueError( "%s: expected a keyword, got %r" % ( where, k ) )
        out[key[1:]] = v
    return out

def text_list( v ):
    return [ v ] if isinstance( v, str ) else list( v or [] )

def columns_of( area, aid ):
    declared = area.get( "columns" )
    if isinstance( declared, ( tuple, str ) ):
        name = sym( declared )
        if name in COLUMN_SETS:
            return COLUMN_SETS[name]
        raise ValueError( "area %s: columns %r is neither transports, platforms nor release" % ( aid, name ) )
    names = [ sym( c ) for c in ( declared or [] ) ]
    for c in names:
        if c not in TRANSPORTS and c not in PLATFORMS:
            raise ValueError( "area %s: column %r is outside the transport and platform sets" % ( aid, c ) )
    for full in ( TRANSPORTS, PLATFORMS ):
        if len( names ) == len( full ) and set( names ) == set( full ):
            return names
    raise ValueError( "area %s: columns %r are not the whole transport set or the whole platform set" % ( aid, names ) )

def labelled_columns( text ):
    # The columns a string names in its labels: "Linux, macOS and Windows: ..."
    # at the start, or after a "; ", each label ending at its colon.
    named = set()
    for m in re.finditer( r"(?:^|;)\s*([A-Za-z_]+(?:(?:,\s*|\s+and\s+)[A-Za-z_]+)*):", text ):
        named.update( re.split( r",\s*|\s+and\s+", m.group( 1 ) ) )
    return named

def evidence_by_column( row, columns, where ):
    evidence = row.get( "evidence" )
    if evidence is None:
        return {}
    if isinstance( evidence, str ):
        named = labelled_columns( evidence )
        return { c: evidence for c in columns if c in named }
    out = {}
    for pair in evidence:
        if not isinstance( pair, list ) or len( pair ) != 2 or not isinstance( pair[1], str ):
            raise ValueError( "%s: evidence entries are (column \"evidence\") pairs" % where )
        column = sym( pair[0] )
        if column not in columns:
            raise ValueError( "%s: evidence for %r, which is not a column of this area" % ( where, column ) )
        if pair[1].strip():
            out[column] = pair[1]
    return out

def transport_cell( cell, column, where ):
    # A list of (transport mark) pairs in a platforms column: [(name, mark)].
    if column not in PLATFORM_TRANSPORTS:
        raise ValueError( "%s: a transport cell %r is only for a platform column, not %s" % ( where, cell, column ) )
    pairs = []
    for pair in cell:
        if not isinstance( pair, list ) or len( pair ) != 2:
            raise ValueError( "%s: a transport cell holds (transport mark) pairs, got %r" % ( where, pair ) )
        name, mark = sym( pair[0] ), sym( pair[1] )
        if name not in PLATFORM_TRANSPORTS[column]:
            raise ValueError( "%s: transport %r does not build on %s" % ( where, name, column ) )
        if mark not in ( "y", ) + OPEN:
            raise ValueError( "%s: transport %s mark %r is not y, n, o or p" % ( where, name, mark ) )
        if name in [ p[0] for p in pairs ]:
            raise ValueError( "%s: transport %s named twice in the %s cell" % ( where, name, column ) )
        pairs.append( ( name, mark ) )
    if not pairs:
        raise ValueError( "%s: an empty transport cell in %s" % ( where, column ) )
    return pairs

def set_of( columns ):
    if columns == RELEASE:
        return "release"
    return "transports" if columns[0] in TRANSPORTS else "platforms"

def render( data, source="docs/roadmap.sexp" ):
    top = plist( data, "roadmap" )
    source_link = source.split( "/" )[-1]  # generated pages live beside their data in docs/
    target = re.sub( r"\.sexp$", "", source.split( "/" )[-1] )
    all_apply = sym( top.get( "all-cells-apply" ) ) == "t"
    tables_only = sym( top.get( "tables-only" ) ) == "t"
    name_source = sym( top.get( "rows-name-source" ) ) == "t"
    owed_note = top.get( "owed" )
    lines = [ "# " + top["title"], "",
              "<!-- Generated from %s by tools/roadmap/render.py. Do not edit this file: edit the data and run `make %s`. -->" % ( source, target ), "" ]
    # Optional page-level orientation remains visible even in tables-only mode.
    # Historical tables need their release scope stated before the first row.
    for para in text_list( top.get( "intro" ) ):
        lines += [ para, "" ]
    for para in ( [] if tables_only else text_list( top.get( "text" ) ) ):
        lines += [ para, "" ]
    seen = set()
    # the tests of each column set: in a full area, and per column in a specific one
    full_tests = { name: {} for name in COLUMN_SETS }
    specific_tests = { name: {} for name in COLUMN_SETS }
    for a in top["areas"]:
        area = plist( a, "area" )
        aid = area["id"]
        if aid in seen:
            raise ValueError( "area %s: duplicate id" % aid )
        seen.add( aid )
        columns = columns_of( area, aid )
        milestone = area.get( "milestone" )
        if milestone not in ( None, "next" ):
            raise ValueError( "area %s: milestone %r is not \"next\"" % ( aid, milestone ) )
        heading = area["title"] + ( " (next milestone)" if milestone == "next" else "" )
        lines += [ "## " + heading, "" ]
        if tables_only:
            lines += [ "Source: [%s](%s), [tools/roadmap/render.py](../tools/roadmap/render.py)" % ( source, source_link ), "" ]
        for para in ( [] if tables_only else text_list( area.get( "text" ) ) ):
            lines += [ para, "" ]
        rows = area["rows"]
        if not rows:
            raise ValueError( "area %s: no rows" % aid )
        if name_source:
            for r in rows:
                row = plist( r, "area %s row" % aid )
                source_of = row.get( "source" )
                if not isinstance( source_of, str ) or not source_of.strip():
                    raise ValueError( "area %s row %r: no :source naming the file or card that owns it" % ( aid, row.get( "feature" ) ) )
        if sym( area.get( "specific" ) ) == "t":
            table, pending, owed = specific_table( area, aid, columns, specific_tests[set_of( columns )] )
        else:
            table, pending, owed = full_table( area, aid, columns, all_apply, full_tests[set_of( columns )] )
        lines += table
        if owed and ( not isinstance( owed_note, str ) or not owed_note.strip() ):
            raise ValueError( "area %s: an o cell, and the page has no :owed note" % aid )
        if tables_only:
            lines += [ "" ]
            continue
        if pending:
            lines += [ "" ] + pending
        if owed:
            lines += [ "", "Planned for the final deliverable: " + owed_note ]
        lines += [ "", "[Source data](%s)" % source_link, "" ]
    for name in full_tests:
        for feature, aid in full_tests[name].items():
            if feature in specific_tests[name]:
                raise ValueError( "area %s row %r: the test is also in a specific %s table" % ( aid, feature, SET_NOUN[name] ) )
        for feature, on in specific_tests[name].items():
            if set( on ) == set( COLUMN_SETS[name] ):
                raise ValueError( "row %r: its specific rows cover every %s; it belongs in the full table" % ( feature, SET_NOUN[name] ) )
    return "\n".join( lines )

def full_table( area, aid, columns, all_apply, tests ):
    lines = [ "| " + area.get( "label", "feature" ) + " | " + " | ".join( columns ) + " |" ]
    lines.append( "|---|" + "|".join( ":---:" for _ in columns ) + "|" )
    pending = []
    owed = False
    done = [ 0 ] * len( columns )
    total = [ 0 ] * len( columns )
    for r in area["rows"]:
        row = plist( r, "area %s row" % aid )
        for key in ( "feature", "cells" ):
            if key not in row:
                raise ValueError( "area %s row %r: no :%s" % ( aid, row.get( "feature" ), key ) )
        feature = row["feature"]
        where = "area %s row %r" % ( aid, feature )
        if feature in tests:
            raise ValueError( "%s: the test is named twice" % where )
        tests[feature] = aid
        if len( row["cells"] ) != len( columns ):
            raise ValueError( "area %s row %r: %d cells for %d columns" % ( aid, feature, len( row["cells"] ), len( columns ) ) )
        # each cell as its marks: [(None, mark)] for a plain cell, [(transport, mark)...] for a transport cell
        cells = []
        for column, c in zip( columns, row["cells"] ):
            if isinstance( c, list ):
                cells.append( transport_cell( c, column, where ) )
            elif sym( c ) in CELL:
                if all_apply and sym( c ) == "-":
                    raise ValueError( "%s: a - cell in %s, and every cell of this page applies; move the test to a specific table" % ( where, column ) )
                cells.append( [ ( None, sym( c ) ) ] )
            else:
                raise ValueError( "area %s row %r: cell %r is not y, n, o, p or -" % ( aid, feature, sym( c ) ) )
        evidence = evidence_by_column( row, columns, where )
        for column, marks in zip( columns, cells ):
            if any( m == "y" for _, m in marks ) and column not in evidence:
                raise ValueError( "%s: the y cell in column %s has no evidence naming it" % ( where, column ) )
        for i, marks in enumerate( cells ):
            for _, m in marks:
                if m != "-":
                    total[i] += 1
                    done[i] += m == "y"
                owed = owed or m == "o"
        lines.append( "| " + feature + " | " + " | ".join( cell_text( marks ) for marks in cells ) + " |" )
        for column, text in pending_of( row, columns, cells, where ):
            pending.append( "- %s, %s: %s" % ( feature, column, text ) )
    pct = [ complete_cell( d, t ) for d, t in zip( done, total ) ]
    lines.append( "| complete | " + " | ".join( pct ) + " |" )
    return lines, pending, owed

def specific_table( area, aid, columns, tests ):
    noun = SET_NOUN[set_of( columns )]
    lines = [ "| %s | %s | status |" % ( area.get( "label", "feature" ), noun ), "|---|:---:|:---:|" ]
    pending = []
    owed = False
    done = total = 0
    for r in area["rows"]:
        row = plist( r, "area %s row" % aid )
        for key in ( "feature", "on", "cell" ):
            if key not in row:
                raise ValueError( "area %s row %r: no :%s" % ( aid, row.get( "feature" ), key ) )
        feature = row["feature"]
        on = sym( row["on"] )
        mark = sym( row["cell"] )
        where = "area %s row %r" % ( aid, feature )
        if on not in columns:
            raise ValueError( "%s: %r is not a %s" % ( where, on, noun ) )
        if on in tests.setdefault( feature, [] ):
            raise ValueError( "%s: %s named twice" % ( where, on ) )
        tests[feature].append( on )
        if mark not in ( "y", ) + OPEN:
            raise ValueError( "%s: status %r is not y, n, o or p" % ( where, mark ) )
        if mark == "y" and on not in evidence_by_column( row, [ on ], where ):
            raise ValueError( "%s: the y status on %s has no evidence naming it" % ( where, on ) )
        note = row.get( "pending" )
        if note is not None:
            if not isinstance( note, str ):
                raise ValueError( "%s: pending is one string" % where )
            if mark == "y":
                raise ValueError( "%s: pending for %s, whose status is y" % ( where, on ) )
            pending.append( "- %s, %s: %s" % ( feature, on, note ) )
        elif mark in ( "o", "p" ):
            raise ValueError( "%s: an %s status on %s needs a pending note" % ( where, mark, on ) )
        total += 1
        done += mark == "y"
        owed = owed or mark == "o"
        lines.append( "| %s | %s | %s |" % ( feature, on, CELL[mark] ) )
    lines.append( "| complete | %d of %d | %s |" % ( done, total, complete_cell( done, total ) ) )
    return lines, pending, owed

def cell_text( marks ):
    if len( marks ) == 1 and marks[0][0] is None:
        return CELL[marks[0][1]]
    return ", ".join( "%s %s" % ( name, CELL[m] ) for name, m in marks )

def pending_of( row, columns, cells, where ):
    out = []
    for pair in row.get( "pending" ) or []:
        if not isinstance( pair, list ) or len( pair ) != 2 or not isinstance( pair[1], str ):
            raise ValueError( "%s: pending entries are (column \"text\") pairs" % where )
        column = sym( pair[0] )
        if column not in columns:
            raise ValueError( "%s: pending for %r, which is not a column of this area" % ( where, column ) )
        if not any( m in OPEN for _, m in cells[columns.index( column )] ):
            raise ValueError( "%s: pending for %s, whose cell has no n, o or p" % ( where, column ) )
        out.append( ( column, pair[1] ) )
    for column, marks in zip( columns, cells ):
        for open_mark in ( "o", "p" ):
            if any( m == open_mark for _, m in marks ) and column not in [ c for c, _ in out ]:
                raise ValueError( "%s: an %s cell in %s needs a pending note" % ( where, open_mark, column ) )
    return out

def complete_cell( done, applies ):
    if not applies:
        return "—"
    return "%d%%" % round( 100 * done / applies )

def main( argv ):
    check = False
    if argv and argv[0] == "--check":
        check = True
        argv = argv[1:]
    if len( argv ) != 2:
        print( "usage: render.py [--check] docs/roadmap.sexp docs/ROADMAP.md | docs/testing.sexp docs/TESTING.md", file=sys.stderr )
        return 2
    source, target = argv
    with open( source, encoding="utf-8" ) as f:
        try:
            page = render( parse( f.read() ), source )
        except ( ValueError, KeyError ) as e:
            print( "roadmap: %s: %s" % ( source, e ), file=sys.stderr )
            return 1
    if check:
        try:
            with open( target, encoding="utf-8" ) as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != page:
            print( "roadmap-check: %s is not what %s renders; run make %s" % ( target, source, re.sub( r"\.sexp$", "", source.split( "/" )[-1] ) ), file=sys.stderr )
            return 1
        return 0
    with open( target, "w", encoding="utf-8" ) as f:
        f.write( page )
    return 0

if __name__ == "__main__":
    sys.exit( main( sys.argv[1:] ) )
