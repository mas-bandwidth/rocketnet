#!/usr/bin/env python3
# RocketNet. Copyright 2026 Mas Bandwidth LLC.
#
# Unit cases for tools/roadmap/render.py: no files written, no network.
#
#   python3 -m unittest discover -s tools/roadmap -p 'test_*.py'
#   python3 -m unittest tools.roadmap.test_render.RenderTest.test_complete_row_skips_not_applicable

import glob
import io
import os
import re
import sys
import unittest
from unittest import mock

HERE = os.path.dirname( os.path.abspath( __file__ ) )
ROOT = os.path.dirname( os.path.dirname( HERE ) )
sys.path.insert( 0, HERE )

import render

def page( areas ):
    return '(:title "T" :areas (' + " ".join( areas ) + "))"

def area( columns, rows, aid="a" ):
    return '(:id "%s" :title "A" :columns %s :rows (%s))' % ( aid, columns, " ".join( rows ) )

def table( text ):
    lines = render.render( render.parse( text ) ).split( "\n" )
    return [ l for l in lines if l.startswith( "| " ) ]

def cells_of( line ):
    return [ c.strip() for c in line.strip( "|" ).split( "|" ) ][1:]

class RenderTest( unittest.TestCase ):

    def refusal( self, text ):
        with self.assertRaises( ValueError ) as caught:
            render.render( render.parse( text ) )
        return str( caught.exception )

    def test_transport_columns_accepted( self ):
        rows = table( page( [ area( "transports", [ '(:feature "f" :cells (n n n n n n n n))' ] ) ] ) )
        self.assertEqual( cells_of( rows[0] ), render.TRANSPORTS )

    def test_platform_columns_accepted( self ):
        rows = table( page( [ area( "platforms", [ '(:feature "f" :cells (n n n))' ] ) ] ) )
        self.assertEqual( cells_of( rows[0] ), [ "Linux", "macOS", "Windows" ] )

    def test_platform_columns_as_a_list_accepted( self ):
        rows = table( page( [ area( '("Linux" "macOS" "Windows")', [ '(:feature "f" :cells (n n n))' ] ) ] ) )
        self.assertEqual( cells_of( rows[0] ), [ "Linux", "macOS", "Windows" ] )

    def test_unknown_column_refused( self ):
        message = self.refusal( page( [ area( '("done")', [ '(:feature "f" :cells (n))' ], aid="old" ) ] ) )
        self.assertIn( "area old", message )
        self.assertIn( "'done'", message )
        message = self.refusal( page( [ area( "done", [ '(:feature "f" :cells (n))' ], aid="old" ) ] ) )
        self.assertIn( "area old", message )

    def test_part_of_a_set_refused( self ):
        message = self.refusal( page( [ area( '("Linux" "macOS")', [ '(:feature "f" :cells (n n))' ] ) ] ) )
        self.assertIn( "whole", message )

    def test_complete_row_skips_not_applicable( self ):
        # Linux: y y n -  -> 2 of 3 apply and are done: 67%
        # macOS: y n n n  -> 1 of 4: 25%
        # Windows: - - - - -> nothing applies: —
        rows = table( page( [ area( "platforms", [
            '(:feature "one" :cells (y y -) :evidence "Linux and macOS: t1")',
            '(:feature "two" :cells (y n -) :evidence "Linux: t2")',
            '(:feature "three" :cells (n n -))',
            '(:feature "four" :cells (- n -))' ] ) ] ) )
        self.assertEqual( rows[-1].split( "|" )[1].strip(), "complete" )
        self.assertEqual( cells_of( rows[-1] ), [ "67%", "25%", "—" ] )

    def test_complete_row_rounds( self ):
        # 5 of 6 is 83.3 -> 83%; 1 of 6 is 16.7 -> 17%
        rows = [ '(:feature "r%d" :cells (%s %s n) :evidence "Linux, macOS: t")' % ( i, "y" if i < 5 else "n", "y" if i < 1 else "n" ) for i in range( 6 ) ]
        last = table( page( [ area( "platforms", rows ) ] ) )[-1]
        self.assertEqual( cells_of( last ), [ "83%", "17%", "0%" ] )

    def test_complete_row_present_and_last( self ):
        text = page( [ area( "platforms", [ '(:feature "f" :cells (y n -) :evidence "Linux: t")', '(:feature "g" :cells (n n n))' ], aid="a" ),
                       area( "transports", [ '(:feature "h" :cells (n n n n n n n n))' ], aid="b" ) ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        tables = []
        for l in lines:
            if l.startswith( "| feature " ):
                tables.append( [] )
            elif l.startswith( "| " ):
                tables[-1].append( l )
        self.assertEqual( len( tables ), 2 )
        for t in tables:
            self.assertTrue( t[-1].startswith( "| complete |" ) )
            self.assertEqual( sum( 1 for l in t if l.startswith( "| complete |" ) ), 1 )

    def test_cell_count_refused( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "short row" :cells (y n) :evidence "Linux: t")' ], aid="conn" ) ] ) )
        self.assertIn( "area conn", message )
        self.assertIn( "short row", message )
        self.assertIn( "2 cells for 3 columns", message )

    def test_y_without_evidence_refused( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "bare" :cells (y n n))' ], aid="conn" ) ] ) )
        self.assertIn( "area conn", message )
        self.assertIn( "bare", message )
        self.assertIn( "Linux", message )

    def test_evidence_must_name_its_column( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "wide" :cells (y y n) :evidence "Linux: t")' ], aid="conn" ) ] ) )
        self.assertIn( "wide", message )
        self.assertIn( "macOS", message )

    def test_evidence_counts_labels_only( self ):
        labelled = '(:feature "f" :cells (y y y) :evidence "Linux: t1; macOS and Windows: t2")'
        self.assertEqual( cells_of( table( page( [ area( "platforms", [ labelled ] ) ] ) )[1] ), [ "✅", "✅", "✅" ] )
        passing = '(:feature "f" :cells (y y n) :evidence "Linux: t1; the macOS job is disabled")'
        self.assertIn( "macOS", self.refusal( page( [ area( "platforms", [ passing ] ) ] ) ) )
        lower = '(:feature "f" :cells (y n n) :evidence "linux: t1")'
        self.assertIn( "Linux", self.refusal( page( [ area( "platforms", [ lower ] ) ] ) ) )
        inside = '(:feature "f" :cells (n n y n n n n n) :evidence "test_server_gso_wire")'
        self.assertIn( "gso", self.refusal( page( [ area( "transports", [ inside ] ) ] ) ) )

    def test_complete_row_rounds_half_to_even( self ):
        # round() as the complete row's rule: 1 of 8 is 12.5 -> 12%, 3 of 8 is 37.5 -> 38%
        rows = [ '(:feature "r%d" :cells (%s %s n) :evidence "Linux, macOS: t")' % ( i, "y" if i < 1 else "n", "y" if i < 3 else "n" ) for i in range( 8 ) ]
        self.assertEqual( cells_of( table( page( [ area( "platforms", rows ) ] ) )[-1] ), [ "12%", "38%", "0%" ] )

    def test_missing_key_names_area_and_row( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "nocells")' ], aid="conn" ) ] ) )
        self.assertIn( "area conn", message )
        self.assertIn( "nocells", message )

    def test_evidence_per_cell( self ):
        ok = '(:feature "f" :cells (n n y y n n n n) :evidence ((gso "t1") (io_uring "t2")))'
        self.assertEqual( cells_of( table( page( [ area( "transports", [ ok ] ) ] ) )[1] ), [ "❌", "❌", "✅", "✅", "❌", "❌", "❌", "❌" ] )
        missing = '(:feature "f" :cells (n n y y n n n n) :evidence ((gso "t1")))'
        self.assertIn( "io_uring", self.refusal( page( [ area( "transports", [ missing ] ) ] ) ) )
        foreign = '(:feature "f" :cells (y n n) :evidence ((Linux "t") (gso "t")))'
        self.assertIn( "gso", self.refusal( page( [ area( "platforms", [ foreign ] ) ] ) ) )

    def test_refusal_exits_one( self ):
        bad = page( [ area( "platforms", [ '(:feature "bare" :cells (y n n))' ], aid="conn" ) ] )
        def fake_open( path, *args, **kwargs ):
            self.assertEqual( path, "data.sexp" )
            return io.StringIO( bad )
        err = io.StringIO()
        with mock.patch( "builtins.open", fake_open ), mock.patch( "sys.stderr", err ):
            self.assertEqual( render.main( [ "--check", "data.sexp", "PAGE.md" ] ), 1 )
        self.assertIn( "area conn", err.getvalue() )
        self.assertIn( "bare", err.getvalue() )

    def test_transports_match_the_registry( self ):
        # The registry rows: ROCKETNET_<X>( ID, "name", discovers, ops, platforms, server rank, client rank ).
        rows = []
        for path in sorted( glob.glob( os.path.join( ROOT, "sdk", "rocketnet_*.inc" ) ) ):
            with open( path, encoding="utf-8" ) as f:
                for m in re.finditer( r'^ROCKETNET_[A-Z]+\(\s*\w+,\s*"([a-z_]+)",[^,]*,[^,]*,[^,]*,\s*([^,]+),', f.read(), re.M ):
                    server = m.group( 2 ).strip()
                    development_rank = re.fullmatch( r'\(\s*ROCKETNET_DEV\s*\?\s*(\d+)\s*:\s*\d+\s*\)', server )
                    if development_rank:
                        server = development_rank.group( 1 )
                    elif not re.fullmatch( r'\d+|ROCKETNET_TRANSPORT_PIN_ONLY', server ):
                        self.fail( "unexpected server transport rank %r for %s" % ( server, m.group( 1 ) ) )
                    rows.append( ( m.group( 1 ), server ) )
        self.assertTrue( rows, "no transport registry under sdk/" )
        def rank( item ):
            index, ( name, server ) = item
            return ( 1, 0, index ) if server.endswith( "PIN_ONLY" ) else ( 0, int( server ), index )
        ranked = [ name for _, ( name, _ ) in sorted( enumerate( rows ), key=rank ) ]
        self.assertEqual( render.TRANSPORTS, ranked )

    def test_the_committed_data_renders( self ):
        with open( os.path.join( ROOT, "docs", "roadmap.sexp" ), encoding="utf-8" ) as f:
            text = render.render( render.parse( f.read() ) )
        self.assertNotIn( "| feature | done |", text )

    # TESTING.md: the same renderer, a second page (docs/testing.sexp).

    def test_platform_columns_in_any_order( self ):
        rows = table( page( [ area( '("Windows" "macOS" "Linux")', [ '(:feature "f" :cells (y n -) :evidence "Windows: t")' ] ) ] ) )
        self.assertEqual( cells_of( rows[0] ), [ "Windows", "macOS", "Linux" ] )
        self.assertEqual( cells_of( rows[1] ), [ "✅", "❌", "—" ] )
        message = self.refusal( page( [ area( '("Windows" "Windows" "Linux")', [ '(:feature "f" :cells (n n n))' ], aid="dup" ) ] ) )
        self.assertIn( "area dup", message )
        self.assertIn( "whole", message )

    def test_transport_cells_name_each_transport( self ):
        row = '(:feature "f" :cells (((portable y) (iocp n) (rio n)) ((msgx n) (portable n)) -) :evidence "Windows: t")'
        rows = table( page( [ area( '("Windows" "macOS" "Linux")', [ row ] ) ] ) )
        self.assertEqual( cells_of( rows[1] ), [ "portable ✅, iocp ❌, rio ❌", "msgx ❌, portable ❌", "—" ] )

    def test_transport_cell_outside_its_platform_refused( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "wrong" :cells (((msgx n)) n n))' ], aid="tr" ) ] ) )
        self.assertIn( "area tr", message )
        self.assertIn( "wrong", message )
        self.assertIn( "msgx", message )
        self.assertIn( "Linux", message )

    def test_transport_cell_marks_refused( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "bad" :cells (((gso x)) n n))' ] ) ] ) )
        self.assertIn( "bad", message )
        self.assertIn( "'x'", message )
        message = self.refusal( page( [ area( "platforms", [ '(:feature "twice" :cells (((gso n) (gso n)) n n))' ] ) ] ) )
        self.assertIn( "twice", message )
        self.assertIn( "gso", message )

    def test_transport_cell_y_needs_evidence( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "bare" :cells (((gso y)) n n))' ] ) ] ) )
        self.assertIn( "bare", message )
        self.assertIn( "Linux", message )

    def test_complete_row_counts_each_transport( self ):
        # Linux: gso y, xdp n, then y  -> 2 of 3: 67%; macOS: n, - -> 0 of 1: 0%; Windows: - -> —
        rows = table( page( [ area( "platforms", [
            '(:feature "one" :cells (((gso y) (xdp n)) n -) :evidence "Linux: t")',
            '(:feature "two" :cells (y - -) :evidence "Linux: t")' ] ) ] ) )
        self.assertEqual( cells_of( rows[-1] ), [ "67%", "0%", "—" ] )

    def test_label_names_the_first_column( self ):
        text = page( [ '(:id "a" :title "A" :label "test" :columns platforms :rows ((:feature "f" :cells (n n n))))' ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        self.assertIn( "| test | Linux | macOS | Windows |", lines )

    def test_pending_is_listed_under_its_table( self ):
        row = '(:feature "f" :cells (y n ((portable n) (iocp y))) :evidence "Linux: t; Windows: t" :pending ((macOS "card-a, working") (Windows "no card")))'
        lines = render.render( render.parse( page( [ area( "platforms", [ row ] ) ] ) ) ).split( "\n" )
        self.assertIn( "- f, macOS: card-a, working", lines )
        self.assertIn( "- f, Windows: no card", lines )
        self.assertLess( lines.index( "| complete | 100% | 0% | 50% |" ), lines.index( "- f, macOS: card-a, working" ) )

    def test_pending_needs_an_open_cell( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "done" :cells (y n n) :evidence "Linux: t" :pending ((Linux "card-a")))' ], aid="p" ) ] ) )
        self.assertIn( "area p", message )
        self.assertIn( "done", message )
        self.assertIn( "Linux", message )

    def test_page_names_its_source( self ):
        data = render.parse( page( [ area( "platforms", [ '(:feature "f" :cells (n n n))' ] ) ] ) )
        self.assertEqual( render.render( data ), render.render( data, "docs/roadmap.sexp" ) )
        text = render.render( data, "docs/testing.sexp" )
        self.assertIn( "<!-- Generated from docs/testing.sexp by tools/roadmap/render.py. Do not edit this file: edit the data and run `make testing`. -->", text )
        self.assertIn( "[Source data](docs/testing.sexp)", text )
        self.assertNotIn( "roadmap.sexp", text )

    def test_platform_transports_match_the_registry( self ):
        # The platforms column of each registry row names where the transport builds.
        bits = { "ROCKETNET_TRANSPORT_ALL_PLATFORMS": render.PLATFORMS, "ROCKETNET_TRANSPORT_LINUX": [ "Linux" ],
                 "ROCKETNET_TRANSPORT_IO_URING_PLATFORMS": [ "Linux" ], "ROCKETNET_TRANSPORT_MAC": [ "macOS" ],
                 "ROCKETNET_TRANSPORT_WINDOWS": [ "Windows" ] }
        built = { p: set() for p in render.PLATFORMS }
        with open( os.path.join( ROOT, "sdk", "rocketnet_drivers.inc" ), encoding="utf-8" ) as f:
            for m in re.finditer( r'^ROCKETNET_TRANSPORT\(\s*\w+,\s*"([a-z_]+)",[^,]*,[^,]*,\s*(\w+),', f.read(), re.M ):
                for p in bits[m.group( 2 )]:
                    built[p].add( m.group( 1 ) )
        self.assertEqual( { p: set( t ) for p, t in render.PLATFORM_TRANSPORTS.items() }, built )

    def test_the_committed_testing_page_is_what_its_data_renders( self ):
        with open( os.path.join( ROOT, "docs", "testing.sexp" ), encoding="utf-8" ) as f:
            text = render.render( render.parse( f.read() ), "docs/testing.sexp" )
        with open( os.path.join( ROOT, "TESTING.md" ), encoding="utf-8" ) as f:
            self.assertEqual( f.read(), text, "TESTING.md is not what docs/testing.sexp renders; run make testing" )

    # The testing page's layout: owed cells, specific tables, every cell applying, tables only.

    def test_owed_cell_shows_owed_and_counts_as_not_done( self ):
        text = '(:title "T" :owed "deferred" :areas (%s))' % area( "platforms", [
            '(:feature "f" :cells (y o n) :evidence "Linux: t" :pending ((macOS "card-a (waiting)")))' ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        self.assertIn( "| f | ✅ | planned for the final deliverable | ❌ |", lines )
        self.assertIn( "| complete | 100% | 0% | 0% |", lines )
        self.assertIn( "Planned for the final deliverable: deferred", lines )

    def test_owed_cell_needs_a_note_and_the_page_note( self ):
        bare = '(:title "T" :owed "deferred" :areas (%s))' % area( "platforms", [ '(:feature "f" :cells (n o n))' ], aid="ow" )
        message = self.refusal( bare )
        self.assertIn( "area ow", message )
        self.assertIn( "macOS", message )
        no_note = page( [ area( "platforms", [ '(:feature "f" :cells (n o n) :pending ((macOS "card-a (waiting)")))' ], aid="ow" ) ] )
        self.assertIn( ":owed", self.refusal( no_note ) )
        tables_only = '(:title "T" :tables-only t :areas (%s))' % area( "platforms", [ '(:feature "f" :cells (n o n) :pending ((macOS "card-a (waiting)")))' ], aid="ow" )
        self.assertIn( ":owed", self.refusal( tables_only ) )

    def test_a_test_named_twice_in_a_full_table_refused( self ):
        message = self.refusal( page( [ area( "platforms", [ '(:feature "f" :cells (n n n))', '(:feature "f" :cells (n n n))' ], aid="dup" ) ] ) )
        self.assertIn( "area dup", message )
        self.assertIn( "twice", message )

    def test_every_cell_applies_refuses_a_dash( self ):
        text = '(:title "T" :all-cells-apply t :areas (%s))' % area( "platforms", [ '(:feature "only one" :cells (n - -))' ], aid="pl" )
        message = self.refusal( text )
        self.assertIn( "area pl", message )
        self.assertIn( "only one", message )
        self.assertIn( "specific", message )

    def specific( self, columns, rows, aid="s" ):
        return '(:id "%s" :title "S" :label "test" :specific t :columns %s :rows (%s))' % ( aid, columns, " ".join( rows ) )

    def test_specific_table_rows_and_complete( self ):
        text = page( [ self.specific( "platforms", [
            '(:feature "connreset" :on Windows :cell n :pending "card-a (working)")',
            '(:feature "app nap" :on macOS :cell y :evidence "macOS: t")' ] ) ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        self.assertIn( "| test | platform | status |", lines )
        self.assertIn( "| connreset | Windows | ❌ |", lines )
        self.assertIn( "| app nap | macOS | ✅ |", lines )
        self.assertIn( "| complete | 1 of 2 | 50% |", lines )
        text = page( [ self.specific( "transports", [ '(:feature "verifier" :on iocp :cell y :evidence "iocp: t")' ] ) ] )
        self.assertIn( "| test | transport | status |", render.render( render.parse( text ) ).split( "\n" ) )

    def test_specific_table_refusals( self ):
        self.assertIn( "Linux", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell y)' ] ) ] ) ) )
        self.assertIn( "'gso'", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on gso :cell n)' ] ) ] ) ) )
        self.assertIn( "'-'", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell -)' ] ) ] ) ) )
        self.assertIn( "twice", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell n)', '(:feature "f" :on Linux :cell n)' ] ) ] ) ) )
        self.assertIn( "pending", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell o)' ] ) ] ) ) )
        every = [ '(:feature "f" :on %s :cell n)' % p for p in render.PLATFORMS ]
        self.assertIn( "full table", self.refusal( page( [ self.specific( "platforms", every ) ] ) ) )
        both = page( [ area( "platforms", [ '(:feature "f" :cells (n n n))' ], aid="full" ), self.specific( "platforms", [ '(:feature "f" :on Linux :cell n)' ] ) ] )
        message = self.refusal( both )
        self.assertIn( "area full", message )
        self.assertIn( "specific platform table", message )

    def test_tables_only_prints_headers_links_and_tables( self ):
        row = '(:feature "f" :cells (y n o) :evidence "Linux: t" :pending ((macOS "card-a (working)") (Windows "card-b (waiting)")))'
        text = '(:title "T" :tables-only t :owed "deferred" :text "intro" :areas ((:id "a" :title "A" :text "about" :columns platforms :rows (%s))))' % row
        out = render.render( render.parse( text ), "docs/testing.sexp" ).split( "\n" )
        self.assertIn( "Source: [docs/testing.sexp](docs/testing.sexp), [tools/roadmap/render.py](tools/roadmap/render.py)", out )
        for gone in ( "intro", "about", "card-a", "card-b", "deferred", "[Source data]" ):
            self.assertFalse( [ l for l in out if gone in l ], gone )
        self.assertLess( out.index( "## A" ), out.index( "| feature | Linux | macOS | Windows |" ) )

    def committed_testing_page( self ):
        with open( os.path.join( ROOT, "TESTING.md" ), encoding="utf-8" ) as f:
            return f.read()

    def test_the_testing_page_has_no_dash_cell( self ):
        page_text = self.committed_testing_page()
        self.assertTrue( [ l for l in page_text.split( "\n" ) if l.startswith( "| " ) ] )
        self.assertNotIn( "—", page_text )

    def test_the_testing_page_has_no_bullet_list( self ):
        bullets = [ l for l in self.committed_testing_page().split( "\n" ) if re.match( r"\s*([-*+]|\d+[.)])\s", l ) ]
        self.assertEqual( bullets, [] )

    def test_the_testing_page_is_headers_and_tables( self ):
        sections = {}
        current = None
        for l in self.committed_testing_page().split( "\n" ):
            if l.startswith( "## " ):
                current = l[3:]
                sections[current] = []
            elif current and l:
                sections[current].append( l )
        self.assertEqual( list( sections ), [ "Preview-2 exact-head qualification",
                                             "Historical preview-1 baseline: tests per platform",
                                             "Historical preview-1 baseline: platform-specific tests",
                                             "Historical preview-1 baseline: tests per transport",
                                             "Historical preview-1 baseline: transport-specific tests",
                                             "Release-wide test inventory and historical preview-1 results" ] )
        for name, body in sections.items():
            self.assertTrue( body[0].startswith( "Source: [docs/testing.sexp](docs/testing.sexp)" ), name )
            self.assertEqual( [ l for l in body[1:] if not l.startswith( "|" ) ], [], name )

    def test_the_transport_tables_name_no_platform( self ):
        sections = {}
        current = None
        for l in self.committed_testing_page().split( "\n" ):
            if l.startswith( "#" ):
                current = l
                sections[current] = []
            elif current and l.startswith( "| " ):
                sections[current].append( [ c.strip() for c in l.strip( "|" ).split( "|" ) ] )
        transport_headings = [ h for h in sections if "transport" in h.lower() ]
        self.assertEqual( transport_headings,
                          [ "## Historical preview-1 baseline: tests per transport",
                            "## Historical preview-1 baseline: transport-specific tests" ] )
        for heading in transport_headings:
            for platform in render.PLATFORMS:
                self.assertNotIn( platform, heading )
                self.assertNotIn( platform, sections[heading][0] )
            column_values = { row[1] for row in sections[heading][1:] if row[0] != "complete" }
            self.assertFalse( column_values & set( render.PLATFORMS ), heading )
        self.assertEqual( sections["## Historical preview-1 baseline: tests per transport"][0][1:], render.TRANSPORTS )

    # PLANNED cells, the release-wide table and the source of every row.

    def test_planned_cell_shows_planned_and_needs_a_note( self ):
        text = page( [ area( "platforms", [ '(:feature "f" :cells (y p n) :evidence "Linux: t" :pending ((macOS "card-a (working)")))' ] ) ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        self.assertIn( "| f | ✅ | planned for the final deliverable | ❌ |", lines )
        self.assertIn( "| complete | 100% | 0% | 0% |", lines )
        message = self.refusal( page( [ area( "platforms", [ '(:feature "f" :cells (n p n))' ], aid="pl" ) ] ) )
        self.assertIn( "area pl", message )
        self.assertIn( "macOS", message )
        self.assertIn( "pending", self.refusal( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell p)' ] ) ] ) ) )
        lines = render.render( render.parse( page( [ self.specific( "platforms", [ '(:feature "f" :on Linux :cell p :pending "card-a (waiting)")' ] ) ] ) ) ).split( "\n" )
        self.assertIn( "| f | Linux | planned for the final deliverable |", lines )

    def test_release_column_is_one_status_column( self ):
        text = page( [ area( "release", [ '(:feature "model" :cells (y) :evidence "status: run 1")', '(:feature "review" :cells (p) :pending ((status "no card")))' ] ) ] )
        lines = render.render( render.parse( text ) ).split( "\n" )
        self.assertIn( "| feature | status |", lines )
        self.assertIn( "| model | ✅ |", lines )
        self.assertIn( "| review | planned for the final deliverable |", lines )
        self.assertIn( "| complete | 50% |", lines )
        self.assertIn( "no evidence", self.refusal( page( [ area( "release", [ '(:feature "model" :cells (y))' ] ) ] ) ) )

    def test_rows_name_source_refuses_a_row_without_one( self ):
        text = '(:title "T" :rows-name-source t :areas (%s))' % area( "platforms", [ '(:feature "f" :cells (n n n))' ], aid="src" )
        message = self.refusal( text )
        self.assertIn( "area src", message )
        self.assertIn( ":source", message )
        with_source = '(:title "T" :rows-name-source t :areas (%s))' % area( "platforms", [ '(:feature "f" :cells (n n n) :source "test.cpp")' ] )
        self.assertTrue( render.render( render.parse( with_source ) ) )

    def plan_rows( self ):
        with open( os.path.join( ROOT, "docs", "testing.sexp" ), encoding="utf-8" ) as f:
            top = render.plist( render.parse( f.read() ), "testing" )
        out = []
        for a in top["areas"]:
            for r in render.plist( a, "area" )["rows"]:
                out.append( render.plist( r, "row" ) )
        return top, out

    def test_the_testing_plan_keeps_every_deferred_run_open( self ):
        # The long runs deferred to the final deliverable on 2026-10-20: each is a row of the
        # plan, and every cell of it that is not green is planned (o or p), never dropped,
        # except the example soak, a known red that stays a ❌.
        known_red = { "Example soak (example_soak)" }
        deferred = [ "Functional suite repeated, zero flakes", "Transport matrix, every cell fifty times",
                     "1000 clients, load harness", "1000 light clients (10 Hz, 1 packet per update)",
                     "Soak (30 minutes per transport under impairment)", "Nightlies (every nightly job's steps green on this branch)",
                     "Every matrix cell fifty times, zero flakes", "Soak under impairment, flat resources",
                     "1000 clients over the LAN, every guarantee checked (soak matrix)", "Sanitizers per transport",
                     "Sanitizers (ASan, UBSan, TSan; MSVC ASan on Windows)", "Leaks (LSan, the leaks tool, App Verifier's leak layer)",
                     "libFuzzer (corpus replay and fuzz runs; clang-cl on Windows)", "Lifecycle stress (init/term, create/destroy, restart, destroy under traffic)",
                     "Hard-killed client never stalls receive", "Receipt loss reported after its own deadline",
                     "Send budget: packets_per_update 256 at 60 Hz, receive sized at least 2x",
                     "Source format in the netcode style (clang-format check)",
                     "TLA+ ledger current (tla/CASES.tsv and tla/COVERAGE.tsv match the models)",
                     "Example soak (example_soak)" ]
        top, rows = self.plan_rows()
        self.assertEqual( render.sym( top.get( "rows-name-source" ) ), "t" )
        for name in deferred:
            marks = []
            for row in rows:
                if row["feature"] == name:
                    cells = [ row["cell"] ] if "cell" in row else row["cells"]
                    marks += [ render.sym( c ) for c in cells ]
            self.assertTrue( marks, name )
            if name in known_red:
                self.assertIn( "n", marks, name )
            else:
                self.assertTrue( set( marks ) <= { "y", "o", "p" }, name )
                self.assertTrue( set( marks ) & { "o", "p" }, name )

    def test_the_plan_names_no_card_or_process_word( self ):
        # The data files are read by integrators: no work-item ids and no process jargon.
        for name in ( "roadmap.sexp", "testing.sexp" ):
            with open( os.path.join( ROOT, "docs", name ), encoding="utf-8" ) as f:
                text = f.read()
            self.assertIsNone( re.search( r"rocketnet-[a-z0-9]+-[a-z0-9-]+|\bsprint\b|\bcards?\b|\bOWED\b|\bPLANNED\b", text ), name )

    def test_the_testing_plan_covers_every_planned_test( self ):
        _, rows = self.plan_rows()
        features = " | ".join( row["feature"] for row in rows )
        for test in ( "Unit suite", "Functional suite (", "Functional suite repeated", "Examples (run_examples", "Example soak",
                      "Sanitizers (ASan, UBSan, TSan; MSVC ASan", "Leaks (LSan, the leaks tool, App Verifier", "libFuzzer", "clang-cl",
                      "Toolchain matrix (MSVC, clang-cl", "Xcode universal", "gcc and clang", "Soak under impairment",
                      "Transport matrix, every cell fifty times", "1000 light clients (10 Hz, 1 packet per update)",
                      "1000 clients over the LAN", "Hard-killed client never stalls receive", "Lifecycle stress", "Socket error paths",
                      "SIO_UDP_CONNRESET", "Nightlies", "Live XDP test", "Holistic review: six", "Frama-C",
                      "Main API calls under 0.1 ms each, Release, under load", "Security pass: per-client resource exhaustion",
                      "Send budget: packets_per_update 256 at 60 Hz, receive sized at least 2x" ):
            self.assertIn( test, features )
        with open( os.path.join( ROOT, "tla", "CASES.tsv" ), encoding="utf-8" ) as f:
            groups = { l.split( "\t" )[5] for l in f.read().splitlines()[1:] if l.strip() }
        for group in groups:
            self.assertIn( "TLA+ model check: %s (" % group, features )

if __name__ == "__main__":
    unittest.main()
