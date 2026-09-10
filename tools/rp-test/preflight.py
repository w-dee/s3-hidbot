"""Warm profiles only. Installs and physical adapters are intentionally absent."""
import argparse
import json
from pathlib import Path
import sys
import time

from common import ROOT, InfraError, cli_error, need, no_links, read_json
from cache import destination, verify
from doctor import doctor

PROFILES = Path(__file__).with_name('profiles.json')
ADAPTER_MODULES = ('hidbot/__init__.py', 'hidbot/client.py', 'hidbot/errors.py',
                   'hidbot/framing.py', 'hidbot/protocol.py', 'hidbot/serial_transport.py')


def validate_python_adapter(root, definitions, spec):
    """Validate cached source only; never import a module or enumerate serial."""
    value = spec['python_adapter']
    if value is None:
        return 'NOT_REQUIRED'
    need(isinstance(value, list) and len(value) == 2, 'PROFILE_INVALID')
    asset, relative = value
    need(asset in spec['assets'] and relative == 'adapters', 'PROFILE_INVALID')
    kind, digest = definitions['assets'][asset]
    adapter = no_links(destination(root, kind, digest) / 'payload' / relative)
    need(adapter.is_dir(), 'PYTHON_ADAPTER_SOURCE_MISSING')
    for name in ADAPTER_MODULES:
        path = no_links(adapter / name)
        need(path.is_file() and path.stat().st_nlink == 1,
             'PYTHON_ADAPTER_SOURCE_INVALID')
    return 'CACHE_SOURCE_READY'

def latency_warning(seconds, phases):
    return {'preoperation_slow': seconds > 120, 'phase_timing_ms': phases if seconds > 120 else {}}

def preflight(profile, root=ROOT, host_doctor=doctor, cache_verify=verify):
    started = time.monotonic()
    definitions = read_json(PROFILES)
    need(profile in definitions['profiles'], 'PROFILE_INVALID')
    spec = definitions['profiles'][profile]
    timing = dict.fromkeys(('bootstrap_stamp', 'host_doctor', 'artifact_cache', 'tooling_cache', 'private_reference', 'mission_static_checks', 'hardware_checks'), 0.0)
    result = {'schema': 1, 'ok': False, 'profile': profile, 'classification': 'COLD_PREPARATION_REQUIRED',
              'hardware_operations': 0, 'hardware_checks': 'NOT_EXECUTED', 'mission_authorized': False,
              'physical_run_locked': None, 'cache': [],
              'python_adapter_source': 'NOT_CHECKED', 'timing_ms': timing}
    try:
        d = host_doctor(root)
        result['physical_run_locked'] = d['physical_run_locked']
        for name in ('bootstrap_stamp', 'host_doctor'):
            timing[name] = d['timing_ms'].get(name, 0.0)
        if not d['ok']:
            result['reason'] = d['classification']
            raise InfraError('COLD_PREPARATION_REQUIRED' if d['classification'] != 'FORENSIC_STORAGE_LOW' else 'FORENSIC_STORAGE_LOW')
        for asset in spec['assets']:
            kind, digest = definitions['assets'][asset]
            t = time.monotonic()
            phase = {'artifacts': 'artifact_cache', 'tooling': 'tooling_cache', 'references': 'private_reference'}[kind]
            try:
                result['cache'].append(cache_verify(root, kind, digest))
            finally:
                timing[phase] += round((time.monotonic() - t) * 1000, 3)
        t = time.monotonic()
        need(spec['startup_attempts'] in (0, 1, 2), 'PROFILE_INVALID')
        need(spec['opportunistic_probe_seconds'] == 2.0
             and spec['reset_readiness_seconds'] == 12.0
             and spec['start_state'] in ('unknown', 'application_running', 'application_not_running'),
             'PROFILE_INVALID')
        result['python_adapter_source'] = validate_python_adapter(root, definitions, spec)
        timing['mission_static_checks'] = round((time.monotonic() - t) * 1000, 3)
        result.update(ok=True, classification='WARM_PREFLIGHT_READY')
    except InfraError as exc:
        result['classification'] = 'COLD_PREPARATION_REQUIRED' if str(exc) == 'CACHE_MISS' else str(exc)
    except Exception:
        result['classification'] = 'PREFLIGHT_FAILED'
    timing['total'] = round((time.monotonic() - started) * 1000, 3)
    result['warm_preflight_slow'] = timing['total'] > 60000
    result['slowest_phase'] = max((k for k in timing if k != 'total'), key=timing.get)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--profile', default='host-only', choices=tuple(read_json(PROFILES)['profiles']))
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--no-hardware', action='store_true', help='Explicit host-only mode (also the only/default mode).')
    args = parser.parse_args()
    result = preflight(args.profile)
    print(json.dumps(result) if args.json else f"{result['classification']}; {result['profile']}; {result['timing_ms']['total']} ms; hardware=NOT_EXECUTED")
    return 0 if result['ok'] else 2

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        sys.exit(cli_error(exc))
