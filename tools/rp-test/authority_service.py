#!/usr/bin/env python3
"""Fixed-root v4 attempt journal, optional capture set and metadata sealer."""
from __future__ import annotations
import contextlib
import fcntl
import hashlib
import os
from pathlib import Path
import secrets
import re
import select
import stat
import subprocess
import sys
import time
import types

# Executed with -I -S: bootstrap only protected source, never local bytecode.
def bootstrap(name):
    path = Path('/usr/local/lib/s3-hidbot-authority-v5') / (name + '.py')
    for parent in (path, *path.parents):
        s = parent.lstat()
        if s.st_uid != 0 or s.st_mode & 0o022 or stat.S_ISLNK(s.st_mode):
            raise RuntimeError('INSTALL_AUTHORITY_INVALID')
    if path.stat().st_mode & 0o222 or path.stat().st_nlink != 1:
        raise RuntimeError('INSTALL_AUTHORITY_INVALID')
    module = types.ModuleType(name); module.__file__ = str(path); sys.modules[name] = module
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module

if __name__ == '__main__':
    c = bootstrap('evidence_contract'); a = bootstrap('authority_runtime'); h = bootstrap('privileged_evidence')
else:
    import evidence_contract as c
    import authority_runtime as a
    import privileged_evidence as h


def read(d, name, owner):
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=d.fd)
    try:
        d.check_file(name, fd, owner)
        c.need(stat.S_IMODE(os.fstat(fd).st_mode) == 0o400, 'JOURNAL_MODE_INVALID')
        digest, s = c.stable_hash(fd)
        data = c.read_bytes(d, name)
        c.need(a.sha(data) == digest and c.signature(s) == c.signature(os.fstat(fd)), 'JOURNAL_CHANGED')
        d.check_file(name, fd, owner)
        return c.decode(data)
    finally: os.close(fd)


def optional(d, name, owner):
    try: return read(d, name, owner)
    except FileNotFoundError: return None


def write(d, name, value, owner):
    try:
        old = read(d, name, owner)
    except FileNotFoundError:
        c.write_once(d, name, value); return
    c.need(c.encode(old) == c.encode(value), 'IMMUTABLE_CONFLICT')


def clear_pending(directory, owner):
    for name in os.listdir(directory.fd):
        if not re.fullmatch(r'\.pending-[0-9a-f]{24}', name): continue
        fd=os.open(name,os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,dir_fd=directory.fd)
        try:
            directory.check_file(name,fd,owner)
            os.unlink(name,dir_fd=directory.fd)
        finally: os.close(fd)
    os.fsync(directory.fd)


