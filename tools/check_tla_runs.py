#!/usr/bin/env python3
"""
CI check that re-runs committed TLA+ models and compares results with RUNS.tsv.
"""
import sys
import os
import re
import subprocess
import tempfile
from pathlib import Path
from dataclasses import dataclass
from typing import Optional, List, Dict, Tuple


@dataclass
class RunRow:
    module: str
    config_desc: str
    states: int
    distinct: int
    result: str
    bench: str
    date: str


@dataclass
class RunResult:
    passed: bool
    distinct_states: int
    total_states: int
    violated_invariant: Optional[str]
    error: Optional[str]


MAX_DISTINCT = 2_000_000
TLC_TIMEOUT_SECONDS = 600  # 10 minutes


def parse_runs_tsv(path: str) -> List[RunRow]:
    """Parse RUNS.tsv file into list of RunRow objects."""
    rows = []
    with open(path, 'r') as f:
        lines = f.readlines()
    
    if not lines:
        return rows
    
    # Skip header
    for line in lines[1:]:
        line = line.strip()
        if not line:
            continue
        parts = line.split('\t')
        if len(parts) != 7:
            print(f"WARNING: Skipping malformed line: {line}", file=sys.stderr)
            continue
        module, config_desc, states, distinct, result, bench, date = parts
        rows.append(RunRow(
            module=module,
            config_desc=config_desc,
            states=int(states),
            distinct=int(distinct),
            result=result,
            bench=bench,
            date=date
        ))
    return rows


def parse_config_description(desc: str) -> Tuple[str, Dict[str, str], str, List[str], List[str]]:
    """
    Parse a config description string.
    Returns: (base_config_file, overrides_dict, specification, properties_list, checks_list)
    """
    # Split on ';' to separate config from checks
    if ';' in desc:
        config_part, checks_part = desc.split(';', 1)
        checks = [c.strip() for c in checks_part.replace('checks', '').strip().split(',') if c.strip()]
    else:
        config_part = desc
        checks = []
    
    config_part = config_part.strip()
    
    # Extract base config file name
    base_match = re.match(r'^(\S+\.cfg)', config_part)
    if not base_match:
        raise ValueError(f"Cannot parse config file from: {desc}")
    base_file = base_match.group(1)
    
    # Extract overrides from parentheses or "with" clause
    overrides = {}
    specification = 'Spec'
    properties = []
    
    # Look for "with" clause first (it may have trailing parentheses)
    with_match = re.search(r'with\s+(.+)$', config_part)
    if with_match:
        override_str = with_match.group(1)
        # Remove trailing parenthetical note like "(reversed witness)" or "(reorder starvation)"
        override_str = re.sub(r'\s*\([^)]+\)\s*$', '', override_str)
    else:
        # Look for parenthetical overrides: (Pattern "arbitrary", Spec, ResentIsDelivered)
        paren_match = re.search(r'\(([^)]+)\)', config_part)
        if paren_match:
            override_str = paren_match.group(1)
        else:
            override_str = ''
    
    if override_str:
        # Split by comma, but respect quoted strings
        parts = []
        current = ''
        in_quotes = False
        for char in override_str:
            if char == '"' and not in_quotes:
                in_quotes = True
                current += char
            elif char == '"' and in_quotes:
                in_quotes = False
                current += char
            elif char == ',' and not in_quotes:
                parts.append(current.strip())
                current = ''
            else:
                current += char
        if current:
            parts.append(current.strip())
        
        for part in parts:
            part = part.strip()
            if not part:
                continue
            # Check for key "value" pattern (quoted)
            kv_match = re.match(r'^(\w+)\s+"([^"]+)"$', part)
            if kv_match:
                overrides[kv_match.group(1)] = f'"{kv_match.group(2)}"'
                continue
            # Check for key value pattern (unquoted)
            kv_match = re.match(r'^(\w+)\s+(\S+)$', part)
            if kv_match:
                key = kv_match.group(1)
                value = kv_match.group(2)
                # Don't treat specification/property keywords as overrides
                if key not in ('Spec', 'SpecWireFair', 'SpecTyped') and not key.startswith('LinkDelivers') and not key.startswith('ResentIsDelivered') and not key.startswith('EphemeralErasedOutsideHandshake') and not key.startswith('TerminalAbsorbing') and not key.startswith('KeysErasedWhenDown') and not key.startswith('Connects') and not key.startswith('ServerNotStranded') and not key.startswith('StepsForwardOrFails') and not key.startswith('DisconnectIsTerminal') and not key.startswith('TimeoutPairing') and not key.startswith('AcceptedUnderLiveKeys') and not key.startswith('NoSendOnArrival') and not key.startswith('SilentServerTimesOut') and not key.startswith('AttemptResolves') and not key.startswith('CrashedServerTimesOut') and key != 'safety' and key != 'only':
                    overrides[key] = value
                continue
            elif part in ('Spec', 'SpecWireFair', 'SpecTyped'):
                specification = part
            elif part.startswith('LinkDelivers') or part.startswith('ResentIsDelivered') or part.startswith('EphemeralErasedOutsideHandshake') or part.startswith('TerminalAbsorbing') or part.startswith('KeysErasedWhenDown') or part.startswith('Connects') or part.startswith('ServerNotStranded') or part.startswith('StepsForwardOrFails') or part.startswith('DisconnectIsTerminal') or part.startswith('TimeoutPairing') or part.startswith('AcceptedUnderLiveKeys') or part.startswith('NoSendOnArrival') or part.startswith('SilentServerTimesOut') or part.startswith('AttemptResolves') or part.startswith('CrashedServerTimesOut') or part.startswith('safety only'):
                properties.append(part)
            elif part == 'TRUE' or part == 'FALSE':
                # Boolean constant, handled as key=value
                pass
            else:
                # Might be a bare constant like "safety only" or something else
                pass
    
    return base_file, overrides, specification, properties, checks


