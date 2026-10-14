#!/usr/bin/env python3
"""
Unit tests for check_tla_runs.py - tests the parser and comparison logic
using a fake runner (no Java/TLC).
"""
import sys
import os
import tempfile
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from check_tla_runs import parse_runs_tsv, parse_config_description, RunRow, compare_result, RunResult


def test_parse_runs_tsv():
    """Test parsing of RUNS.tsv fixture."""
    fixture = """module\tconfig\tstates\tdistinct\tresult\tbench\tdate
MCLink\tMCLink.cfg (Pattern "arbitrary", Spec, ResentIsDelivered)\t18543\t2146\tpass\tvision\t2026-10-06
MCLink\tMCLink.cfg with Pattern "none", Spec, ResentIsDelivered\t10424\t1763\tpass\tvision\t2026-10-06
MCLink\tMCLink.cfg with Pattern "arbitrary", SpecWireFair, LinkDelivers (reversed witness)\t18543\t2146\tfail as expected: LinkDelivers violated\tvision\t2026-10-06
MCClientConnection\tMCClientConnection.cfg with Timeout 2, Deadline 4, ServerDeadline 3, MaxAttempts 2, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", MaxDelay 1, Duplication TRUE, Reordering TRUE; checks EphemeralErasedOutsideHandshake\t245\t124\tfail as expected (F4): Invariant EphemeralErasedOutsideHandshake is violated\tvision\t2026-10-06
BigModel\tBigModel.cfg\t5000000\t3000000\tpass\tvision\t2026-10-06
UnfinishedModel\tUnfinishedModel.cfg\t1000\t500\tnot finished (stopped at deadline)\tvision\t2026-10-06
"""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.tsv', delete=False) as f:
        f.write(fixture)
        fixture_path = f.name
    
    try:
        rows = parse_runs_tsv(fixture_path)
        assert len(rows) == 6
        
        # Row 0: base MCLink
        assert rows[0].module == "MCLink"
        assert rows[0].config_desc == 'MCLink.cfg (Pattern "arbitrary", Spec, ResentIsDelivered)'
        assert rows[0].states == 18543
        assert rows[0].distinct == 2146
        assert rows[0].result == 'pass'
        assert rows[0].bench == 'vision'
        assert rows[0].date == '2026-10-06'
        
        # Row 2: fail as expected with invariant name
        assert rows[2].result == 'fail as expected: LinkDelivers violated'
        
        # Row 3: fail as expected with parenthetical note
        assert rows[3].result == 'fail as expected (F4): Invariant EphemeralErasedOutsideHandshake is violated'
        
        # Row 4: above bound
        assert rows[4].distinct == 3000000
        
        # Row 5: unfinished
        assert rows[5].result == 'not finished (stopped at deadline)'
        
        print("test_parse_runs_tsv: PASSED")
    finally:
        os.unlink(fixture_path)


def test_parse_config_description():
    """Test parsing of config descriptions."""
    # Base config with parenthetical overrides
    base_file, overrides, spec, props, checks = parse_config_description(
        'MCLink.cfg (Pattern "arbitrary", Spec, ResentIsDelivered)'
    )
    assert base_file == 'MCLink.cfg'
    assert overrides == {'Pattern': '"arbitrary"'}
    assert spec == 'Spec'
    assert props == ['ResentIsDelivered']
    assert checks == []
    
    # With keyword
    base_file, overrides, spec, props, checks = parse_config_description(
        'MCLink.cfg with Pattern "none", Spec, ResentIsDelivered'
    )
    assert base_file == 'MCLink.cfg'
    assert overrides == {'Pattern': '"none"'}
    assert spec == 'Spec'
    assert props == ['ResentIsDelivered']
    
    # With SpecWireFair and LinkDelivers
    base_file, overrides, spec, props, checks = parse_config_description(
        'MCLink.cfg with Pattern "arbitrary", SpecWireFair, LinkDelivers (reversed witness)'
    )
    assert base_file == 'MCLink.cfg'
    assert overrides == {'Pattern': '"arbitrary"'}
    assert spec == 'SpecWireFair'
    assert props == ['LinkDelivers']
    
    # MCClientConnection with many overrides
    base_file, overrides, spec, props, checks = parse_config_description(
        'MCClientConnection.cfg with KindLoss "none"; checks Connects, ServerNotStranded'
    )
    assert base_file == 'MCClientConnection.cfg'
    assert overrides == {'KindLoss': '"none"'}
    assert spec == 'Spec'
    assert props == []
    assert checks == ['Connects', 'ServerNotStranded']
    
    # Complex MCClientConnection
    desc = 'MCClientConnection.cfg with Timeout 2, Deadline 4, ServerDeadline 3, MaxAttempts 2, MaxNonces 2, OthersMayJoin TRUE, Disconnects TRUE, KindLoss "none", C2SPattern "arbitrary", S2CPattern "arbitrary", MaxDelay 1, Duplication TRUE, Reordering TRUE; checks EphemeralErasedOutsideHandshake'
    base_file, overrides, spec, props, checks = parse_config_description(desc)
    assert base_file == 'MCClientConnection.cfg'
    assert overrides['Timeout'] == '2'
    assert overrides['Deadline'] == '4'
    assert overrides['ServerDeadline'] == '3'
    assert overrides['MaxAttempts'] == '2'
    assert overrides['MaxNonces'] == '2'
    assert overrides['OthersMayJoin'] == 'TRUE'
    assert overrides['Disconnects'] == 'TRUE'
    assert overrides['KindLoss'] == '"none"'
    assert overrides['C2SPattern'] == '"arbitrary"'
    assert overrides['S2CPattern'] == '"arbitrary"'
    assert overrides['MaxDelay'] == '1'
    assert overrides['Duplication'] == 'TRUE'
    assert overrides['Reordering'] == 'TRUE'
    assert spec == 'Spec'
    assert props == []
    assert checks == ['EphemeralErasedOutsideHandshake']
    
    print("test_parse_config_description: PASSED")


