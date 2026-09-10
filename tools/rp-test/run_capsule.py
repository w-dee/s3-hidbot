"""Private durable forensic capsules. No cleanup path purges evidence."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sys

from common import ROOT, DIGEST, MIN_FREE, TOKEN, InfraError, atomic_json, check_mode, cli_error, fsync_dir, lock, need, no_links, private_dir, read_json, sha, tree_files, verify_marker

RUN_ID = re.compile(r'\d{8}T\d{6}Z-[0-9a-f]{12}\Z')
TRANSITIONS = {'ACTIVE': {'UNRESOLVED', 'RESOLVED'}, 'UNRESOLVED': {'RESOLVED'}, 'RESOLVED': {'PURGE_ELIGIBLE'}, 'PURGE_ELIGIBLE': set()}
RAW = ('HCI', 'UART', 'NVS')

def utc():
    return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')

def run_path(root, run_id):
    need(isinstance(run_id, str) and RUN_ID.fullmatch(run_id), 'RUN_ID_INVALID')
    root = no_links(Path(root) / 'runs')
    path = no_links(root / run_id)
    need(path.parent == root and path.is_dir(), 'RUN_NOT_FOUND')
    check_mode(path, 0o700)
    return path

def hash_tree(path):
    return {str(p.relative_to(path)): sha(p) for p in tree_files(path)}

def tree_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()

def runner_source_allowed(value):
    return all('__pycache__' not in Path(name).parts and not name.endswith(('.pyc', '.pyo')) for name in value)

def sync_tree(path):
    for p in tree_files(path):
        os.chmod(p, 0o600)
        with p.open('rb') as stream:
            os.fsync(stream.fileno())
    for p in reversed([path, *sorted(q for q in path.rglob('*') if q.is_dir())]):
        os.chmod(p, 0o700)
        fsync_dir(p)

def seal_runner_tree(path):
    """Read-only private source; runtime state must use scratch instead."""
    for p in tree_files(path):
        os.chmod(p, 0o400)
        with p.open('rb') as stream:
            os.fsync(stream.fileno())
    for p in reversed([path, *sorted(q for q in path.rglob('*') if q.is_dir())]):
        os.chmod(p, 0o500)
        fsync_dir(p)

def unseal_runner_directories_for_purge(path):
    """Called only after the explicit purge gate validates the entire tree."""
    for p in [path, *sorted((q for q in path.rglob('*') if q.is_dir()), reverse=True)]:
        os.chmod(p, 0o700)

def create(root, profile, runner, digests):
    need(isinstance(profile, str) and TOKEN.fullmatch(profile), 'PROFILE_INVALID')
    need(set(digests) == {'functional', 'firmware', 'parser'} and all(DIGEST.fullmatch(v) for v in digests.values()), 'RUN_DIGESTS_INVALID')
    need(shutil.disk_usage(root).free >= MIN_FREE, 'FORENSIC_STORAGE_LOW')
    source = no_links(runner)
    exact = hash_tree(source)
    need(bool(exact), 'RUNNER_EMPTY')
    need(runner_source_allowed(exact), 'RUNNER_GENERATED_FILE_REFUSED')
    run_id = utc() + '-' + secrets.token_hex(6)
    path = no_links(Path(root) / 'runs' / run_id)
    path.mkdir(mode=0o700)
    # If interrupted, the partial ACTIVE capsule is retained and is unstartable.
    atomic_json(path / 'retention.json', {'schema': 1, 'state': 'ACTIVE'})
    shutil.copytree(source, path / 'runner', symlinks=True)
    sync_tree(path / 'runner')
    need(hash_tree(path / 'runner') == exact, 'RUNNER_SNAPSHOT_MISMATCH')
    seal_runner_tree(path / 'runner')
    for sub in ('evidence', 'checksums', 'scratch'):
        private_dir(path / sub)
    atomic_json(path / 'checksums/runner.json', exact)
    runner_digest = tree_digest(exact)
    manifest = {'schema': 2, 'run_id': run_id, 'profile': profile, 'created': utc(),
                'runner_sha256': runner_digest, 'digests': digests}
    atomic_json(path / 'manifest.json', manifest)
    atomic_json(path / 'ledger.json', {'schema': 1, 'sequence': 0, 'events': []})
    atomic_json(path / 'startup.json', {'schema': 1, 'attempts': []})
    atomic_json(path / 'handoff.json', {'schema': 1, 'state': 'PENDING'})
    fsync_dir(path)
    fsync_dir(path.parent)
    return manifest

def validate_capsule(path):
    check_mode(path, 0o700)
    m = read_json(path / 'manifest.json')
    need(set(m) == {'schema', 'run_id', 'profile', 'created', 'runner_sha256', 'digests'}
         and m['schema'] in (1, 2) and RUN_ID.fullmatch(m['run_id'])
         and isinstance(m['profile'], str) and TOKEN.fullmatch(m['profile'])
         and DIGEST.fullmatch(m['runner_sha256']), 'CAPSULE_SCHEMA_INVALID')
    for p in tree_files(path):
        relative = p.relative_to(path)
        check_mode(p, 0o400 if m['schema'] == 2 and relative.parts[0] == 'runner' else 0o600)
    for p in path.rglob('*'):
        if p.is_dir():
            relative = p.relative_to(path)
            check_mode(p, 0o500 if m['schema'] == 2 and relative.parts[0] == 'runner' else 0o700)
    exact = read_json(path / 'checksums/runner.json')
    need(hash_tree(path / 'runner') == exact, 'RUNNER_SNAPSHOT_MISMATCH')
    need(tree_digest(exact) == m['runner_sha256'], 'RUNNER_SNAPSHOT_MISMATCH')
    retention(path)
    return m

def retention(path):
    value = read_json(path / 'retention.json')
    need(set(value) == {'schema', 'state'} and value['schema'] == 1 and value['state'] in TRANSITIONS, 'RETENTION_INVALID')
    return value['state']

def transition(root, run_id, state):
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        need(state in TRANSITIONS[retention(path)], 'RETENTION_TRANSITION_REFUSED')
        atomic_json(path / 'retention.json', {'schema': 1, 'state': state})

def validate_ledger(value):
    need(set(value) == {'schema', 'sequence', 'events'} and value['schema'] == 1 and type(value['sequence']) is int
         and isinstance(value['events'], list) and value['sequence'] == len(value['events']) <= 10000, 'LEDGER_SCHEMA_INVALID')
    for event in value['events']:
        need(set(event) == {'event', 'operation'} and event['event'] in ('STARTUP_INVOKED', 'SIDE_EFFECT_INVOKED', 'CHECKPOINT')
             and event['operation'] in OPERATIONS, 'LEDGER_SCHEMA_INVALID')

OPERATIONS = {'startup', 'Pair', 'Connect', 'Disconnect', 'ble.bond.remove', 'NVS.mutation', 'flash',
              'route.change', 'pairing.response', 'ble.enable', 'ble.disable', 'identity', 'public', 'persistent', 'cleanup'}

def append_event(root, run_id, event, operation):
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        need(retention(path) == 'ACTIVE', 'RUN_NOT_ACTIVE')
        value = read_json(path / 'ledger.json')
        validate_ledger(value)
        value['sequence'] += 1
        value['events'].append({'event': event, 'operation': operation})
        validate_ledger(value)
        atomic_json(path / 'ledger.json', value)

def validate_startup_attempt(value):
    from serial_readiness import STATES, validate_diagnostics
    need(isinstance(value, dict) and set(value) == {
        'probe', 'reset_attempt', 'deadline_ms', 'classification', 'states',
        'attempts', 'serial',
    }, 'STARTUP_ATTEMPT_INVALID')
    need(value['probe'] in ('opportunistic', 'reset') and type(value['reset_attempt']) is int
         and 0 <= value['reset_attempt'] <= 2 and type(value['deadline_ms']) is int
         and 0 < value['deadline_ms'] <= 60000 and TOKEN.fullmatch(value['classification'])
         and type(value['attempts']) is int and 0 <= value['attempts'] <= 100000
         and isinstance(value['states'], list) and len(value['states']) <= 100000
         and all(state in STATES or state == 'RESET_REQUESTED' for state in value['states']),
         'STARTUP_ATTEMPT_INVALID')
    validate_diagnostics(value['serial'])
    return value

def append_startup_attempt(root, run_id, value):
    """Durably retain only allowlisted serial readiness facts, never endpoints."""
    validate_startup_attempt(value)
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        need(retention(path) == 'ACTIVE', 'RUN_NOT_ACTIVE')
        current = read_json(path / 'startup.json')
        need(set(current) == {'schema', 'attempts'} and current['schema'] == 1
             and isinstance(current['attempts'], list) and len(current['attempts']) < 16,
             'STARTUP_LEDGER_INVALID')
        for existing in current['attempts']:
            validate_startup_attempt(existing)
        current['attempts'].append(value)
        atomic_json(path / 'startup.json', current)

def resume_budget(root, run_id, startup_limit):
    """Called only while holding physical lock; recover committed invocation intent."""
    from startup_probe import OperationBudget
    path = run_path(root, run_id)
    validate_capsule(path)
    value = read_json(path / 'ledger.json')
    validate_ledger(value)
    starts = sum(e['event'] == 'STARTUP_INVOKED' for e in value['events'])
    invoked = {e['operation'] for e in value['events'] if e['event'] == 'SIDE_EFFECT_INVOKED'}
    return OperationBudget(startup_limit, starts, invoked,
        lambda event, operation: append_event(root, run_id, event, operation))

def retain_raw(root, run_id, kind, source):
    """Explicit private ingest. No stdout/stderr is implicitly treated as ledger data."""
    need(kind in RAW, 'RAW_KIND_INVALID')
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        need(retention(path) == 'ACTIVE', 'RUN_NOT_ACTIVE')
        source = no_links(source)
        need(source.is_file(), 'RAW_SOURCE_INVALID')
        destination = no_links(path / 'evidence' / ('raw-' + kind.lower()))
        with source.open('rb') as incoming:
            fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
                outgoing.flush()
                os.fsync(outgoing.fileno())
        need(sha(source) == sha(destination), 'RAW_COPY_CHANGED')
        fsync_dir(destination.parent)
        size = destination.stat().st_size
        return {'RAW_' + kind + '_RETAINED': True,
                'RAW_' + kind + '_FILE_PRESENT': True,
                'RAW_' + kind + '_HAS_DATA': size > 0,
                'RAW_' + kind + '_BYTES': size}

def runner_snapshot_status(path):
    expected = read_json(path / 'checksums/runner.json')
    need(isinstance(expected, dict) and expected and all(DIGEST.fullmatch(value) for value in expected.values()),
         'RUNNER_CHECKSUM_INVALID')
    current = hash_tree(path / 'runner')
    changed = sum(current.get(name) != digest for name, digest in expected.items() if name in current)
    missing = sum(name not in current for name in expected)
    added = sum(name not in expected for name in current)
    mode_mismatches = sum(p.stat().st_mode & 0o777 != 0o400 for p in tree_files(path / 'runner'))
    mode_mismatches += sum(p.stat().st_mode & 0o777 != 0o500
                           for p in [path / 'runner', *(path / 'runner').rglob('*')] if p.is_dir())
    return {'schema': 1, 'exact': not (changed or missing or added or mode_mismatches),
            'expected_digest': tree_digest(expected), 'current_digest': tree_digest(current),
            'expected_files': len(expected), 'current_files': len(current),
            'changed_files': changed, 'missing_files': missing, 'added_files': added,
            'mode_mismatches': mode_mismatches}

def finalize_runner_snapshot(root, run_id):
    path = run_path(root, run_id)
    value = runner_snapshot_status(path)
    atomic_json(path / 'checksums/post-run-runner.json', value)
    return value

@contextlib.contextmanager
def runner_execution(root, run_id):
    """Verified scratch execution copy; immutable snapshot is never imported."""
    path = run_path(root, run_id)
    need(retention(path) == 'ACTIVE', 'RUN_NOT_ACTIVE')
    before = runner_snapshot_status(path)
    need(before['exact'], 'RUNNER_SNAPSHOT_MISMATCH')
    execution = no_links(path / 'scratch/execution')
    need(not execution.exists(), 'RUNNER_EXECUTION_EXISTS')
    shutil.copytree(path / 'runner', execution, symlinks=True)
    sync_tree(execution)
    copied = hash_tree(execution)
    need(tree_digest(copied) == before['expected_digest'], 'RUNNER_EXECUTION_MISMATCH')
    atomic_json(path / 'checksums/execution.json', {
        'schema': 1, 'snapshot_digest': before['expected_digest'],
        'execution_digest': tree_digest(copied), 'matches_snapshot': True,
        'bytecode_disabled': True,
    })
    old_flag = sys.dont_write_bytecode
    old_environment = os.environ.get('PYTHONDONTWRITEBYTECODE')
    sys.dont_write_bytecode = True
    os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
    try:
        yield execution
    finally:
        sys.dont_write_bytecode = old_flag
        if old_environment is None:
            os.environ.pop('PYTHONDONTWRITEBYTECODE', None)
        else:
            os.environ['PYTHONDONTWRITEBYTECODE'] = old_environment
        need(runner_snapshot_status(path)['exact'], 'RUNNER_SNAPSHOT_MUTATED')

def sanitized_exception(exc, stage, substage, allowed_sources=()):
    """Never serialize exception messages, locals, absolute paths or arbitrary symbols."""
    need(stage in ('preflight', 'startup', 'mission', 'cleanup') and substage in ('open', 'query', 'identity', 'operation', 'result'), 'EXCEPTION_CONTEXT_INVALID')
    known_classes = {'InfraError', 'RequestTimeoutError', 'ProtocolError', 'RemoteError', 'OSError', 'TimeoutError', 'ValueError', 'RuntimeError'}
    result = {'exception_class': type(exc).__name__ if type(exc).__name__ in known_classes else 'Exception',
              'module': 'unknown', 'function': 'unknown', 'source': 'unknown', 'line': 0,
              'category': 'INFRA_EXCEPTION', 'stage': stage, 'substage': substage}
    tb = exc.__traceback__
    while tb:
        code = tb.tb_frame.f_code
        basename = Path(code.co_filename).name
        # The registration list comes from reviewed tooling, never exception text.
        if basename in allowed_sources and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*\.py', basename):
            result.update(source=basename, line=tb.tb_lineno, module=basename[:-3])
            if code.co_name in ('main', 'readiness', 'call', 'invoke_once', 'create', 'append_event'):
                result['function'] = code.co_name
        tb = tb.tb_next
    return result

def commit_result(root, run_id, outcome, code, exception=None):
    need(outcome in ('PASS', 'FAIL', 'UNRESOLVED') and code in ('SUCCESS', 'PRECHECK_FAILED', 'PRODUCT_ERROR', 'INFRA_ERROR', 'INTERRUPTED'), 'RESULT_SCHEMA_INVALID')
    if exception is not None:
        need(set(exception) == set(sanitized_exception(RuntimeError(), 'mission', 'operation')), 'EXCEPTION_SCHEMA_INVALID')
        # Only callers passing the sanitizer's output are supported; validate every field.
        need(exception['exception_class'] in ('Exception', 'InfraError', 'RequestTimeoutError', 'ProtocolError', 'RemoteError', 'OSError', 'TimeoutError', 'ValueError', 'RuntimeError')
             and exception['category'] == 'INFRA_EXCEPTION' and type(exception['line']) is int and exception['line'] >= 0
             and exception['stage'] in ('preflight', 'startup', 'mission', 'cleanup')
             and exception['substage'] in ('open', 'query', 'identity', 'operation', 'result')
             and all(TOKEN.fullmatch(exception[k]) for k in ('module', 'function', 'source')), 'EXCEPTION_SCHEMA_INVALID')
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        need(not (path / 'result.json').exists(), 'RESULT_ALREADY_COMMITTED')
        atomic_json(path / 'result.json', {'schema': 1, 'outcome': outcome, 'code': code, 'exception': exception})
        atomic_json(path / 'handoff.json', {'schema': 1, 'state': 'RESULT_COMMITTED'})

def acknowledge(root, run_id, state):
    with lock(root, 'capsules'):
        path = run_path(root, run_id)
        old = read_json(path / 'handoff.json')
        need((old['state'], state) in (('RESULT_COMMITTED', 'RETRIEVED'), ('RETRIEVED', 'ACKNOWLEDGED')), 'HANDOFF_TRANSITION_REFUSED')
        atomic_json(path / 'handoff.json', {'schema': 1, 'state': state})

def cleanup(root, run_id):
    """Only disposable scratch. No finally hook can delete a run or evidence."""
    with lock(root, 'capsules'):
        scratch = no_links(run_path(root, run_id) / 'scratch')
        tree_files(scratch)
        shutil.rmtree(scratch)
        private_dir(scratch)

def purge(root, run_id, override_unresolved=False):
    with lock(root, 'physical'), lock(root, 'capsules'):
        path = run_path(root, run_id)
        state = retention(path)
        need(state == 'PURGE_ELIGIBLE' or (state == 'UNRESOLVED' and override_unresolved), 'PURGE_REFUSED')
        tree_files(path)  # Reject every symlink and special file before any deletion.
        need(shutil.rmtree.avoids_symlink_attacks, 'SAFE_PURGE_UNAVAILABLE')
        unseal_runner_directories_for_purge(path / 'runner')
        shutil.rmtree(path)
        fsync_dir(path.parent)
        return {'schema': 1, 'run_id': run_id, 'deleted': True, 'previous_state': state}

def inventory(root=ROOT):
    values = []
    for p in sorted((Path(root) / 'runs').iterdir()):
        if not RUN_ID.fullmatch(p.name):
            raise InfraError('RUN_ROOT_UNEXPECTED_ENTRY')
        path = run_path(root, p.name)
        files = tree_files(path)
        uart = path / 'evidence/raw-uart'
        values.append({'run_id': p.name, 'state': retention(path), 'created': p.name.split('-')[0],
                       'total_bytes': sum(f.stat().st_size for f in files),
                       **{'RAW_' + k + '_RETAINED': (path / 'evidence' / ('raw-' + k.lower())).is_file() for k in RAW},
                       'RAW_UART_BYTES': uart.stat().st_size if uart.is_file() else 0,
                       'RAW_UART_HAS_DATA': uart.is_file() and uart.stat().st_size > 0})
    return values

def export(root, run_id, destination):
    with lock(root, 'physical'), lock(root, 'capsules'):
        source = run_path(root, run_id)
        need(retention(source) != 'ACTIVE', 'EXPORT_ACTIVE_REFUSED')
        validate_capsule(source)
        target = no_links(destination)
        need(not target.exists() and not target.is_relative_to(source) and not target.is_relative_to(Path(root) / 'runs'), 'EXPORT_DESTINATION_INVALID')
        check_mode(target.parent, 0o700)
        hashes = hash_tree(source)
    shutil.copytree(source, target, symlinks=True)
    sync_tree(target)
    if read_json(target / 'manifest.json')['schema'] == 2:
        seal_runner_tree(target / 'runner')
        need(hash_tree(target) == hashes and hash_tree(source) == hashes, 'EXPORT_HASH_MISMATCH')
        fsync_dir(target.parent)
        return {'schema': 1, 'run_id': run_id, 'export_verified': True, 'files': len(hashes)}

@contextlib.contextmanager
def physical_run(root, profile, runner, digests, warm_check, *, startup_limit=1):
    """Acquire lock, snapshot runner and yield durable budgets before device use."""
    from startup_probe import OperationBudget
    need(warm_check()['ok'], 'WARM_PREFLIGHT_REQUIRED')
    with lock(root, 'physical'):
        manifest = create(root, profile, runner, digests)
        run_id = manifest['run_id']
        atomic_json(Path(root) / 'state/physical-owner.json', {'run_id': run_id, 'profile': profile, 'started': utc()})
        budget = OperationBudget(startup_limit=startup_limit,
            commit=lambda event, operation: append_event(root, run_id, event, operation))
        body_failed = False
        try:
            yield manifest, budget
        except BaseException:
            body_failed = True
            if retention(run_path(root, run_id)) == 'ACTIVE':
                transition(root, run_id, 'UNRESOLVED')
            raise
        finally:
            snapshot = finalize_runner_snapshot(root, run_id)
            if not snapshot['exact'] and retention(run_path(root, run_id)) == 'ACTIVE':
                transition(root, run_id, 'UNRESOLVED')
            # ACK and lock release are not retention transitions.
            cleanup(root, run_id)
            if not snapshot['exact'] and not body_failed:
                raise InfraError('RUNNER_SNAPSHOT_MUTATED')

def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('inventory')
    for command in ('purge-run', 'mark', 'export'):
        p = sub.add_parser(command)
        p.add_argument('run_id')
        if command == 'purge-run':
            p.add_argument('--override-unresolved', action='store_true')
        elif command == 'mark':
            p.add_argument('state', choices=('UNRESOLVED', 'RESOLVED', 'PURGE_ELIGIBLE'))
        else:
            p.add_argument('destination', type=Path)
    args = parser.parse_args()
    verify_marker()
    if args.command == 'inventory':
        result = {'schema': 1, 'runs': inventory()}
    elif args.command == 'purge-run':
        result = purge(ROOT, args.run_id, args.override_unresolved)
    elif args.command == 'mark':
        transition(ROOT, args.run_id, args.state)
        result = {'schema': 1, 'run_id': args.run_id, 'state': args.state}
    else:
        result = export(ROOT, args.run_id, args.destination)
    print(json.dumps(result))
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        sys.exit(cli_error(exc))