def apply_config_overrides(base_config_path: Path, overrides: Dict[str, str], specification: str, properties: List[str]) -> Path:
    """Create a temporary config file with overrides applied."""
    with open(base_config_path, 'r') as f:
        content = f.read()
    
    # Replace SPECIFICATION
    content = re.sub(r'^SPECIFICATION\s+\w+', f'SPECIFICATION {specification}', content, flags=re.MULTILINE)
    
    # Replace CONSTANTS - match key with leading whitespace
    for key, value in overrides.items():
        # Match CONSTANT key = value (with any leading whitespace)
        pattern = rf'^\s*{re.escape(key)}\s*=\s*[^\n]+'
        replacement = f'    {key}     = {value}'
        content = re.sub(pattern, replacement, content, flags=re.MULTILINE)
    
    # Replace PROPERTIES
    if properties:
        props_text = '\n    '.join(properties)
        # Match PROPERTIES section - from PROPERTIES line to next section or end
        content = re.sub(r'^PROPERTIES\n(?:^\s+\w+\n)*', f'PROPERTIES\n    {props_text}\n', content, flags=re.MULTILINE)
    
    # Write to temp file
    tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.cfg', delete=False, dir=base_config_path.parent)
    tmp.write(content)
    tmp.close()
    return Path(tmp.name)


def extract_state_counts(output: str) -> Tuple[int, int]:
    """
    Pull (distinct, total) state counts out of TLC output.

    TLC prints a running line with thousands separators
    ("18,543 states generated, 2,146 distinct states found") before the final
    summary. Strip the separators first so the first match is the whole number,
    not the trailing group after a comma.
    """
    text = output.replace(',', '')
    distinct_match = re.search(r'(\d+)\s+distinct states? found', text)
    total_match = re.search(r'(\d+)\s+states? generated', text)
    distinct_states = int(distinct_match.group(1)) if distinct_match else 0
    total_states = int(total_match.group(1)) if total_match else 0
    return distinct_states, total_states


