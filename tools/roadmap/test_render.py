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

    def test_evidence_per_cell( self ):
        ok = '(:feature "f" :cells (n n y y n n n n) :evidence ((gso "t1") (io_uring "t2")))'
        self.assertEqual( cells_of( table( page( [ area( "transports", [ ok ] ) ] ) )[0] ), [ "❌", "❌", "✅", "✅", "❌", "❌", "❌", "❌" ] )
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
                for m in re.finditer( r'^ROCKETNET_[A-Z]+\(\s*\w+,\s*"([a-z_]+)",[^,]*,[^,]*,[^,]*,\s*(\w+),', f.read(), re.M ):
                    rows.append( ( m.group( 1 ), m.group( 2 ) ) )
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

if __name__ == "__main__":
    unittest.main()
