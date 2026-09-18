#!/usr/bin/env python3
"""Passive, opt-in NOT_QUALIFICATION using the exact Q8/coordinator authority chain."""
import argparse
import sys
import time
import evidence_contract as c
from evidence_pipeline import CLASSIFICATION, create_package, package_request, verify_attempt
from official_campaign import finish
from q8_capture import capture_pair


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence-pipeline-rehearsal', action='store_true')
    parser.add_argument('--not-qualification', action='store_true')
    args = parser.parse_args()
    c.need(args.evidence_pipeline_rehearsal and args.not_qualification, 'REHEARSAL_OPT_IN_REQUIRED')
    operations = []
    def audit(event, args):
        if event in ('open','os.chmod','os.chown','os.rename','os.remove','os.truncate') and any(
            isinstance(arg,str) and ('raw-hci' in arg or '/captures/' in arg) for arg in args):
            operations.append(event); raise c.EvidenceError('ORDINARY_RAW_OPERATION')
    sys.addaudithook(audit)
    handle = create_package(CLASSIFICATION)
    receipt = None; passed = False
    try:
        receipt = capture_pair(package_request(handle), lambda: time.sleep(2), require_pairing=False)
        verify_attempt(handle); passed = True
    finally:
        terminal = finish(handle, passed, {'classification':CLASSIFICATION})
    c.need(terminal['code'] == 'SUCCESS', 'REHEARSAL_FAILED')
    c.need(finish(handle,passed,{'classification':CLASSIFICATION}) == terminal, 'REVERIFY_FAILED')
    print(c.encode({'schema':3,'classification':CLASSIFICATION,'handle':handle,
                    'terminal':terminal,'receipt':receipt,'ordinary_raw_operations':operations}).decode(), end='')

if __name__ == '__main__': main()