def run_tlc(module: str, config_path: Path, tla_dir: Path, tla2tools_jar: Path) -> RunResult:
    """Run TLC and return the result."""
    try:
        cmd = [
            'java', '-cp', str(tla2tools_jar),
            'tlc2.TLC',
            '-config', str(config_path),
            '-nowarning',
            module
        ]
        result = subprocess.run(
            cmd,
            cwd=tla_dir,
            capture_output=True,
            text=True,
            timeout=TLC_TIMEOUT_SECONDS
        )
        
        output = result.stdout + result.stderr
        
        # Parse output for state counts
        distinct_states, total_states = extract_state_counts(output)
        
        # Check for invariant violation
        violated_invariant = None
        if 'Invariant' in output and 'is violated' in output:
            inv_match = re.search(r'Invariant\s+(\w+)\s+is violated', output)
            if inv_match:
                violated_invariant = inv_match.group(1)
        elif 'Action property' in output and 'is violated' in output:
            inv_match = re.search(r'Action property\s+(\w+)\s+is violated', output)
            if inv_match:
                violated_invariant = inv_match.group(1)
        elif 'Temporal properties were violated' in output:
            violated_invariant = 'TemporalProperties'
        
        passed = result.returncode == 0 and violated_invariant is None
        
        return RunResult(
            passed=passed,
            distinct_states=distinct_states,
            total_states=total_states,
            violated_invariant=violated_invariant,
            error=None if passed else output[-500:]  # Last 500 chars as error summary
        )
    except subprocess.TimeoutExpired:
        return RunResult(
            passed=False,
            distinct_states=0,
            total_states=0,
            violated_invariant=None,
            error=f'TLC timed out after {TLC_TIMEOUT_SECONDS} seconds'
        )
    except Exception as e:
        return RunResult(
            passed=False,
            distinct_states=0,
            total_states=0,
            violated_invariant=None,
            error=str(e)
        )


def compare_result(row: RunRow, tlc_result: RunResult) -> Tuple[bool, str]:
    """Compare expected result with TLC result."""
    if row.result == 'pass':
        if not tlc_result.passed:
            return False, f'expected pass but TLC failed: {tlc_result.violated_invariant or tlc_result.error}'
        if tlc_result.distinct_states != row.distinct:
            return False, f'distinct state count mismatch: expected {row.distinct}, got {tlc_result.distinct_states}'
        return True, ''
    
    if row.result.startswith('fail as expected'):
        # Extract expected invariant name
        expected_inv = None
        # Format: "fail as expected: LinkDelivers violated"
        inv_match = re.search(r'fail as expected:\s*(\w+)\s+violated', row.result)
        if inv_match:
            expected_inv = inv_match.group(1)
        # Format: "fail as expected: Invariant X is violated"
        elif 'Invariant' in row.result and 'is violated' in row.result:
            inv_match = re.search(r'Invariant\s+(\w+)\s+is violated', row.result)
            if inv_match:
                expected_inv = inv_match.group(1)
        # Format: "fail as expected: Action property X is violated"
        elif 'Action property' in row.result and 'is violated' in row.result:
            inv_match = re.search(r'Action property\s+(\w+)\s+is violated', row.result)
            if inv_match:
                expected_inv = inv_match.group(1)
        elif 'Temporal properties were violated' in row.result:
            expected_inv = 'TemporalProperties'
        else:
            # Try to extract from parentheses like (F4): Invariant X is violated
            inv_match = re.search(r'Invariant\s+(\w+)\s+is violated', row.result)
            if inv_match:
                expected_inv = inv_match.group(1)
        
        if not tlc_result.passed:
            if expected_inv and tlc_result.violated_invariant == expected_inv:
                if tlc_result.distinct_states != row.distinct:
                    return False, f'distinct state count mismatch: expected {row.distinct}, got {tlc_result.distinct_states}'
                return True, ''
            elif expected_inv:
                return False, f'expected violation of {expected_inv}, got {tlc_result.violated_invariant}'
            else:
                # No specific invariant named, any failure is ok
                if tlc_result.distinct_states != row.distinct:
                    return False, f'distinct state count mismatch: expected {row.distinct}, got {tlc_result.distinct_states}'
                return True, ''
        else:
            return False, f'expected fail but TLC passed'
    
    # Unfinished or other - should be skipped by caller
    return False, f'unexpected result type: {row.result}'


