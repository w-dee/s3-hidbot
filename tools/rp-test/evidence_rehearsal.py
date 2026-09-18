#!/usr/bin/env python3
"""Bounded passive capture through the exact future Q8/coordinator boundary."""
import argparse
from pathlib import Path
import time
import sys

import evidence_contract as c
from evidence_pipeline import CLASSIFICATION, create_package, package_request, tree_digest
from official_campaign import finish, frozen_authority
from q8_capture import capture_pair


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence-pipeline-rehearsal', action='store_true')
    parser.add_argument('--not-qualification', action='store_true')
    args = parser.parse_args()
    c.need(args.evidence_pipeline_rehearsal and args.not_qualification, 'REHEARSAL_OPT_IN_REQUIRED')
    raw_operations = []
    def audit(event, args):
        if event in ('open', 'os.chmod', 'os.chown', 'os.rename', 'os.remove', 'os.truncate'):
            if any(isinstance(arg, str) and ('raw-hci' in arg or '/srv/s3-hidbot-test/private/evidence-authority-v2' in arg) for arg in args):
                raw_operations.append(event)
                raise c.EvidenceError('ORDINARY_RAW_OPERATION')
    sys.addaudithook(audit)
    authority = frozen_authority()
    package = create_package(Path('/srv/s3-hidbot-test/private/evidence-pipeline-rehearsals'),
                             CLASSIFICATION, authority)
    receipt = None; passed = False
    try:
        receipt = capture_pair(package_request(package), lambda: time.sleep(2), require_pairing=False)
        frozen_authority(); passed = True
    finally:
        terminal = finish(package, passed, {'classification': CLASSIFICATION})
    c.need(terminal['code'] == 'SUCCESS', 'REHEARSAL_FAILED')
    c.need(finish(package, passed, {'classification': CLASSIFICATION}) == terminal, 'REVERIFY_FAILED')
    print(c.encode({'schema': 2, 'classification': CLASSIFICATION, 'package': str(package),
                    'package_sha256': tree_digest(package), 'terminal': terminal,
                    'receipt': receipt, 'ordinary_raw_operations': raw_operations}).decode(), end='')


if __name__ == '__main__':
    main()
