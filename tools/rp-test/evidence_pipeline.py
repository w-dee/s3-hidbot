"""Version 2 unprivileged coordinator boundary; raw bytes never enter this module."""
from __future__ import annotations

from datetime import datetime, timezone
import fcntl
import hashlib
import os
from pathlib import Path
import secrets
import re
import stat
import subprocess

import evidence_contract as c
from privileged_evidence import INSTALL, ENV, line

CLASSIFICATION = 'EVIDENCE_PIPELINE_REHEARSAL / NOT_QUALIFICATION'
FILES = ('authority.json', 'test.json', 'evidence.json', 'manifest.json', 'index.json')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def code_authority(root, plan):
    root = Path(root)
    rows = {str(p.relative_to(root)): sha(p) for p in sorted(root.rglob('*'))
            if p.is_file() and '__pycache__' not in p.parts and not p.name.endswith(('.pyc', '.pyo'))
            and p.name not in ('FROZEN.json', 'TOOLSHA256SUMS', 'RUNNER_DIGEST')}
    c.need('official_campaign.py' in rows, 'COORDINATOR_MISSING')
    helper = {'version': 2, 'helper_sha256': rows['privileged_evidence.py'],
              'imports': {'evidence_contract.py': rows['evidence_contract.py']}}
    return {'schema': 2, 'runner_sha256': c.digest(rows), 'runner_files': rows,
            'helper': helper, 'coordinator_sha256': rows['official_campaign.py'],
            'plan_sha256': sha(plan)}


def command(operation, req):
    c.request(req)
    c.need(operation in ('capture', 'verify'), 'OPERATION_INVALID')
    return ['/usr/bin/sudo', '-n', '/usr/bin/python3', '-I', '-B',
            str(INSTALL / 'privileged_evidence.py'), operation, req['run_id'], req['capture_id']]


def verify_capture(req):
    result = subprocess.run(command('verify', req), input=c.encode(req), capture_output=True,
                            timeout=25, env=ENV, shell=False)
    c.need(result.returncode == 0, 'CAPTURE_VERIFY_FAILED')
    return c.receipt(c.decode(result.stdout), req)


class CaptureSession:
    def __init__(self, req):
        self.req = c.request(req); self.process = None; self.object = None

    def start(self):
        self.process = subprocess.Popen(command('capture', self.req), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        env=ENV, shell=False)
        try:
            self.process.stdin.write(c.encode(self.req)); self.process.stdin.flush()
            value = line(self.process.stdout.fileno(), 8)
            c.exact(value, 'schema event request helper object')
            c.need(type(value['schema']) is int and value['schema'] == 2 and value['event'] == 'READY'
                   and c.encode(value['request']) == c.encode(self.req)
                   and value['helper'] == self.req['helper'], 'CAPTURE_READY_INVALID')
            c.exact(value['object'], 'device inode')
            c.need(all(c.integer(x) for x in value['object'].values()), 'OBJECT_INVALID')
            self.object = value['object']
            return self
        except BaseException:
            self.close()
            raise

    def stop(self):
        c.need(self.process is not None and self.object is not None, 'CAPTURE_NOT_STARTED')
        try:
            self.process.stdin.write(c.encode({'command': 'STOP'})); self.process.stdin.flush()
            value = c.receipt(line(self.process.stdout.fileno(), 25), self.req)
            c.need(self.process.wait(timeout=3) == 0 and self.process.stdout.read() == b'',
                   'CAPTURE_HELPER_FAILED')
            if value['raw'] is not None:
                c.need(value['raw']['object'] == self.object, 'CAPTURE_OBJECT_CHANGED')
            return value
        finally:
            self.close()

    def close(self):
        if self.process is None:
            return
        self.process.stdin.close()
        # EOF asks the producer to stop/reap; do not kill sudo and orphan a capture.
        try:
            self.process.wait(timeout=28)
        except subprocess.TimeoutExpired:
            # The root producer independently enforces its capture deadline.
            raise c.EvidenceError('CAPTURE_HELPER_UNRESPONSIVE')
        finally:
            self.process.stdout.close()


def new_id():
    return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + secrets.token_hex(6)


def create_package(base, classification, authority, *, attempt=None, kind='REHEARSAL_HCI', max_seconds=30):
    base = Path(base)
    base.mkdir(parents=True, exist_ok=True, mode=0o700)
    run = new_id(); capture = secrets.token_hex(16)
    frozen = {'schema': 2, 'attempt_id': attempt or run, 'run_id': run, 'capture_id': capture,
              'classification': classification, 'kind': kind, 'code': authority, 'max_seconds': max_seconds}
    with c.Directory(base) as parent:
        parent.child(run, create=True)
        c.write_once(parent, 'authority.json', frozen)
        fd = os.open('lock', os.O_CREAT | os.O_EXCL | os.O_RDONLY | os.O_NOFOLLOW, 0o400, dir_fd=parent.fd)
        os.fsync(fd); os.close(fd); os.fsync(parent.fd)
    with c.Directory(base) as parent:
        parent.child('commits', create=True)
    return base / run


def package_request(package):
    with c.Directory(package) as directory:
        a = c.read_json(directory, 'authority.json')
    c.need(a['run_id'] == Path(package).name, 'PACKAGE_IDENTITY_INVALID')
    return c.request({'schema': 2, 'attempt_id': a['attempt_id'], 'run_id': a['run_id'],
                      'capture_id': a['capture_id'], 'kind': a['kind'], 'authority_sha256': c.digest(a),
                      'helper': a['code']['helper'], 'max_seconds': a['max_seconds']})


