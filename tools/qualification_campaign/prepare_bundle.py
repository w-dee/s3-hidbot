#!/usr/bin/env python3
"""Copy the previously audited phase source, overlay maintained boundary, freeze."""
import argparse
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'rp-test'))
import evidence_contract as c
from evidence_pipeline import code_authority


def prepare(source, destination, plan):
    c.need(not destination.exists(), 'DESTINATION_EXISTS')
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pyo',
                                                                    'FROZEN.json', 'RUNNER_DIGEST', 'TOOLSHA256SUMS'))
    for name in ('official_campaign.py', 'q8_host_security.py', 'q8_capture.py'):
        shutil.copyfile(HERE / name, destination / name)
    for name in ('evidence_pipeline.py', 'evidence_contract.py', 'privileged_evidence.py', 'evidence_rehearsal.py', 'install_evidence_helper.py'):
        shutil.copyfile(HERE.parent / 'rp-test' / name, destination / name)
    shutil.copyfile(plan, destination / 'qualification-plan.md')
    authority = code_authority(destination, destination / 'qualification-plan.md')
    (destination / 'FROZEN.json').write_bytes(c.encode(authority))
    print(c.encode(authority).decode(), end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--plan', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.destination, args.plan)
