#!/usr/bin/env python3
"""Fixed-root, root-owned capture producer. The CLI never accepts a path."""
from __future__ import annotations

import argparse
import ctypes
import fcntl
import hashlib
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import sys
import time
import types

INSTALL = Path('/usr/local/lib/s3-hidbot-evidence-v2')
EVIDENCE_ROOT = Path('/srv/s3-hidbot-test/private/evidence-authority-v2')
ENV = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'}


def protected_bytes(path):
    """Root-only installation, including all ancestors; load exactly hashed bytes."""
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd); fd = child
            s = os.fstat(fd)
            if s.st_uid != 0 or s.st_mode & 0o022:
                raise RuntimeError('INSTALL_AUTHORITY_INVALID')
        child = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
        try:
            s = os.fstat(child)
            if s.st_uid != 0 or s.st_nlink != 1 or not stat.S_ISREG(s.st_mode) or s.st_mode & 0o222:
                raise RuntimeError('INSTALL_AUTHORITY_INVALID')
            with os.fdopen(child, 'rb', closefd=False) as stream:
                return stream.read(1024 * 1024)
        finally:
            os.close(child)
    finally:
        os.close(fd)


if __name__ == '__main__':
    source = protected_bytes(INSTALL / 'evidence_contract.py')
    c = types.ModuleType('evidence_contract')
    exec(compile(source, str(INSTALL / 'evidence_contract.py'), 'exec'), c.__dict__)
    CODE_IDENTITY = {'version': 2,
                     'helper_sha256': hashlib.sha256(protected_bytes(INSTALL / 'privileged_evidence.py')).hexdigest(),
                     'imports': {'evidence_contract.py': hashlib.sha256(source).hexdigest()}}
else:
    import evidence_contract as c


def line(fd, timeout):
    data = b''; deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
            break
        part = os.read(fd, 1)
        c.need(part, 'CONTROL_EOF')
        data += part
        c.need(len(data) <= c.MAX_JSON, 'JSON_SIZE_INVALID')
        if part == b'\n':
            return c.decode(data)
    raise c.EvidenceError('CONTROL_TIMEOUT')


def child_death_signal(parent):
    def setup():
        if ctypes.CDLL(None).prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != parent:
            os._exit(125)
    return setup


def stop_writer(process, requested, early, waits=(5, 2, 2)):
    forced = None
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=waits[0])
        except subprocess.TimeoutExpired:
            forced = 'SIGTERM'; process.terminate()
            try:
                process.wait(timeout=waits[1])
            except subprocess.TimeoutExpired:
                forced = 'SIGKILL'; process.kill(); process.wait(timeout=waits[2])
    process.wait()
    return {'pid': process.pid, 'exit_code': process.returncode, 'requested_stop': requested,
            'forced_signal': forced, 'reaped': True, 'early_exit': early}


def decode_counts(fd):
    process = subprocess.Popen(['/usr/bin/btmon', '-r', '/proc/self/fd/' + str(fd)],
                               pass_fds=(fd,), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, env=ENV,
                               preexec_fn=child_death_signal(os.getpid()))
    data = bytearray(); deadline = time.monotonic() + 15
    try:
        while True:
            c.need(time.monotonic() < deadline, 'DECODE_TIMEOUT')
            if not select.select([process.stdout], [], [], .1)[0]:
                continue
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                break
            data.extend(chunk); c.need(len(data) <= 16 * 1024 * 1024, 'DECODE_TOO_LARGE')
        c.need(process.wait(timeout=2) == 0, 'DECODE_FAILED')
        lower = bytes(data).lower()
        return dict(zip(('pairing_requests', 'pairing_responses', 'security_requests'),
                        (lower.count(x) for x in (b'pairing request', b'pairing response', b'security request'))))
    finally:
        if process.poll() is None:
            process.kill(); process.wait(timeout=2)
        process.stdout.close()