def clear_interrupted_writes(directory):
    """Only uncommitted, privately created staging names are disposable."""
    for name in os.listdir(directory.fd):
        if not re.fullmatch(r'\.pending-[0-9a-f]{24}', name, re.ASCII):
            continue
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory.fd)
        try:
            directory.check_file(name, fd, os.getuid())
            os.unlink(name, dir_fd=directory.fd)
        finally:
            os.close(fd)
    os.fsync(directory.fd)


def finalize_terminal_package(package, *, test_outcome, details=None, _verify=verify_capture, _cut=lambda _: None):
    """Recoverable staging; only the separate last commit record is terminal authority.

    No receipt JSON argument exists. Production always reverifies through the fixed producer.
    Dependency overrides prefixed '_' are deterministic test seams, absent from any CLI.
    """
    c.need(test_outcome in ('PASS', 'FAIL'), 'TEST_OUTCOME_INVALID')
    package = Path(package); req = package_request(package)
    with c.Directory(package) as directory, c.Directory(package.parent / 'commits') as commits:
        lock = os.open('lock', os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory.fd)
        try:
            directory.check_file('lock', lock); fcntl.flock(lock, fcntl.LOCK_EX)
            clear_interrupted_writes(directory)
            authority = c.read_json(directory, 'authority.json')
            test = {'schema': 2, 'attempt_id': req['attempt_id'], 'run_id': req['run_id'],
                    'outcome': test_outcome, 'details': details}
            c.write_once(directory, 'test.json', test); _cut('test')
            prior = c.optional_json(directory, 'evidence.json')
            if prior is not None and prior['status'] == 'FAILED':
                evidence = prior  # Failure cannot upgrade on retry.
            else:
                try:
                    value = c.receipt(_verify(req), req)
                    evidence = {'status': value['status'], 'receipt': value, 'error': value['error']}
                except (c.EvidenceError, OSError, subprocess.SubprocessError):
                    evidence = {'status': 'FAILED', 'receipt': None, 'error': 'CAPTURE_VERIFY_FAILED'}
                if prior is not None:
                    c.need(c.encode(evidence) == c.encode(prior), 'EVIDENCE_CHANGED')
            c.write_once(directory, 'evidence.json', evidence)
            manifest = {'schema': 2, 'state': 'PREPARED', 'run_id': req['run_id'],
                        'classification': authority['classification'], 'authority_sha256': c.digest(authority),
                        'test_sha256': c.digest(test), 'evidence_sha256': c.digest(evidence),
                        'automatic_purge': False}
            c.write_once(directory, 'manifest.json', manifest); _cut('manifest')
            index = {'schema': 2, 'state': 'PREPARED',
                     'files': {name: hashlib.sha256(c.read_bytes(directory, name)).hexdigest()
                               for name in FILES[:-1]},
                     'raw_evidence': None if evidence['receipt'] is None else evidence['receipt']['raw']}
            c.write_once(directory, 'index.json', index); _cut('index')
            c.need(set(os.listdir(directory.fd)) == set((*FILES, 'lock')), 'PACKAGE_ENTRIES_INVALID')
            for name in (*FILES, 'lock'):
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory.fd)
                try:
                    directory.check_file(name, fd, os.getuid())
                    if stat.S_IMODE(os.fstat(fd).st_mode) != 0o400:
                        os.fchmod(fd, 0o400)
                    os.fsync(fd)
                finally:
                    os.close(fd)
            directory.check()
            if stat.S_IMODE(os.fstat(directory.fd).st_mode) != 0o500:
                os.fchmod(directory.fd, 0o500)
            os.fsync(directory.fd); os.fsync(directory.fds[-2]); _cut('permissions')
            # Validate staged bytes/modes after sealing and before the terminal publication.
            for name in (*FILES, 'lock'):
                s = os.stat(name, dir_fd=directory.fd, follow_symlinks=False)
                c.regular(s, os.getuid()); c.need(stat.S_IMODE(s.st_mode) == 0o400, 'PACKAGE_MODE_INVALID')
            for name, digest in index['files'].items():
                c.need(hashlib.sha256(c.read_bytes(directory, name)).hexdigest() == digest, 'PACKAGE_CHANGED')
            directory.check()
            code = ('EVIDENCE_FINALIZATION_FAILED' if evidence['status'] != 'FINALIZED' else
                    'SUCCESS' if test_outcome == 'PASS' else 'TEST_FAILED')
            commit = {'schema': 2, 'state': 'COMMITTED', 'run_id': req['run_id'],
                      'authority_sha256': c.digest(authority), 'index_sha256': c.digest(index),
                      'test': test_outcome, 'evidence': evidence['status'], 'code': code}
            _cut('before_commit')
            c.write_once(commits, req['run_id'] + '.json', commit); _cut('after_commit')
            return commit
        finally:
            os.close(lock)


def tree_digest(package):
    with c.Directory(package) as directory:
        rows = {name: hashlib.sha256(c.read_bytes(directory, name)).hexdigest() for name in FILES}
    with c.Directory(Path(package).parent / 'commits') as commits:
        rows['commit'] = hashlib.sha256(c.read_bytes(commits, Path(package).name + '.json')).hexdigest()
    return c.digest(rows)
