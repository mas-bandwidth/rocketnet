#!/usr/bin/env python3
"""The unit suite reads no real time (the owner's rule: unit tests use no real
time, no sleeps, no wall clock, no threads, no hostname lookups and no
sockets; time is an argument). Scans the unit sources and exits 1, naming
file and line, on any call to a sleep, a clock, a thread start or a name
lookup. The socket half is tools/check_unit_no_sockets.sh.

Scanned: every test_unit_*.cpp at the repository root, and
sdk/rocketnet_tests.cpp, whose cases are all registered in the unit table
(rocketnet_unit_tests) and which builds only into ./bin/test
(CMakeLists.txt: the rocketnet_test_support library, linked by the test
target). The whole file is scanned, so a helper a case reaches through a
callback (the log capture) is held to the same rule.

Comments and string literals are blanked before matching, so a name in a
comment or a string (an export list) is not a call. A definition whose name
only ends in a banned name (__wrap_rocketnet_platform_time) is not a call of it.

usage: tools/check_unit_purity.py [repository root]   (default: the parent of tools/)
"""

import glob
import os
import re
import sys

BANNED = [
    ( "rocketnet_platform_sleep", re.compile( r"(?<![A-Za-z0-9_])rocketnet_platform_sleep\s*\(" ) ),
    ( "rocketnet_platform_time", re.compile( r"(?<![A-Za-z0-9_])rocketnet_platform_time\s*\(" ) ),
    ( "time(", re.compile( r"(?<![A-Za-z0-9_.>])time\s*\(" ) ),
    ( "std::thread", re.compile( r"\bstd::thread\b" ) ),
    ( "getaddrinfo", re.compile( r"(?<![A-Za-z0-9_])getaddrinfo\s*\(" ) ),
    ( "rocketnet_platform_thread_create", re.compile( r"(?<![A-Za-z0-9_])rocketnet_platform_thread_create\s*\(" ) ),
]


def blank_comments_and_strings( text ):
    """Return text with comments, string and character literals replaced by
    spaces, newlines kept, so line numbers are unchanged."""
    out = []
    i = 0
    n = len( text )
    while i < n:
        c = text[i]
        if text.startswith( "//", i ):
            j = text.find( "\n", i )
            j = n if j < 0 else j
            out.append( " " * ( j - i ) )
            i = j
        elif text.startswith( "/*", i ):
            j = text.find( "*/", i + 2 )
            j = n if j < 0 else j + 2
            out.append( re.sub( r"[^\n]", " ", text[i:j] ) )
            i = j
        elif c == "\"" or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min( j + 1, n )
            out.append( c + " " * max( 0, j - i - 2 ) + ( c if j - i >= 2 else "" ) )
            i = j
        else:
            out.append( c )
            i += 1
    return "".join( out )


def unit_sources( root ):
    files = sorted( glob.glob( os.path.join( root, "test_unit_*.cpp" ) ) )
    files.append( os.path.join( root, "sdk", "rocketnet_tests.cpp" ) )
    return files


def scan( path, root ):
    with open( path, encoding="utf-8", errors="replace" ) as f:
        text = blank_comments_and_strings( f.read() )
    found = []
    for number, line in enumerate( text.split( "\n" ), start=1 ):
        for name, pattern in BANNED:
            if pattern.search( line ):
                found.append( "%s:%d: %s" % ( os.path.relpath( path, root ), number, name ) )
    return found


def main( argv ):
    root = argv[1] if len( argv ) > 1 else os.path.dirname( os.path.dirname( os.path.abspath( __file__ ) ) )
    files = unit_sources( root )
    missing = [ f for f in files if not os.path.isfile( f ) ]
    if missing or len( files ) < 2:
        print( "check_unit_purity: missing unit source(s): %s" % ", ".join( missing or [ "test_unit_*.cpp" ] ) )
        return 2
    found = []
    for path in files:
        found += scan( path, root )
    for hit in found:
        print( "FAIL check_unit_purity: %s" % hit )
    if found:
        print( "FAIL check_unit_purity: %d real-time call(s) in the unit sources; a case that needs the real thing is functional" % len( found ) )
        return 1
    print( "ok check_unit_purity: no sleep, clock read, thread start or name lookup in %d unit sources" % len( files ) )
    return 0


if __name__ == "__main__":
    sys.exit( main( sys.argv ) )