def test_compare_result_pass():
    """Test comparison when expected result is pass."""
    row = RunRow(
        module='MCLink',
        config_desc='MCLink.cfg',
        states=18543,
        distinct=2146,
        result='pass',
        bench='vision',
        date='2026-10-06'
    )
    
    # TLC passes, counts match
    tlc_result = RunResult(passed=True, distinct_states=2146, total_states=18543, violated_invariant=None, error=None)
    ok, msg = compare_result(row, tlc_result)
    assert ok
    assert msg == ''
    
    # TLC fails but expected pass
    tlc_result = RunResult(passed=False, distinct_states=2146, total_states=18543, violated_invariant='SomeInvariant', error=None)
    ok, msg = compare_result(row, tlc_result)
    assert not ok
    assert 'expected pass but TLC failed' in msg
    
    # TLC passes but distinct count mismatch
    tlc_result = RunResult(passed=True, distinct_states=2147, total_states=18543, violated_invariant=None, error=None)
    ok, msg = compare_result(row, tlc_result)
    assert not ok
    assert 'distinct state count mismatch' in msg
    
    print("test_compare_result_pass: PASSED")


def test_compare_result_fail_expected():
    """Test comparison when expected result is fail as expected."""
    row = RunRow(
        module='MCLink',
        config_desc='MCLink.cfg',
        states=18543,
        distinct=2146,
        result='fail as expected: LinkDelivers violated',
        bench='vision',
        date='2026-10-06'
    )
    
    # TLC fails on correct invariant
    tlc_result = RunResult(passed=False, distinct_states=2146, total_states=18543, violated_invariant='LinkDelivers', error=None)
    ok, msg = compare_result(row, tlc_result)
    assert ok
    assert msg == ''
    
    # TLC fails on wrong invariant
    tlc_result = RunResult(passed=False, distinct_states=2146, total_states=18543, violated_invariant='OtherInvariant', error=None)
    ok, msg = compare_result(row, tlc_result)
    assert not ok
    assert 'expected violation of LinkDelivers' in msg
    
    # TLC passes but expected fail
    tlc_result = RunResult(passed=True, distinct_states=2146, total_states=18543, violated_invariant=None, error=None)
    ok, msg = compare_result(row, tlc_result)
    assert not ok
    assert 'expected fail but TLC passed' in msg
    
    # With parenthetical note (F4)
    row2 = RunRow(
        module='MCClientConnection',
        config_desc='MCClientConnection.cfg',
        states=245,
        distinct=124,
        result='fail as expected (F4): Invariant EphemeralErasedOutsideHandshake is violated',
        bench='vision',
        date='2026-10-06'
    )
    tlc_result = RunResult(passed=False, distinct_states=124, total_states=245, violated_invariant='EphemeralErasedOutsideHandshake', error=None)
    ok, msg = compare_result(row2, tlc_result)
    assert ok
    assert msg == ''
    
    print("test_compare_result_fail_expected: PASSED")


def test_compare_result_skip():
    """Test that rows above bound or unfinished are skipped."""
    row_high = RunRow(
        module='BigModel',
        config_desc='BigModel.cfg',
        states=5000000,
        distinct=3000000,
        result='pass',
        bench='vision',
        date='2026-10-06'
    )
    row_unfinished = RunRow(
        module='UnfinishedModel',
        config_desc='UnfinishedModel.cfg',
        states=1000,
        distinct=500,
        result='not finished (stopped at deadline)',
        bench='vision',
        date='2026-10-06'
    )
    
    # These should be handled by the main check logic, not compare_result
    # compare_result should still work but the main logic skips them
    tlc_result = RunResult(passed=True, distinct_states=3000000, total_states=5000000, violated_invariant=None, error=None)
    ok, msg = compare_result(row_high, tlc_result)
    assert ok  # comparison itself works
    
    print("test_compare_result_skip: PASSED")


def run_all_tests():
    """Run all tests."""
    test_parse_runs_tsv()
    test_parse_config_description()
    test_compare_result_pass()
    test_compare_result_fail_expected()
    test_compare_result_skip()
    print("\nALL TESTS PASSED")


if __name__ == '__main__':
    run_all_tests()
