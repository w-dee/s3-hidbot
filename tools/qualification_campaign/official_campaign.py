#!/usr/bin/env python3
"""Future official coordinator. Preparing a bundle never starts qualification."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

import evidence_contract as c
from evidence_pipeline import code_authority, create_package, finalize_terminal_package, sha

EXPECTED_ARCHIVE = 'db7c9afec9ba2a6ec210ebffc030ac61542079d7e4a29db5d026d3c542e3ece9'
EXPECTED_COMMIT = '8ca6a1e0ce9eea88ec15a716fffa89ffeff0bfad'
PHASES = [('q1_strict', 'q1_strict.py'), ('q2_just_works', 'q2_mouse.py'),
          ('q3_keyboard', 'q3_keyboard.py'), ('q4_id7', 'q4_id7.py'),
          ('q5_led', 'q5_led.py'), ('q6_metadata', 'q6_metadata.py'),
          ('q7_sleep', 'q7_sleep.py'), ('q8_host_security', 'q8_host_security.py'),
          ('q9_lifecycle', 'q9_lifecycle.py'), ('q10_usb', 'q10_usb.py'), ('q11_cache', 'q11_cache.py')]
ROOT = Path(__file__).resolve().parent


def frozen_authority(root=ROOT):
    expected = c.decode((root / 'FROZEN.json').read_bytes())
    c.need(code_authority(root, root / 'qualification-plan.md') == expected, 'RUNNER_AUTHORITY_CHANGED')
    return expected


def finish(package, passed, details, **test_seams):
    return finalize_terminal_package(package, test_outcome='PASS' if passed else 'FAIL',
                                     details=details, **test_seams)


def run(artifact, base):
    c.need(sha(artifact) == EXPECTED_ARCHIVE, 'ARTIFACT_AUTHORITY_INVALID')
    authority = frozen_authority()
    package = create_package(base, 'OFFICIAL_FNK0099_V0_4_0', authority, kind='Q8_HCI', max_seconds=60)
    # Phase outputs are ordinary metadata outside the sealed package. The immutable
    # test journal embeds their full JSON content; root raw is never in this tree.
    outputs = base / (package.name + '-phase-output'); outputs.mkdir(mode=0o700)
    details = {'source_commit': EXPECTED_COMMIT, 'archive_sha256': EXPECTED_ARCHIVE, 'phases': []}
    print('QUALIFICATION_ATTEMPT_START=' + package.name, flush=True)
    env = {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1', 'S3_EVIDENCE_PACKAGE': str(package)}
    passed = False
    try:
        for name, script in PHASES:
            frozen_authority()
            out = outputs / (name + '.json')
            proc = subprocess.run([sys.executable, str(ROOT / script), 'official', str(artifact), str(out)],
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                result = c.decode(out.read_bytes())
            except (OSError, c.EvidenceError):
                result = {'result': 'MISSING'}
            details['phases'].append({'name': name, 'returncode': proc.returncode, 'result': result})
            if proc.returncode != 0 or result.get('result') != 'PASS':
                break
        else:
            passed = True
    except (OSError, c.EvidenceError):
        details['coordinator_error'] = 'RUNNER_FAILED'
    finally:
        final_path = outputs / 'final-state.json'
        final = subprocess.run([sys.executable, str(ROOT / 'safe_finalize.py'), str(final_path)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            result = c.decode(final_path.read_bytes())
        except (OSError, c.EvidenceError):
            result = {'result': 'MISSING'}
        details['final_state'] = result
        passed = passed and final.returncode == 0 and result.get('result') == 'PASS'
        try:
            frozen_authority()
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
