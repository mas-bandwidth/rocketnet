#!/usr/bin/env python3
"""Exercise the actual CMake version block and release workflow with fixtures.

Run on a Linux bench: python3 tools/test_release_version.py
No compiler, library build, network, or third-party Python packages required.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap

ROOT = Path(__file__).resolve().parents[1]
cmake = (ROOT / 'CMakeLists.txt').read_text()
block = cmake[cmake.index('file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/rocketnet.h" ROCKETNET_VERSION_FULL_LINE'):cmake.index('if(MSVC)')]
workflow = (ROOT / '.github/workflows/release-check.yml').read_text()
start = workflow.index('          set -euo pipefail')
end = workflow.index('          exit $fail', start) + len('          exit $fail')
check = textwrap.dedent(workflow[start:end])
extract = next(line.strip() for line in workflow.splitlines() if line.strip().startswith('version=$(sed '))
def fixture(full, numeric='1.0.0', dev_full='dev', dev_numeric='255.255.255'):
    # Final public API deliberately has no VERSION_PRERELEASE macro.
    lines = ['#if !ROCKETNET_DEVELOPMENT']
    for label, numbers in [(full, numeric), (dev_full, dev_numeric)]:
        if len(lines) > 1:
            lines.append('#else // !ROCKETNET_DEVELOPMENT')
        lines.append('#define ROCKETNET_VERSION_FULL "' + label + '"')
        for part, value in zip(('MAJOR', 'MINOR', 'PATCH'), numbers.split('.')):
            lines.append('#define ROCKETNET_VERSION_' + part + ' ' + value)
    lines.append('#endif // !ROCKETNET_DEVELOPMENT')
    return '\n'.join(lines) + '\n'


def run(command, cwd, okay, **kwargs):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, **kwargs)
    if (result.returncode == 0) != okay:
        raise AssertionError(f'{command}: unexpected exit {result.returncode}\n{result.stdout}\n{result.stderr}')


with tempfile.TemporaryDirectory(prefix='rocketnet-version-') as tmp:
    root = Path(tmp)
    (root / 'sdk').mkdir()
    script = root / 'version.cmake'
    for full, prerelease, okay in [
        ('v1.0.0-preview-1', 'preview-1', True),
        ('v1.0.0', '', True),
        ('1.0.0-preview-1', 'preview-1', False),
        ('vv1.0.0-preview-1', 'preview-1', False),
        ('v2.0.0-preview-1', 'preview-1', False),
        ('v1.0.0-', '', False),
    ]:
        (root / 'sdk/rocketnet.h').write_text(fixture(full))
        script.write_text('cmake_minimum_required(VERSION 3.20)\nset(PROJECT_VERSION "1.0.0")\n' + block +
                          f'\nif(NOT ROCKETNET_VERSION_STRING STREQUAL "{full}" OR NOT ROCKETNET_VERSION_PRERELEASE STREQUAL "{prerelease}")\n'
                          'message(FATAL_ERROR "Exported label or prerelease changed")\nendif()\n')
        run(['cmake', '-P', str(script)], root, okay)
        print('CMake:', full, 'accepted' if okay else 'rejected')
    for full, numeric, dev_full, dev_numeric, tag, okay in [
        ('v1.0.0-preview-1', '1.0.0', 'dev', '255.255.255', 'v1.0.0-preview-1', True),
        ('v1.0.0-preview-1', '1.0.0', 'dev', '255.255.255', 'vv1.0.0-preview-1', False),
        ('v1.0.0-preview-1', '1.0.0', 'dev', '255.255.255', 'v1.0.0-preview-2', False),
        ('1.0.0-preview-1', '1.0.0', 'dev', '255.255.255', 'v1.0.0-preview-1', False),
        ('v1.0.0-preview-1', '1.0.1', 'dev', '255.255.255', 'v1.0.0-preview-1', False),
        ('v1.0.0-', '1.0.0', 'dev', '255.255.255', 'v1.0.0-', False),
        ('v1.0.0', '1.0.0', 'dev', '255.255.255', 'v1.0.0', True),
        ('v1.0.0', '1.0.0', 'dev', '1.0.0', 'v1.0.0', False),
        ('v1.0.0', '1.0.0', 'v255.255.255', '255.255.255', 'v1.0.0', False),
        ('dev', '255.255.255', 'dev', '255.255.255', 'dev', False),
    ]:
        (root / 'sdk/rocketnet.h').write_text(fixture(full, numeric, dev_full, dev_numeric))
        env = dict(os.environ, GITHUB_REF='refs/tags/' + tag, GITHUB_REF_NAME=tag)
        run(['bash', '-c', check], root, okay, env=env)
        print('Workflow:', full, numeric, dev_full, dev_numeric, tag, 'accepted' if okay else 'rejected')
    for full, expected in [('v1.0.0-preview-1', 'v1.0.0-preview-1'),
                           ('1.0.0-preview-1', ''), ('vv1.0.0-preview-1', '')]:
        (root / 'sdk/rocketnet.h').write_text(fixture(full))
        result = subprocess.run(['bash', '-c', extract + '\nprintf \"%s\" \"$version\"'],
                                cwd=root, text=True, capture_output=True, check=True)
        assert result.stdout == expected, (full, result.stdout)
        print('Drift extraction:', full, repr(expected))
print('19 release version fixtures passed')
