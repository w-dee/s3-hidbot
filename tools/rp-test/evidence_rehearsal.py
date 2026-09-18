#!/usr/bin/env python3
"""Explicit nonqualification rehearsal for the privileged HCI evidence path."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from common import InfraError, need, no_links
from evidence_pipeline import (
    CLASSIFICATION,
    create_package,
    finalize_terminal_package,
    invoke_privileged_capture,
    tree_digest,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence-pipeline-rehearsal', action='store_true')
    parser.add_argument('--not-qualification', action='store_true')
    parser.add_argument('--base', type=Path, default=Path('/srv/s3-hidbot-test/private/evidence-pipeline-rehearsals'))
    parser.add_argument('--duration', type=float, default=2.0)
    args = parser.parse_args()
    need(args.evidence_pipeline_rehearsal and args.not_qualification, 'REHEARSAL_OPT_IN_REQUIRED')
    base = no_links(args.base)
    need(base == Path('/srv/s3-hidbot-test/private/evidence-pipeline-rehearsals'), 'REHEARSAL_BASE_INVALID')
    path, token = create_package(base, CLASSIFICATION)
    try:
        receipt = invoke_privileged_capture(
            Path(__file__).with_name('privileged_evidence.py'), path,
            capture_token=token, duration=args.duration,
        )
        package = finalize_terminal_package(
            path, classification=CLASSIFICATION, test_outcome='PASS',
            capture_token=token, receipt=receipt,
        )
    except Exception:
        finalize_terminal_package(
            path, classification=CLASSIFICATION, test_outcome='FAIL',
            capture_token=token, evidence_error='CAPTURE_HELPER_FAILED',
        )
        raise
    print(json.dumps({
        'schema': 1,
        'classification': CLASSIFICATION,
        'package_id': path.name,
        'package_digest': tree_digest(path),
        'evidence': package['manifest']['evidence'],
        'runner_raw_metadata_mutations': 0,
    }, sort_keys=True, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        code = str(exc) if isinstance(exc, InfraError) else 'EVIDENCE_REHEARSAL_FAILED'
        print(json.dumps({'schema': 1, 'error': code}), file=sys.stderr)
        sys.exit(1)
