#!/usr/bin/env python3
# RocketNet. Copyright 2026 Mas Bandwidth LLC.
#
# Renders ROADMAP.md from docs/roadmap.sexp, and TESTING.md from
# docs/testing.sexp: one section per area, a table of features with a mark per
# column, and a closing "complete" row.
#
#   python3 tools/roadmap/render.py docs/roadmap.sexp ROADMAP.md          write the page
#   python3 tools/roadmap/render.py --check docs/roadmap.sexp ROADMAP.md  exit 1 if it differs
#
# The page names its data file as given on the command line, and the make
# target is that file's name without .sexp (make roadmap, make testing).
#
# An area declares :columns transports (a column per server transport, in the
# order the automatic ladder ranks them in sdk/rocketnet_drivers.inc) or
# :columns platforms (Linux, macOS, Windows); a list naming one whole set, in
# any order, is the same declaration with its columns in that order. Any other
# column is refused. :label names the first column (default "feature").
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
# A cell is y (done and tested), n (not yet) or - (does not apply). Every y
# cell needs evidence: :evidence is one string whose labels name each column
# it covers ("Linux, macOS and Windows: ..." at the start or after "; "), or a
# list of (column "evidence") pairs. A column named only in passing is not
# covered. The renderer refuses the
# data otherwise, naming the area and the row. Evidence stays in the data,
# not on the page.
#
# The complete row gives, per column, round(100 * y / (y + n)) followed by %;
# - cells are left out, and a column that is - throughout shows —.

import re
import sys

CELL = { "y": "✅", "n": "❌", "-": "—" }

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
COLUMN_SETS = { "transports": TRANSPORTS, "platforms": PLATFORMS }

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
        raise ValueError( "area %s: columns %r is neither transports nor platforms" % ( aid, name ) )
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
        if mark not in ( "y", "n" ):
            raise ValueError( "%s: transport %s mark %r is not y or n" % ( where, name, mark ) )
        if name in [ p[0] for p in pairs ]:
            raise ValueError( "%s: transport %s named twice in the %s cell" % ( where, name, column ) )
        pairs.append( ( name, mark ) )
    if not pairs:
        raise ValueError( "%s: an empty transport cell in %s" % ( where, column ) )
    return pairs

def render( data, source="docs/roadmap.sexp" ):
    top = plist( data, "roadmap" )
    target = re.sub( r"\.sexp$", "", source.split( "/" )[-1] )
    lines = [ "# " + top["title"], "",
              "<!-- Generated from %s by tools/roadmap/render.py. Do not edit this file: edit the data and run `make %s`. -->" % ( source, target ), "" ]
    for para in text_list( top.get( "text" ) ):
        lines += [ para, "" ]
    seen = set()
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
        for para in text_list( area.get( "text" ) ):
            lines += [ para, "" ]
        lines.append( "| " + area.get( "label", "feature" ) + " | " + " | ".join( columns ) + " |" )
        lines.append( "|---|" + "|".join( ":---:" for _ in columns ) + "|" )
        pending = []
        done = [ 0 ] * len( columns )
        total = [ 0 ] * len( columns )
        rows = area["rows"]
        if not rows:
            raise ValueError( "area %s: no rows" % aid )
        for r in rows:
            row = plist( r, "area %s row" % aid )
            for key in ( "feature", "cells" ):
                if key not in row:
                    raise ValueError( "area %s row %r: no :%s" % ( aid, row.get( "feature" ), key ) )
            feature = row["feature"]
            where = "area %s row %r" % ( aid, feature )
            if len( row["cells"] ) != len( columns ):
                raise ValueError( "area %s row %r: %d cells for %d columns" % ( aid, feature, len( row["cells"] ), len( columns ) ) )
            # each cell as its marks: [(None, mark)] for a plain cell, [(transport, mark)...] for a transport cell
            cells = []
            for column, c in zip( columns, row["cells"] ):
                if isinstance( c, list ):
                    cells.append( transport_cell( c, column, where ) )
                elif sym( c ) in CELL:
                    cells.append( [ ( None, sym( c ) ) ] )
                else:
                    raise ValueError( "area %s row %r: cell %r is not y, n or -" % ( aid, feature, sym( c ) ) )
            evidence = evidence_by_column( row, columns, where )
            for column, marks in zip( columns, cells ):
                if any( m == "y" for _, m in marks ) and column not in evidence:
                    raise ValueError( "%s: the y cell in column %s has no evidence naming it" % ( where, column ) )
            for i, marks in enumerate( cells ):
                for _, m in marks:
                    if m != "-":
                        total[i] += 1
                        done[i] += m == "y"
            lines.append( "| " + feature + " | " + " | ".join( cell_text( marks ) for marks in cells ) + " |" )
            for column, text in pending_of( row, columns, cells, where ):
                pending.append( "- %s, %s: %s" % ( feature, column, text ) )
        pct = [ complete_cell( d, t ) for d, t in zip( done, total ) ]
        lines.append( "| complete | " + " | ".join( pct ) + " |" )
        if pending:
            lines += [ "" ] + pending
        lines += [ "", "[Source data](%s)" % source, "" ]
    return "\n".join( lines )

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
        if not any( m == "n" for _, m in cells[columns.index( column )] ):
            raise ValueError( "%s: pending for %s, whose cell has no n" % ( where, column ) )
        out.append( ( column, pair[1] ) )
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
        print( "usage: render.py [--check] docs/roadmap.sexp ROADMAP.md | docs/testing.sexp TESTING.md", file=sys.stderr )
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
