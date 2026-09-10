"""Default host-only doctor: no device enumeration, D-Bus or network calls."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import time

from common import ROOT, LAYOUT, MIN_FREE, InfraError, check_mode, cli_error, lock, need, read_json, verify_marker
from bootstrap import compatible, host_facts, stamp_value, runtime_matches

EXECUTABLES = ('python3', 'rsync', 'scp', 'git', 'sha256sum', 'lsusb', 'udevadm', 'bluetoothctl', 'btmon', 'busctl', 'flock')

def host_checks(root=ROOT, marker_check=verify_marker, facts=host_facts, disk=shutil.disk_usage):
    marker_check()
    need(compatible(facts()), 'HOST_INCOMPATIBLE')
    for sub in ('', *LAYOUT):
        check_mode(Path(root) / sub, 0o700)
    need(all(shutil.which(name) for name in EXECUTABLES), 'EXECUTABLE_MISSING')
    need(disk(root).free >= MIN_FREE, 'FORENSIC_STORAGE_LOW')
    locked = False
    try:
        with lock(root, 'physical'):
            pass
    except InfraError as exc:
        if str(exc) != 'PHYSICAL_RUN_LOCKED':
            raise
        locked = True
    return {'physical_run_locked': locked, 'minimum_free_bytes': MIN_FREE}

def toolchain_check(root=ROOT, current=stamp_value):
    path = Path(root) / 'state/toolchain.json'
    try:
        check_mode(path, 0o600)
        old = read_json(path)
        observed = current(root)
        need(old == observed and runtime_matches(observed['runtime']), 'TOOLCHAIN_CACHE_STALE')
    except (OSError, ValueError, InfraError):
        raise InfraError('TOOLCHAIN_CACHE_STALE') from None
    return {'esptool': observed['runtime']['esptool'], 'python': observed['runtime']['python']}

def doctor(root=ROOT, host=host_checks, toolchain=toolchain_check):
    started = time.monotonic()
    result = {'schema': 1, 'ok': False, 'classification': 'HOST_DOCTOR_FAILED',
              'hardware_operations': 0, 'physical_run_locked': None, 'timing_ms': {}}
    try:
        t = time.monotonic()
        result.update(host(root))
        result['timing_ms']['host_doctor'] = round((time.monotonic() - t) * 1000, 3)
        t = time.monotonic()
        result['toolchain'] = toolchain(root)
        result['timing_ms']['bootstrap_stamp'] = round((time.monotonic() - t) * 1000, 3)
        result.update(ok=True, classification='HOST_DOCTOR_READY')
    except InfraError as exc:
        result['classification'] = str(exc)
    except Exception:
        result['classification'] = 'COLD_PREPARATION_REQUIRED'
    result['timing_ms']['total'] = round((time.monotonic() - started) * 1000, 3)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    result = doctor()
    print(json.dumps(result) if args.json else f"{result['classification']}; PHYSICAL_RUN_LOCKED={result['physical_run_locked']}; {result['timing_ms']['total']} ms")
    return 0 if result['ok'] else 2

if __name__ == '__main__':
    sys.exit(main())
