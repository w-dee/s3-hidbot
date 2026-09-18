"""Ordinary coordinator: immutable v4 handle, root metadata RPC, no raw access."""
from __future__ import annotations
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import secrets
import re
import subprocess
import sys

import evidence_contract as c
import authority_runtime as a
from privileged_evidence import ENV, line

CLASSIFICATION = a.CLASSIFICATION
FILES = a.FILES


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def new_id(): return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + secrets.token_hex(6)


def command(operation, handle):
    a.handle(handle)
    c.need(operation in ('begin','resume','load','activate','capture','verify','record','prepare','seal'), 'OPERATION_INVALID')
    return ['/usr/bin/sudo', '-n', '/usr/bin/python3', '-I', '-S', '-B',
            str(a.INSTALL / 'authority_service.py'), operation, handle['run_id']]


def rpc(operation, handle, payload=None):
    value = payload if operation == 'begin' else {'handle': handle, 'payload': payload}
    result = subprocess.run(command(operation, handle), input=c.encode(value), capture_output=True,
                            timeout=35, env=ENV, shell=False)
    c.need(result.returncode == 0, 'AUTHORITY_RPC_FAILED')
    return c.decode(result.stdout)


def runtime_context():
    c.need(hasattr(sys, '_s3_runtime'), 'PROTECTED_LAUNCHER_REQUIRED')
    return sys._s3_runtime


def verify_attempt(handle):
    a.handle(handle)
    value = rpc('load', handle)
    c.exact(value, 'handle snapshot request')
    c.need(value['handle'] == handle and c.digest(value['snapshot']) == handle['attempt_authority_sha256'],
           'AUTHORITY_CONFLICT')
    a.capture_request(value['request'])
    c.need(value['request']['handle'] == handle, 'AUTHORITY_CONFLICT')
    runtime_id, manifest = runtime_context()
    c.need(runtime_id == handle['runtime_id'] and value['snapshot']['runtime'] == manifest, 'AUTHORITY_CONFLICT')
    return value


def create_package(classification, *, kind='REHEARSAL_HCI', max_seconds=30):
    runtime_id, manifest = runtime_context(); run = new_id()
    temporary = {'schema':3, 'run_id':run, 'capture_id':'0'*32, 'runtime_id':runtime_id, 'attempt_authority_sha256':'0'*64}
    value = rpc('begin', temporary, {'runtime_id':runtime_id, 'classification':classification, 'max_seconds':max_seconds})
    c.exact(value, 'handle snapshot'); handle = a.handle(value['handle'])
    c.need(handle['run_id'] == run and value['snapshot']['kind'] == kind and value['snapshot']['runtime'] == manifest,
           'AUTHORITY_CONFLICT')
    verify_attempt(handle)
    return handle


def package_request(handle=None):
    if handle is None:
        c.need(hasattr(sys, '_s3_attempt'), 'ATTEMPT_REQUIRED'); handle = sys._s3_attempt
    expected = verify_attempt(handle)['request']
    activated = rpc('activate', handle)
    c.need(activated == expected, 'AUTHORITY_CONFLICT')
    return activated


def preflight_command(script, *args):
    runtime_id, manifest = runtime_context()
    c.need(script in manifest['files'] and '/' not in script and script.endswith('.py'), 'ENTRY_INVALID')
    return ['/usr/bin/python3', '-I', '-S', '-B', str(a.INSTALL / 'runtime_launcher.py'),
            runtime_id, script, '-', *map(str, args)]


def verify_capture(envelope):
    a.capture_request(envelope)
    return c.receipt(rpc('verify', envelope['handle'], envelope), envelope['request'])