class Authority:
    """Internal path/producer/cut seams are for tests; none are CLI parameters."""
    def __init__(self, state=a.STATE, install=a.INSTALL, *, owner=0, producer=None, cut=lambda _: None):
        self.state, self.install, self.owner = Path(state), Path(install), owner
        self.producer_override, self.cut = producer, cut

    def engine(self): return a.engine_identity(self.install, self.owner)

    def storage(self):
        value=c.decode(a.source(self.install / 'roots.json',self.owner))
        c.exact(value,'state captures')
        for name,path in (('state',self.state),('captures',self.state/'captures')):
            with c.Directory(path) as directory:
                a.protected(directory,self.owner,0o755 if name=='state' else 0o700)
                c.need(c.object_identity(os.fstat(directory.fd))==value[name],'ROOT_OBJECT_CHANGED')
        return value

    def check_runtime(self, runtime_id, expected=None):
        manifest = a.runtime(self.install / 'runtimes' / runtime_id, runtime_id, expected, owner=self.owner)
        c.need(manifest['engine'] == self.engine(), 'ENGINE_AUTHORITY_CONFLICT')
        return manifest

    @contextlib.contextmanager
    def locked(self, run):
        c.run_id(run)
        with c.Directory(self.state) as d:
            a.protected(d, self.owner, 0o755)
            d.child('attempts'); a.protected(d, self.owner, 0o700)
            d.child(run); a.protected(d, self.owner, 0o700)
            fd = os.open('lock', os.O_RDONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o400, dir_fd=d.fd)
            try:
                d.check_file('lock', fd, self.owner); fcntl.flock(fd, fcntl.LOCK_EX)
                clear_pending(d, self.owner)
                yield d
            finally: os.close(fd)

    def begin(self, runtime_id, run, classification, uid, max_seconds):
        c.run_id(run)
        c.need(classification in (a.CLASSIFICATION, 'OFFICIAL_FNK0099_V0_4_0'), 'CLASSIFICATION_INVALID')
        c.need(c.integer(uid) and c.integer(max_seconds, 1) and max_seconds <= 3600, 'BEGIN_INVALID')
        manifest = self.check_runtime(runtime_id)
        with c.Directory(self.state / 'attempts') as parent:
            a.protected(parent, self.owner, 0o700)
            os.mkdir(run, 0o700, dir_fd=parent.fd); os.fsync(parent.fd)
        with self.locked(run) as d:
            snapshot = {'schema': 3, 'attempt_id': run, 'run_id': run, 'capture_id': secrets.token_hex(16),
                'classification': classification, 'owner_uid': uid, 'runtime_id': runtime_id,
                'runtime': manifest, 'product': a.PRODUCT, 'engine': self.engine(), 'storage': self.storage(), 'evidence_schema': 2,
                'max_seconds': max_seconds, 'kind': 'REHEARSAL_HCI' if classification == a.CLASSIFICATION else 'Q8_HCI',
                'interpreter': {'path': '/usr/bin/python3', 'sha256': a.sha(Path('/usr/bin/python3').read_bytes()),
                                'version': sys.version, 'dependencies': 'root-owned /usr/lib/python3*; isolated -I -S -B'}}
            handle = {'schema': 3, 'run_id': run, 'capture_id': snapshot['capture_id'], 'runtime_id': runtime_id,
                      'attempt_authority_sha256': c.digest(snapshot)}
            request = {'schema': 2, 'attempt_id': run, 'run_id': run, 'capture_id': snapshot['capture_id'],
                'kind': snapshot['kind'], 'authority_sha256': handle['attempt_authority_sha256'],
                'helper': {'version': 2, 'helper_sha256': snapshot['engine']['privileged_evidence.py'],
                           'imports': {'evidence_contract.py': snapshot['engine']['evidence_contract.py']}},
                'max_seconds': max_seconds}
            write(d, 'authority.json', snapshot, self.owner)
            write(d, 'handle.json', handle, self.owner)
            write(d, 'request.json', {'handle': handle, 'attempt_authority_sha256': handle['attempt_authority_sha256'],
                                      'request': c.request(request)}, self.owner)
            with c.Directory(self.state / 'staging') as stage:
                a.protected(stage, self.owner, 0o755)
                os.mkdir(run, 0o700, dir_fd=stage.fd); stage.child(run)
                os.fchown(stage.fd, uid, -1); os.fsync(stage.fd); os.fsync(stage.fds[-2])
            self.cut('snapshot')
            return {'handle': handle, 'snapshot': snapshot}

    def load_locked(self, d, expected, uid):
        a.handle(expected)
        original = read(d, 'handle.json', self.owner)
        c.need(original == expected, 'AUTHORITY_CONFLICT')
        snapshot = read(d, 'authority.json', self.owner)
        c.need(c.digest(snapshot) == expected['attempt_authority_sha256'], 'AUTHORITY_CONFLICT')
        c.need(snapshot['owner_uid'] == uid and snapshot['run_id'] == expected['run_id']
               and snapshot['capture_id'] == expected['capture_id'] and snapshot['runtime_id'] == expected['runtime_id'],
               'AUTHORITY_CONFLICT')
        self.check_runtime(expected['runtime_id'], snapshot['runtime'])
        c.need(snapshot['engine'] == self.engine(), 'ENGINE_AUTHORITY_CONFLICT')
        c.need(snapshot['storage'] == self.storage(), 'ROOT_OBJECT_CHANGED')
        c.need(snapshot['interpreter']['sha256'] == a.sha(Path('/usr/bin/python3').read_bytes()), 'INTERPRETER_CHANGED')
        req = a.capture_request(read(d, 'request.json', self.owner))
        c.need(req['handle'] == expected, 'AUTHORITY_CONFLICT')
        original_request = {'schema':2, 'attempt_id':snapshot['attempt_id'], 'run_id':snapshot['run_id'],
            'capture_id':snapshot['capture_id'], 'kind':snapshot['kind'],
            'authority_sha256':expected['attempt_authority_sha256'], 'max_seconds':snapshot['max_seconds'],
            'helper':{'version':2, 'helper_sha256':snapshot['engine']['privileged_evidence.py'],
                      'imports':{'evidence_contract.py':snapshot['engine']['evidence_contract.py']}}}
        c.need(req['request'] == original_request, 'AUTHORITY_CONFLICT')
        return snapshot, req

    def load(self, expected, uid):
        with self.locked(expected['run_id']) as d:
            snapshot, req = self.load_locked(d, expected, uid)
            return {'handle': expected, 'snapshot': snapshot, 'request': req}

    def resume(self, run, uid):
        with self.locked(run) as d:
            expected=read(d,'handle.json',self.owner)
            snapshot,req=self.load_locked(d,expected,uid)
            with c.Directory(self.state/'staging') as stage:
                a.protected(stage,self.owner,0o755)
                try:
                    os.mkdir(run,0o700,dir_fd=stage.fd)
                except FileExistsError: pass
                stage.child(run)
                current=os.fstat(stage.fd)
                c.need(current.st_uid in (self.owner,uid) and stat.S_IMODE(current.st_mode)==0o700,'STAGE_INVALID')
                os.fchown(stage.fd,uid,-1); os.fsync(stage.fd); os.fsync(stage.fds[-2])
            return {'handle':expected,'snapshot':snapshot,'request':req}

    def producer(self, req):
        if self.producer_override is not None: return self.producer_override
        engine=self.engine()
        actual={'version':2,'helper_sha256':engine['privileged_evidence.py'],
                'imports':{'evidence_contract.py':engine['evidence_contract.py']}}
        c.need(req['helper']==actual,'HELPER_IDENTITY_MISMATCH')
        return h.Producer(self.state / 'captures', actual, root_object=self.storage()['captures'])

    def capture(self, envelope, uid, *, capture=False, ready=lambda _: None, control=lambda _: False):
        a.capture_request(envelope); expected = envelope['handle']
        with self.locked(expected['run_id']) as d:
            _, req = self.load_locked(d, expected, uid)
            c.need(envelope == req, 'AUTHORITY_CONFLICT')
            activation = optional(d, 'evidence-required.json', self.owner)
            c.need(activation == {'handle': expected, 'request': req}, 'EVIDENCE_NOT_REQUIRED')
            return c.receipt(self.producer(req['request']).operate(req['request'], capture=capture,
                ready=ready, control=control), req['request'])

    def activate(self, expected, uid):
        with self.locked(expected['run_id']) as d:
            _, req = self.load_locked(d, expected, uid)
            c.need(optional(d, 'test.json', self.owner) is None
                   and optional(d, 'evidence.json', self.owner) is None,
                   'EVIDENCE_ACTIVATION_TOO_LATE')
            write(d, 'evidence-required.json', {'handle': expected, 'request': req}, self.owner)
            self.cut('evidence_required')
            return req

    def record(self, expected, uid, outcome, details):
        c.need(outcome in ('PASS', 'FAIL'), 'TEST_OUTCOME_INVALID')
        with self.locked(expected['run_id']) as d:
            self.load_locked(d, expected, uid)
            value = {'handle': expected, 'outcome': outcome, 'details': details}
            write(d, 'test.json', value, self.owner); self.cut('test')
            return value

    def prepare(self, expected, uid):
        with self.locked(expected['run_id']) as d:
            snapshot, req = self.load_locked(d, expected, uid)
            test = read(d, 'test.json', self.owner)
            c.need(test['handle'] == expected, 'AUTHORITY_CONFLICT')
            evidence = optional(d, 'evidence.json', self.owner)
            if evidence is None:
                activation = optional(d, 'evidence-required.json', self.owner)
                if activation is not None:
                    c.need(activation == {'handle': expected, 'request': req}, 'AUTHORITY_CONFLICT')
                required = activation is not None
                c.need(not (snapshot['classification'] == 'OFFICIAL_FNK0099_V0_4_0'
                           and test['outcome'] == 'PASS' and not required),
                       'OFFICIAL_EVIDENCE_NOT_ACTIVATED')
                if not required:
                    evidence = {'handle': expected, 'required': False, 'status': 'FINALIZED',
                                'receipt': None, 'error': None}
                else:
                    try:
                        receipt = c.receipt(self.producer(req['request']).operate(req['request']), req['request'])
                        evidence = {'handle': expected, 'required': True, 'status': receipt['status'],
                                    'receipt': receipt, 'error': receipt['error']}
                    except (c.EvidenceError, OSError, subprocess.SubprocessError):
                        evidence = {'handle': expected, 'required': True, 'status': 'FAILED',
                                    'receipt': None, 'error': 'CAPTURE_VERIFY_FAILED'}
                write(d, 'evidence.json', evidence, self.owner)
            c.need(evidence['handle'] == expected, 'AUTHORITY_CONFLICT')
            if evidence['receipt'] is not None: c.receipt(evidence['receipt'], req['request'])
            files = {'authority.json': snapshot, 'request.json': req, 'test.json': test, 'evidence.json': evidence}
            files['manifest.json'] = {'schema': 3, 'state': 'PREPARED', 'handle': expected,
                'classification': snapshot['classification'], 'files': {n:c.digest(v) for n,v in files.items()},
                'automatic_purge': False}
            files['index.json'] = {'schema': 3, 'state': 'PREPARED', 'handle': expected,
                'files': {n:c.digest(v) for n,v in files.items()},
                'raw_evidence': None if evidence['receipt'] is None else evidence['receipt']['raw']}
            prepared = {'handle': expected, 'files': files}
            write(d, 'prepared.json', prepared, self.owner); self.cut('prepared')
            return prepared

    def validate_files(self, directory, files, owner):
        c.need(set(os.listdir(directory.fd)) == set(files), 'PACKAGE_ENTRIES_INVALID')
        result = {}; held = []
        try:
            for name, value in files.items():
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory.fd); held.append((name, fd))
                directory.check_file(name, fd, owner)
                s = os.fstat(fd)
                c.need(stat.S_IMODE(s.st_mode) == 0o400, 'PACKAGE_MODE_INVALID')
                digest, final = c.stable_hash(fd)
                c.need(digest == c.digest(value), 'AUTHORITY_CONFLICT' if name in ('authority.json','request.json') else 'PACKAGE_CHANGED')
                result[name] = {'sha256': digest, 'object': c.object_identity(final), 'mode': '0400', 'uid': final.st_uid}
            for name, fd in held: directory.check_file(name, fd, owner)
            return result
        finally:
            for _, fd in held: os.close(fd)

    def seal(self, expected, uid, prepared_sha256):
        with self.locked(expected['run_id']) as d:
            snapshot, req = self.load_locked(d, expected, uid)
            prepared = read(d, 'prepared.json', self.owner)
            c.need(prepared['handle'] == expected and c.digest(prepared) == prepared_sha256, 'AUTHORITY_CONFLICT')
            files = prepared['files']
            c.need(files['authority.json'] == snapshot and files['request.json'] == req, 'AUTHORITY_CONFLICT')
            c.need(files['test.json'] == read(d, 'test.json', self.owner) and
                   files['evidence.json'] == read(d, 'evidence.json', self.owner), 'AUTHORITY_CONFLICT')
            evidence = files['evidence.json']
            activation = optional(d, 'evidence-required.json', self.owner)
            if activation is not None:
                c.need(activation == {'handle': expected, 'request': req}, 'AUTHORITY_CONFLICT')
            c.need(evidence['required'] == (activation is not None), 'EVIDENCE_PLAN_CHANGED')
            if evidence['status'] == 'FINALIZED' and evidence['receipt'] is not None:
                current = self.producer(req['request']).operate(req['request'])
                c.need(c.receipt(current, req['request']) == evidence['receipt'], 'EVIDENCE_CHANGED')
            prior = optional(d, 'commit.json', self.owner)
            if prior is None:
                with c.Directory(self.state / 'staging' / expected['run_id']) as stage:
                    self.validate_files(stage, files, uid); self.cut('staged')
                    # New root-owned inodes, never chmod/chown any user-owned source.
                    with c.Directory(self.state / 'attempts' / expected['run_id']) as output:
                        output.child('package', create=True)
                        clear_pending(output, self.owner)
                        for name, value in files.items():
                            write(output, name, value, self.owner); self.cut(name)
                        self.validate_files(stage, files, uid)
                        os.fchmod(output.fd, 0o500); os.fsync(output.fd); os.fsync(output.fds[-2])
            self.cut('before_validation')
            with c.Directory(self.state / 'attempts' / expected['run_id'] / 'package') as final:
                a.protected(final, self.owner, 0o500)
                actual = self.validate_files(final, files, self.owner)
                self.cut('validated')
                # Repeat after fault/crash seam: commit always describes final actual objects.
                c.need(self.validate_files(final, files, self.owner) == actual, 'PACKAGE_CHANGED')
            code = ('EVIDENCE_FINALIZATION_FAILED' if evidence['status'] != 'FINALIZED' else
                    'SUCCESS' if files['test.json']['outcome'] == 'PASS' else 'TEST_FAILED')
            commit = {'schema': 3, 'state': 'COMMITTED', 'handle': expected, 'engine': snapshot['engine'],
                      'files': actual, 'index_sha256': actual['index.json']['sha256'],
                      'test': files['test.json']['outcome'], 'evidence': evidence['status'], 'code': code}
            self.cut('before_commit')
            with c.Directory(self.state / 'attempts' / expected['run_id'] / 'package') as final:
                a.protected(final,self.owner,0o500)
                c.need(self.validate_files(final,files,self.owner)==actual,'PACKAGE_CHANGED')
            # No untrusted writer can change the private package namespace in this interval.
            write(d, 'commit.json', commit, self.owner); self.cut('after_commit')
            return commit


