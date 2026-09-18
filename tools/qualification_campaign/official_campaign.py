#!/usr/bin/env python3
"""Future official coordinator. Preparing a bundle never starts qualification."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import secrets
import subprocess
import sys
import time

import evidence_contract as c
from evidence_pipeline import (create_package, finalize_terminal_package, sha,
                               verify_attempt, phase_command, preflight_command)

EXPECTED_ARCHIVE = '2e6b5a1a20a83e20cfeafa2d10835ddb605f4aff0319225b48e340c856b39a35'
EXPECTED_COMMIT = '137d489d3d1219b203f84633cf8b570d9fe9f19a'
PHASES = [('q1_strict', 'q1_strict.py'), ('q2_just_works', 'q2_mouse.py'),
          ('q3_keyboard', 'q3_keyboard.py'), ('q4_id7', 'q4_id7.py'),
          ('q5_led', 'q5_led.py'), ('q6_metadata', 'q6_metadata.py'),
          ('q7_sleep', 'q7_sleep.py'), ('q8_host_security', 'q8_host_security.py'),
          ('q9_lifecycle', 'q9_lifecycle.py'), ('q10_usb', 'q10_usb.py'), ('q11_cache', 'q11_cache.py')]
ROOT = Path(__file__).resolve().parent


def finish(package, passed, details, **test_seams):
    return finalize_terminal_package(package, test_outcome='PASS' if passed else 'FAIL',
                                     details=details, **test_seams)


def normalized_preflight(artifact, base):
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + secrets.token_hex(6)
    output = base / ('preflight-' + stamp + '.json')
    proc = subprocess.run(preflight_command('official_preflight.py', '--official-preflight',
                          '--artifact', artifact, '--evidence', output),
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        result = c.decode(output.read_bytes())
    except (OSError, c.EvidenceError):
        result = {'result': 'MISSING'}
    c.need(proc.returncode == 0 and result.get('result') == 'PASS'
           and result.get('reset_count') == 1
           and result.get('post_reset', {}).get('profile') == 'strict_composite',
           'OFFICIAL_PREFLIGHT_FAILED')
    return result


def run(artifact, base):
    c.need(sha(artifact) == EXPECTED_ARCHIVE, 'ARTIFACT_AUTHORITY_INVALID')
    preflight = normalized_preflight(artifact, base)
    package = create_package('OFFICIAL_FNK0099_V0_4_0', kind='Q8_HCI', max_seconds=60)
    # Phase outputs are ordinary metadata outside the sealed package. The immutable
    # test journal embeds their full JSON content; root raw is never in this tree.
    outputs = base / (package['run_id'] + '-phase-output'); outputs.mkdir(mode=0o700)
    details = {'source_commit': EXPECTED_COMMIT, 'archive_sha256': EXPECTED_ARCHIVE,
               'preflight': preflight,
               'phases': [{'name': name, 'status': 'NOT EXECUTED'} for name, _ in PHASES]}
    print('QUALIFICATION_ATTEMPT_START=' + package['run_id'], flush=True)
    env = {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', 'S3_ATTEMPT_AUTHORITY_SHA256': package['attempt_authority_sha256']}
    passed = False
    try:
        for index, (name, script) in enumerate(PHASES):
            verify_attempt(package)
            details['phases'][index]['status'] = 'RUNNING'
            out = outputs / (name + '.json')
            proc = subprocess.run(phase_command(package, script, 'official', artifact, out),
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                result = c.decode(out.read_bytes())
            except (OSError, c.EvidenceError):
                result = {'result': 'MISSING'}
            details['phases'][index].update(returncode=proc.returncode, result=result,
                                            status='PASS' if proc.returncode == 0 and result.get('result') == 'PASS' else 'FAIL')
            if proc.returncode != 0 or result.get('result') != 'PASS':
                break
        else:
            passed = True
    except (OSError, c.EvidenceError):
        details['coordinator_error'] = 'RUNNER_FAILED'
    finally:
        final_path = outputs / 'final-state.json'
        final = subprocess.run(phase_command(package, 'safe_finalize.py', final_path),
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            result = c.decode(final_path.read_bytes())
        except (OSError, c.EvidenceError):
            result = {'result': 'MISSING'}
        details['final_state'] = result
        passed = passed and final.returncode == 0 and result.get('result') == 'PASS'
        try:
            verify_attempt(package)
        except c.EvidenceError:
            passed = False; details['coordinator_error'] = 'RUNNER_AUTHORITY_CHANGED'
        terminal = finish(package, passed, details)
        print(c.encode(terminal).decode(), end='', flush=True)
    return 0 if terminal['code'] == 'SUCCESS' else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--official-qualification', action='store_true')
    parser.add_argument('--artifact', type=Path, required=True)
    parser.add_argument('--capsule-root', type=Path, required=True)
    args = parser.parse_args()
    c.need(args.official_qualification, 'OFFICIAL_OPT_IN_REQUIRED')
    return run(args.artifact, args.capsule_root)


if __name__ == '__main__':
    raise SystemExit(main())
