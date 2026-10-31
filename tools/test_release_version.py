#!/usr/bin/env python3
"""Exercise the actual CMake version block and release workflow with fixtures.

Run on a Linux bench: python3 tools/test_release_version.py
Needs cmake, bash and git; no compiler, library build, network, or third-party
Python packages.
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
drift_start = workflow.index('          newest_tag=$(git tag --list')
drift_end = workflow.index('\n', workflow.index('echo "::warning::HEAD carries', drift_start))
drift = 'set -euo pipefail\n' + textwrap.dedent(workflow[drift_start:drift_end])
def fixture(full, numeric='1.0.0', dev_full='dev', dev_numeric='255.255.255'):
    # Final public API deliberately has no VERSION_PRERELEASE macro.
    lines = ['#if !ROCKETNET_DEV']
    for label, numbers in [(full, numeric), (dev_full, dev_numeric)]:
        if len(lines) > 1:
            lines.append('#else // !ROCKETNET_DEV')
        lines.append('#define ROCKETNET_VERSION_FULL "' + label + '"')
        for part, value in zip(('MAJOR', 'MINOR', 'PATCH'), numbers.split('.')):
            lines.append('#define ROCKETNET_VERSION_' + part + ' ' + value)
    lines.append('#endif // !ROCKETNET_DEV')
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

# The unreleased-wire-changes job, run against a scratch repository: a commit on
# a wire path past a tag with the shipping label unbumped fails; the same
# history with the label bumped passes with a warning; a commit off the wire
# paths passes; a checkout without tags fails.
with tempfile.TemporaryDirectory(prefix='rocketnet-drift-') as tmp:
    root = Path(tmp)
    env = dict(os.environ, GIT_AUTHOR_NAME='fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
               GIT_COMMITTER_NAME='fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
               GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1',
               GITHUB_STEP_SUMMARY=str(root / 'summary.md'))
    repo = root / 'repo'
    (repo / 'sdk').mkdir(parents=True)
    (repo / 'standard').mkdir()

    def git(*args):
        subprocess.run(['git', *args], cwd=repo, env=env, check=True, capture_output=True)

    def commit(path, text, message):
        (repo / path).write_text(text)
        git('add', path)
        git('commit', '-q', '-m', message)

    def drift_check(okay, expect):
        result = subprocess.run(['bash', '-c', drift], cwd=repo, env=env, text=True, capture_output=True)
        if (result.returncode == 0) != okay or expect not in result.stdout:
            raise AssertionError(f'drift check: exit {result.returncode}, expected {expect!r}\n{result.stdout}\n{result.stderr}')

    git('init', '-q', '-b', 'main')
    commit('sdk/rocketnet.h', fixture('v1.0.0-preview-1'), 'release v1.0.0-preview-1')
    drift_check(False, '::error::No v* tag is visible')
    print('Drift: no tag rejected')
    git('tag', 'v1.0.0-preview-1')
    drift_check(True, 'HEAD is exactly at v1.0.0-preview-1')
    print('Drift: at the tag accepted')
    commit('README.md', 'notes\n', 'not a wire path')
    drift_check(True, 'none of them wire-affecting')
    print('Drift: non-wire commit past the tag accepted')
    commit('sdk/rocketnet_protocol.cpp', '// one line\n', 'touch the protocol layer')
    drift_check(False, '::error::HEAD carries 1 wire-affecting commit(s) past v1.0.0-preview-1 and the shipping version is still v1.0.0-preview-1')
    print('Drift: wire commit with the label unbumped rejected')
    commit('sdk/rocketnet.h', fixture('v1.0.0-preview-2'), 'bump to v1.0.0-preview-2')
    drift_check(True, '::warning::HEAD carries 1 wire-affecting commit(s) past v1.0.0-preview-1. The version is already bumped to v1.0.0-preview-2')
    print('Drift: wire commit with the label bumped accepted with a warning')
print('24 release version fixtures passed')