class Producer:
    """Internal dependency injection is for deterministic tests; CLI fixes every input."""
    def __init__(self, root, identity, *, owner=0, root_object=None,
                 writer=('/usr/bin/btmon',), decoder=decode_counts,
                 cut=lambda _: None, waits=(5, 2, 2)):
        self.root, self.identity, self.owner = Path(root), identity, owner
        self.root_object, self.writer, self.decoder = root_object, writer, decoder
        self.cut, self.waits = cut, waits

    def failed(self, req, code, writer=None):
        return {'schema': 2, 'request': req, 'helper': self.identity, 'status': 'FAILED',
                'error': code, 'raw': None, 'writer': writer,
                'counts': {'pairing_requests': 0, 'pairing_responses': 0, 'security_requests': 0}}

    def finalize_fd(self, directory, fd, req, stopped):
        directory.check_file('raw-hci', fd, self.owner)
        before = os.fstat(fd)
        c.need(c.object_identity(before) == stopped['object'] and
               list(c.signature(before)) == stopped['stat'], 'CAPTURE_OBJECT_CHANGED')
        c.need(stopped['writer']['reaped'], 'WRITER_ALIVE')
        # No mutation precedes the regular-file/link-count/object checks above.
        if stat.S_IMODE(before.st_mode) != 0o600:
            os.fchmod(fd, 0o600)
        os.fsync(fd)
        digest, s = c.stable_hash(fd)
        directory.check_file('raw-hci', fd, self.owner)
        accepted = c.writer_valid(stopped['writer'])
        counts = self.decoder(fd) if accepted else self.failed(req, 'WRITER_NOT_ACCEPTED')['counts']
        directory.check_file('raw-hci', fd, self.owner)
        c.need(c.signature(s) == c.signature(os.fstat(fd)), 'RAW_CHANGED')
        raw = {'relative_name': req['run_id'] + '/' + req['capture_id'] + '/raw-hci',
               'object': c.object_identity(s), 'sha256': digest, 'bytes': s.st_size,
               'mode': format(stat.S_IMODE(s.st_mode), '04o'), 'uid': s.st_uid, 'gid': s.st_gid,
               'mtime_ns': s.st_mtime_ns, 'ctime_ns': s.st_ctime_ns}
        ok = accepted and s.st_size > 0
        return {'schema': 2, 'request': req, 'helper': self.identity,
                'status': 'FINALIZED' if ok else 'FAILED', 'error': None if ok else 'WRITER_NOT_ACCEPTED',
                'raw': raw, 'writer': stopped['writer'], 'counts': counts}

    def operate(self, req, *, capture=False, ready=lambda _: None, control=lambda _: False):
        c.request(req); c.need(req['helper'] == self.identity, 'HELPER_IDENTITY_MISMATCH')
        with c.Directory(self.root) as directory:
            s = os.fstat(directory.fd)
            c.need(s.st_uid == self.owner and stat.S_IMODE(s.st_mode) == 0o700, 'ROOT_AUTHORITY_INVALID')
            if self.root_object is not None:
                c.need(c.object_identity(s) == self.root_object, 'ROOT_OBJECT_CHANGED')
            for part in (req['run_id'], req['capture_id']):
                directory.check(); directory.child(part, create=capture)
                s = os.fstat(directory.fd)
                c.need(s.st_uid == self.owner and stat.S_IMODE(s.st_mode) == 0o700,
                       'DIRECTORY_AUTHORITY_INVALID')
            lock = os.open('lock', os.O_RDONLY | (os.O_CREAT if capture else 0) | os.O_NOFOLLOW,
                           0o400, dir_fd=directory.fd)
            try:
                directory.check_file('lock', lock, self.owner)
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as exc:
                    raise c.EvidenceError('CAPTURE_ACTIVE') from exc
                old = c.optional_json(directory, 'request.json')
                if old is not None:
                    c.need(c.encode(old) == c.encode(req), 'IMMUTABLE_CONFLICT')
                    return self.recover(directory, req)
                c.need(capture, 'CAPTURE_MISSING')
                c.write_once(directory, 'request.json', req)
                return self.capture(directory, req, ready, control)
            finally:
                os.close(lock)

    def capture(self, directory, req, ready, control):
        fd = os.open('raw-hci', os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory.fd)
        process = None; requested = False
        try:
            directory.check_file('raw-hci', fd, self.owner)
            process = subprocess.Popen([*self.writer, '-w', '/proc/self/fd/' + str(fd)],
                                       pass_fds=(fd,), stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV,
                                       preexec_fn=child_death_signal(os.getpid()))
            try:
                deadline = time.monotonic() + min(2, req['max_seconds'])
                while process.poll() is None and os.fstat(fd).st_size < 16 and time.monotonic() < deadline:
                    time.sleep(.01)
                c.need(process.poll() is None and os.fstat(fd).st_size >= 16, 'WRITER_NOT_STARTED')
                directory.check_file('raw-hci', fd, self.owner)
                ready({'schema': 2, 'event': 'READY', 'request': req, 'helper': self.identity,
                       'object': c.object_identity(os.fstat(fd))})
                requested = bool(control(process))
            except (c.EvidenceError, OSError):
                requested = False
            finally:
                writer = stop_writer(process, requested, process.poll() is not None, self.waits)
            directory.check_file('raw-hci', fd, self.owner)
            stopped = {'object': c.object_identity(os.fstat(fd)), 'stat': list(c.signature(os.fstat(fd))),
                       'writer': writer}
            c.write_once(directory, 'stopped.json', stopped); self.cut('writer_stop')
            value = self.finalize_fd(directory, fd, req, stopped)
            c.write_once(directory, 'receipt.json', value); self.cut('receipt')
            return value
        finally:
            if process is not None and process.poll() is None:
                stop_writer(process, False, False, self.waits)
            os.close(fd)

    def recover(self, directory, req):
        old = c.optional_json(directory, 'receipt.json')
        stopped = c.optional_json(directory, 'stopped.json')
        if stopped is None:
            # No numeric-PID recovery: an interrupted producer cannot certify its writer lifetime.
            value = self.failed(req, 'CAPTURE_INTERRUPTED')
        else:
            fd = os.open('raw-hci', os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory.fd)
            try:
                value = self.finalize_fd(directory, fd, req, stopped)
            finally:
                os.close(fd)
        if old is not None:
            c.need(c.encode(old) == c.encode(value), 'RECEIPT_CHANGED')
        else:
            c.write_once(directory, 'receipt.json', value)
        return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('capture', 'verify'))
    parser.add_argument('run_id'); parser.add_argument('capture_id')
    args = parser.parse_args()
    c.run_id(args.run_id); c.capture_id(args.capture_id)
    c.need(os.getuid() == 0 and os.getgid() == 0 and Path(__file__) == INSTALL / 'privileged_evidence.py',
           'EXECUTION_AUTHORITY_INVALID')
    config = c.decode(protected_bytes(INSTALL / 'root.json'))
    c.exact(config, 'device inode')
    req = c.request(line(0, 5))
    c.need(req['run_id'] == args.run_id and req['capture_id'] == args.capture_id, 'REQUEST_BINDING_MISMATCH')
    def output(value):
        sys.stdout.buffer.write(c.encode(value)); sys.stdout.buffer.flush()
    def control(process):
        deadline = time.monotonic() + req['max_seconds']
        while time.monotonic() < deadline and process.poll() is None:
            if select.select([0], [], [], .05)[0]:
                return line(0, min(2, max(.01, deadline - time.monotonic()))) == {'command': 'STOP'}
        return False
    value = Producer(EVIDENCE_ROOT, CODE_IDENTITY, root_object=config).operate(
        req, capture=args.command == 'capture', ready=output, control=control)
    output(c.receipt(value, req))


if __name__ == '__main__':
    try:
        main()
    except (c.EvidenceError, OSError, subprocess.SubprocessError) as exc:
        code = str(exc) if isinstance(exc, c.EvidenceError) else 'PRODUCER_FAILED'
        sys.stdout.buffer.write(c.encode({'schema': 2, 'error': code})); sys.stdout.buffer.flush()
        raise SystemExit(1)