def run_check(tla_dir, runs_tsv, tla2tools_jar, runner=run_tlc,
              max_distinct=MAX_DISTINCT) -> int:
    """
    Run the check against one tla/ tree and return the process exit code.

    ``runner`` is injected so the unit test drives the whole check with a fake
    TLC (no Java). It has the signature ``runner(module, config_path, tla_dir,
    tla2tools_jar)`` and returns a RunResult.
    """
    tla_dir = Path(tla_dir)
    runs_tsv = Path(runs_tsv)

    if not runs_tsv.exists():
        print(f"ERROR: {runs_tsv} not found", file=sys.stderr)
        return 1

    if tla2tools_jar is None or not Path(tla2tools_jar).exists():
        print(f"ERROR: TLA2TOOLS_JAR not found at {tla2tools_jar}", file=sys.stderr)
        print("Set TLA2TOOLS_JAR environment variable", file=sys.stderr)
        return 1

    rows = parse_runs_tsv(str(runs_tsv))

    parsed = []
    for row in rows:
        try:
            base_file, overrides, specification, properties, checks = parse_config_description(row.config_desc)
        except ValueError as exc:
            print(f"ERROR: cannot parse config description for {row.module}: {exc}", file=sys.stderr)
            return 1
        parsed.append((row, base_file, overrides, specification, properties))

    # A row that names a config file which is not committed is a dangling
    # record: the tree and the table disagree, so fail.
    for row, base_file, _, _, _ in parsed:
        if not (tla_dir / base_file).exists():
            print(f"ERROR: Row references config {base_file} which does not exist", file=sys.stderr)
            return 1

    referenced = {base_file for _, base_file, _, _, _ in parsed}

    skipped = []
    # A committed config with no recorded row is an owed run: the models land
    # in parallel and their rows are recorded as owed in tla/README-*.md before
    # they are copied into RUNS.tsv. List every one, never silently, but do not
    # fail while the rows are still owed.
    for cfg in sorted(tla_dir.glob('*.cfg')):
        if cfg.name not in referenced:
            skipped.append((
                cfg.name,
                'no RUNS.tsv row (run not recorded yet; rows owed, see tla/README-*.md)'
            ))

    failed = []
    passed_count = 0

    for row, base_file, overrides, specification, properties in parsed:
        if row.distinct > max_distinct:
            skipped.append((f'{row.module} {row.config_desc}', f'distinct states {row.distinct} > {max_distinct}'))
            continue
        if 'not finished' in row.result.lower() or 'unfinished' in row.result.lower():
            skipped.append((f'{row.module} {row.config_desc}', f'result is {row.result!r}'))
            continue

        base_config_path = tla_dir / base_file
        tmp_config = None
        try:
            tmp_config = apply_config_overrides(base_config_path, overrides, specification, properties)
            tlc_result = runner(row.module, tmp_config, tla_dir, tla2tools_jar)

            ok, msg = compare_result(row, tlc_result)
            if ok:
                passed_count += 1
                print(f"PASS: {row.module} {row.config_desc}")
            else:
                failed.append((row, msg))
                print(f"FAIL: {row.module} {row.config_desc} - {msg}")
        finally:
            if tmp_config and tmp_config.exists():
                tmp_config.unlink()

    # Print skipped
    for name, reason in skipped:
        print(f"SKIPPED: {name} - {reason}")

    if failed:
        print(f"\n{len(failed)} check(s) failed, {passed_count} passed, {len(skipped)} skipped")
        return 1

    print(f"\nAll {passed_count} checks passed, {len(skipped)} skipped")
    return 0


def main():
    repo_root = Path(__file__).parent.parent
    tla_dir = repo_root / 'tla'
    runs_tsv = tla_dir / 'RUNS.tsv'
    tla2tools_jar = os.environ.get('TLA2TOOLS_JAR', '/opt/tla/tla2tools.jar')
    return run_check(tla_dir, runs_tsv, tla2tools_jar)


if __name__ == '__main__':
    sys.exit(main())