class CaptureSession:
    def __init__(self, envelope):
        self.envelope = a.capture_request(envelope); self.req = envelope['request']; self.process = None; self.object = None
    def start(self):
        self.process = subprocess.Popen(command('capture', self.envelope['handle']), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=ENV, shell=False)
        try:
            self.process.stdin.write(c.encode({'handle': self.envelope['handle'], 'payload': self.envelope})); self.process.stdin.flush()
            value = line(self.process.stdout.fileno(), 8)
            c.exact(value, 'schema event request helper object')
            c.need(type(value['schema']) is int and value['schema'] == 2 and value['event'] == 'READY'
                   and value['request'] == self.req and value['helper'] == self.req['helper'], 'CAPTURE_READY_INVALID')
            c.exact(value['object'], 'device inode')
            c.need(all(c.integer(x) for x in value['object'].values()), 'OBJECT_INVALID')
            self.object = value['object']; return self
        except BaseException:
            self.close(); raise
    def stop(self):
        c.need(self.process is not None and self.object is not None, 'CAPTURE_NOT_STARTED')
        try:
            self.process.stdin.write(c.encode({'command':'STOP'})); self.process.stdin.flush()
            value = c.receipt(line(self.process.stdout.fileno(),25),self.req)
            c.need(self.process.wait(timeout=3) == 0 and self.process.stdout.read() == b'', 'CAPTURE_HELPER_FAILED')
            if value['raw'] is not None: c.need(value['raw']['object'] == self.object, 'CAPTURE_OBJECT_CHANGED')
            return value
        finally: self.close()
    def close(self):
        if self.process is None: return
        self.process.stdin.close()
        try: self.process.wait(timeout=28)
        except subprocess.TimeoutExpired: raise c.EvidenceError('CAPTURE_HELPER_UNRESPONSIVE')
        finally: self.process.stdout.close()


def stage_package(handle, prepared):
    c.exact(prepared, 'handle files'); c.need(prepared['handle'] == handle and set(prepared['files']) == set(FILES), 'AUTHORITY_CONFLICT')
    with c.Directory(a.STATE / 'staging' / handle['run_id']) as d:
        for name in os.listdir(d.fd):
            if not re.fullmatch(r'\.pending-[0-9a-f]{24}',name): continue
            fd=os.open(name,os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,dir_fd=d.fd)
            try:
                d.check_file(name,fd,os.getuid()); os.unlink(name,dir_fd=d.fd)
            finally: os.close(fd)
        os.fsync(d.fd)
        for name,value in prepared['files'].items():
            # Existing stage is evidence to verify, never refreshed/overwritten on retry.
            try:
                old = c.read_bytes(d, name)
                c.need(old == c.encode(value), 'AUTHORITY_CONFLICT' if name == 'authority.json' else 'PACKAGE_CHANGED')
            except FileNotFoundError: c.write_once(d, name, value)
    return c.digest(prepared)


def finalize_terminal_package(handle, *, test_outcome, details=None):
    verify_attempt(handle)
    test = rpc('record', handle, {'outcome':test_outcome, 'details':details})
    c.need(test == {'handle':handle, 'outcome':test_outcome, 'details':details}, 'AUTHORITY_CONFLICT')
    prepared = rpc('prepare', handle)
    digest = stage_package(handle, prepared)
    commit = rpc('seal', handle, digest)
    c.exact(commit, 'schema state handle engine files index_sha256 test evidence code')
    c.need(type(commit['schema']) is int and commit['schema'] == 3 and commit['state'] == 'COMMITTED'
           and commit['handle'] == handle and commit['test'] == test_outcome
           and commit['engine'] == prepared['files']['authority.json']['engine'], 'COMMIT_INVALID')
    c.need(set(commit['files']) == set(FILES), 'COMMIT_INVALID')
    for name, value in prepared['files'].items():
        c.need(commit['files'][name]['sha256'] == c.digest(value), 'COMMIT_INVALID')
    evidence = prepared['files']['evidence.json']['status']
    expected = 'EVIDENCE_FINALIZATION_FAILED' if evidence != 'FINALIZED' else 'SUCCESS' if test_outcome == 'PASS' else 'TEST_FAILED'
    c.need(commit['index_sha256'] == commit['files']['index.json']['sha256'] and commit['evidence'] == evidence
           and commit['code'] == expected, 'COMMIT_INVALID')
    return commit


def phase_command(handle, script, *args):
    verify_attempt(handle)
    return ['/usr/bin/python3', '-I', '-S', '-B', str(a.INSTALL / 'runtime_launcher.py'),
            handle['runtime_id'], script, c.encode(handle).decode().strip(), *map(str,args)]


def resume_attempt(run):
    c.run_id(run); runtime_id,_=runtime_context()
    lookup={'schema':3,'run_id':run,'capture_id':'0'*32,'runtime_id':runtime_id,'attempt_authority_sha256':'0'*64}
    value=rpc('resume',lookup)
    a.handle(value['handle']); verify_attempt(value['handle'])
    return value['handle']


def resume_terminal(run):
    handle=resume_attempt(run)
    prepared=rpc('prepare',handle)  # Requires the already durable original test result.
    c.need(prepared['handle']==handle,'AUTHORITY_CONFLICT')
    test=prepared['files']['test.json']
    return finalize_terminal_package(handle,test_outcome=test['outcome'],details=test['details'])
