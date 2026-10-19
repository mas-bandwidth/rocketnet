#!/usr/bin/env python3
# RocketNet. Copyright 2026 Mas Bandwidth LLC.
#
# Renders ROADMAP.md from docs/roadmap.sexp: one section per area, a table of
# features with a mark per column, and a closing "complete" row.
#
#   python3 tools/roadmap/render.py docs/roadmap.sexp ROADMAP.md          write the page
#   python3 tools/roadmap/render.py --check docs/roadmap.sexp ROADMAP.md  exit 1 if it differs
#
# An area declares :columns transports (a column per server transport, in the
# order the automatic ladder ranks them in sdk/rocketnet_drivers.inc) or
# :columns platforms (Linux, macOS, Windows); a list naming one whole set is
# the same declaration. Any other column is refused.
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
        if names == full:
            return full
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

def render( data ):
    top = plist( data, "roadmap" )
    lines = [ "# " + top["title"], "",
              "<!-- Generated from docs/roadmap.sexp by tools/roadmap/render.py. Do not edit this file: edit the data and run `make roadmap`. -->", "" ]
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
        lines.append( "| feature | " + " | ".join( columns ) + " |" )
        lines.append( "|---|" + "|".join( ":---:" for _ in columns ) + "|" )
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
            cells = [ sym( c ) for c in row["cells"] ]
            if len( cells ) != len( columns ):
                raise ValueError( "area %s row %r: %d cells for %d columns" % ( aid, feature, len( cells ), len( columns ) ) )
            for c in cells:
                if c not in CELL:
                    raise ValueError( "area %s row %r: cell %r is not y, n or -" % ( aid, feature, c ) )
            where = "area %s row %r" % ( aid, feature )
            evidence = evidence_by_column( row, columns, where )
            for column, c in zip( columns, cells ):
                if c == "y" and column not in evidence:
                    raise ValueError( "%s: the y cell in column %s has no evidence naming it" % ( where, column ) )
            for i, c in enumerate( cells ):
                if c != "-":
                    total[i] += 1
                    done[i] += c == "y"
            lines.append( "| " + feature + " | " + " | ".join( CELL[c] for c in cells ) + " |" )
        pct = [ complete_cell( d, t ) for d, t in zip( done, total ) ]
        lines.append( "| complete | " + " | ".join( pct ) + " |" )
        lines += [ "", "[Source data](docs/roadmap.sexp)", "" ]
    return "\n".join( lines )

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
        print( "usage: render.py [--check] docs/roadmap.sexp ROADMAP.md", file=sys.stderr )
        return 2
    source, target = argv
    with open( source, encoding="utf-8" ) as f:
        try:
            page = render( parse( f.read() ) )
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
            print( "roadmap-check: %s is not what %s renders; run make roadmap" % ( target, source ), file=sys.stderr )
            return 1
        return 0
    with open( target, "w", encoding="utf-8" ) as f:
        f.write( page )
    return 0

if __name__ == "__main__":
    sys.exit( main( sys.argv[1:] ) )
