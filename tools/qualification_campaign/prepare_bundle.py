#!/usr/bin/env python3
"""Prepare source-only runtime; this never installs it or starts qualification."""
import argparse
from pathlib import Path
import shutil
import sys
HERE = Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent / 'rp-test'))
import evidence_contract as c
import authority_runtime as a

EXPECTED_BASE_PLAN = 'd18126b4a3418bbb0db24b280faebcca0f854cf9e9d92460d155fdd0e0469f44'
ADDENDUM = HERE / 'qualification-plan-v5-addendum.md'


def prepare(source, destination, plan):
    c.need(not destination.exists(), 'DESTINATION_EXISTS')
    c.need(a.sha(plan.read_bytes()) == EXPECTED_BASE_PLAN, 'BASE_PLAN_AUTHORITY_INVALID')
    shutil.copytree(source,destination,ignore=shutil.ignore_patterns('__pycache__','*.pyc','*.pyo',
        'FROZEN.json','BUNDLE.json','RUNNER_DIGEST','TOOLSHA256SUMS'))
    for name in ('official_campaign.py','official_preflight.py','q8_host_security.py','q8_capture.py'):
        shutil.copyfile(HERE / name, destination / name)
    for name in ('evidence_pipeline.py','evidence_rehearsal.py','evidence_resume.py',*a.ENGINE):
        shutil.copyfile(HERE.parent / 'rp-test' / name,destination / name)
    (destination / 'qualification-plan.md').write_bytes(plan.read_bytes() + ADDENDUM.read_bytes())
    (destination / 'runtime_probe.py').write_text(
        "import json, q8_capture, hidbot.client, hidbot.serial_transport, dbus, gi\n"
        "print(json.dumps({m.__name__: {'file': m.__file__, 'loader': type(m.__loader__).__name__} for m in (q8_capture,hidbot.client,hidbot.serial_transport,dbus,gi)}))\n")
    engine = {name:a.sha((HERE.parent / 'rp-test' / name).read_bytes()) for name in a.ENGINE}
    frozen = {'schema':3, 'product':a.PRODUCT, 'engine':engine,
              'coordinator':a.sha((destination / 'official_campaign.py').read_bytes()),
              'plan':a.sha((destination / 'qualification-plan.md').read_bytes())}
    (destination / 'FROZEN.json').write_bytes(c.encode(frozen))
    rows = a.membership(destination)
    manifest = {'schema':3,'files':rows,'engine':engine,'product':a.PRODUCT,
        'coordinator':rows['official_campaign.py'],'plan':rows['qualification-plan.md'],'frozen':rows['FROZEN.json']}
    (destination / 'BUNDLE.json').write_bytes(c.encode(manifest))
    print(c.digest(manifest))
    return c.digest(manifest)

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,required=True); parser.add_argument('--destination',type=Path,required=True)
    parser.add_argument('--plan',type=Path,required=True)
    args=parser.parse_args(); prepare(args.source,args.destination,args.plan)