def main():
    c.need(os.getuid() == 0 and os.getgid() == 0 and Path(__file__) == a.INSTALL / 'authority_service.py'
           and sys.flags.isolated and sys.flags.no_site, 'EXECUTION_AUTHORITY_INVALID')
    c.need(len(sys.argv) == 3, 'ARGUMENT_INVALID')
    operation, run = sys.argv[1:]; c.run_id(run)
    uid = int(os.environ['SUDO_UID']); c.need(uid != 0, 'CALLER_INVALID')
    value = h.line(0, 5); service = Authority()
    def output(result): sys.stdout.buffer.write(c.encode(result)); sys.stdout.buffer.flush()
    if operation == 'begin':
        c.exact(value, 'runtime_id classification max_seconds')
        result = service.begin(value['runtime_id'], run, value['classification'], uid, value['max_seconds'])
    else:
        c.exact(value, 'handle payload'); a.handle(value['handle'])
        expected = value['handle']; payload = value['payload']
        c.need(expected['run_id'] == run, 'AUTHORITY_CONFLICT')
        if operation == 'resume':
            c.need(payload is None,'PAYLOAD_INVALID'); result=service.resume(run,uid)
        elif operation == 'load':
            c.need(payload is None, 'PAYLOAD_INVALID'); result = service.load(expected, uid)
        elif operation == 'activate':
            c.need(payload is None, 'PAYLOAD_INVALID'); result = service.activate(expected, uid)
        elif operation in ('capture', 'verify'):
            c.need(payload['handle'] == expected, 'AUTHORITY_CONFLICT')
            def control(process):
                deadline = time.monotonic() + payload['request']['max_seconds']
                while time.monotonic() < deadline and process.poll() is None:
                    if select.select([0], [], [], .05)[0]: return h.line(0, 2) == {'command': 'STOP'}
                return False
            result = service.capture(payload, uid, capture=operation == 'capture', ready=output, control=control)
        elif operation == 'record':
            c.exact(payload, 'outcome details'); result = service.record(expected, uid, **payload)
        elif operation == 'prepare':
            c.need(payload is None, 'PAYLOAD_INVALID'); result = service.prepare(expected, uid)
        elif operation == 'seal':
            c.need(c.matches(c.DIGEST, payload), 'PAYLOAD_INVALID'); result = service.seal(expected, uid, payload)
        else: raise c.EvidenceError('OPERATION_INVALID')
    output(result)

if __name__ == '__main__':
    try: main()
    except (c.EvidenceError, OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        sys.stdout.buffer.write(c.encode({'schema':3, 'error': str(exc) if isinstance(exc,c.EvidenceError) else 'AUTHORITY_FAILED'}))
        raise SystemExit(1)
