"""Version 2 evidence schema and descriptor-relative filesystem primitives."""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat

SCHEMA = 2
HELPER_VERSION = 2
RUN = re.compile(r'[0-9]{8}T[0-9]{6}Z-[0-9a-f]{12}\Z', re.ASCII)
CAPTURE = re.compile(r'[0-9a-f]{32}\Z', re.ASCII)
DIGEST = re.compile(r'[0-9a-f]{64}\Z', re.ASCII)
KINDS = ('Q8_HCI', 'REHEARSAL_HCI')
MAX_JSON = 1024 * 1024


class EvidenceError(RuntimeError):
    pass


def need(ok, code):
    if not ok:
        raise EvidenceError(code)


def exact(value, keys, code='SCHEMA_INVALID'):
    need(type(value) is dict and set(value) == set(keys.split()), code)


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def matches(pattern, value):
    return type(value) is str and pattern.fullmatch(value) is not None


def run_id(value):
    need(matches(RUN, value), 'RUN_ID_INVALID')
    return value


def capture_id(value):
    need(matches(CAPTURE, value), 'CAPTURE_ID_INVALID')
    return value


def encode(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n').encode()


def digest(value):
    return hashlib.sha256(encode(value)).hexdigest()


def decode(data):
    def pairs(items):
        value = {}
        for key, item in items:
            need(key not in value, 'DUPLICATE_JSON_KEY')
            value[key] = item
        return value
    need(isinstance(data, (str, bytes)) and len(data) <= MAX_JSON, 'JSON_SIZE_INVALID')
    try:
        return json.loads(data, object_pairs_hook=pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(EvidenceError('JSON_INVALID')))
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise EvidenceError('JSON_INVALID') from exc


def identity(value):
    exact(value, 'version helper_sha256 imports')
    need(type(value['version']) is int and value['version'] == HELPER_VERSION, 'HELPER_VERSION_INVALID')
    need(matches(DIGEST, value['helper_sha256']), 'HELPER_DIGEST_INVALID')
    exact(value['imports'], 'evidence_contract.py')
    need(matches(DIGEST, value['imports']['evidence_contract.py']), 'IMPORT_DIGEST_INVALID')
    return value


def request(value):
    exact(value, 'schema attempt_id run_id capture_id kind authority_sha256 helper max_seconds')
    need(type(value['schema']) is int and value['schema'] == SCHEMA, 'SCHEMA_VERSION_INVALID')
    run_id(value['attempt_id']); run_id(value['run_id']); capture_id(value['capture_id'])
    need(value['kind'] in KINDS, 'EVIDENCE_KIND_INVALID')
    need(matches(DIGEST, value['authority_sha256']), 'AUTHORITY_DIGEST_INVALID')
    identity(value['helper'])
    need(integer(value['max_seconds'], 1) and value['max_seconds'] <= 3600, 'DURATION_INVALID')
    return value


def object_identity(s):
    return {'device': s.st_dev, 'inode': s.st_ino}


def signature(s):
    return (s.st_dev, s.st_ino, s.st_mode, s.st_nlink, s.st_uid, s.st_gid,
            s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def regular(s, owner=None):
    need(stat.S_ISREG(s.st_mode) and s.st_nlink == 1, 'RAW_OBJECT_INVALID')
    if owner is not None:
        need(s.st_uid == owner, 'RAW_OWNER_INVALID')


def stable_hash(fd):
    before = os.fstat(fd); regular(before)
    os.lseek(fd, 0, os.SEEK_SET)
    value = hashlib.sha256()
    for data in iter(lambda: os.read(fd, 1024 * 1024), b''):
        value.update(data)
    after = os.fstat(fd)
    need(signature(before) == signature(after), 'RAW_CHANGED')
    return value.hexdigest(), after


class Directory:
    """Keep every ancestor FD; verify all canonical names without following links."""
    def __init__(self, path):
        path = Path(path)
        need(path.is_absolute() and all(x not in ('.', '..') for x in path.parts), 'ROOT_INVALID')
        self.fds = [os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)]
        self.names = []
        try:
            for name in path.parts[1:]:
                self.child(name)
        except BaseException:
            self.close()
            raise

    @property
    def fd(self):
        return self.fds[-1]

    def child(self, name, create=False):
        need(type(name) is str and name not in ('', '.', '..') and '/' not in name and '\\' not in name,
             'BASENAME_INVALID')
        if create:
            try:
                os.mkdir(name, 0o700, dir_fd=self.fd)
                os.fsync(self.fd)
            except FileExistsError:
                pass
        fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=self.fd)
        self.names.append(name); self.fds.append(fd)
        return self

    def check(self):
        for parent, name, fd in zip(self.fds, self.names, self.fds[1:]):
            entry = os.stat(name, dir_fd=parent, follow_symlinks=False)
            need(stat.S_ISDIR(entry.st_mode) and object_identity(entry) == object_identity(os.fstat(fd)),
                 'NAMESPACE_CHANGED')

    def check_file(self, name, fd, owner=None):
        self.check()
        entry = os.stat(name, dir_fd=self.fd, follow_symlinks=False)
        current = os.fstat(fd)
        regular(entry, owner); regular(current, owner)
        need(object_identity(entry) == object_identity(current), 'RAW_NAME_CHANGED')

    def close(self):
        for fd in reversed(self.fds):
            os.close(fd)
        self.fds.clear()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def read_bytes(directory, name):
    directory.check()
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory.fd)
    try:
        regular(os.fstat(fd))
        data = b''
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            data += chunk
            need(len(data) <= MAX_JSON, 'JSON_SIZE_INVALID')
        directory.check_file(name, fd)
        return data
    finally:
        os.close(fd)


def read_json(directory, name):
    return decode(read_bytes(directory, name))


def optional_json(directory, name):
    try:
        return read_json(directory, name)
    except FileNotFoundError:
        return None


def write_once(directory, name, value, mode=0o400):
    """Caller holds its transaction flock. Atomic publication, equality on retry."""
    directory.check()
    old = optional_json(directory, name)
    if old is not None:
        need(encode(old) == encode(value), 'IMMUTABLE_CONFLICT')
        return
    temporary = '.pending-' + secrets.token_hex(12)
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600,
                 dir_fd=directory.fd)
    try:
        data = encode(value)
        with os.fdopen(fd, 'wb', closefd=False) as stream:
            stream.write(data); stream.flush()
        os.fchmod(fd, mode); os.fsync(fd)
        directory.check()
        os.rename(temporary, name, src_dir_fd=directory.fd, dst_dir_fd=directory.fd)
        os.fsync(directory.fd)
    finally:
        os.close(fd)
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary, dir_fd=directory.fd)


def writer_valid(value):
    exact(value, 'pid exit_code requested_stop forced_signal reaped early_exit')
    need(integer(value['pid'], 1) and type(value['exit_code']) is int, 'WRITER_INVALID')
    need(all(type(value[k]) is bool for k in ('requested_stop', 'reaped', 'early_exit')),
         'WRITER_INVALID')
    need(value['forced_signal'] in (None, 'SIGTERM', 'SIGKILL'), 'WRITER_INVALID')
    return (value['exit_code'] == 0 and value['requested_stop'] and value['reaped']
            and not value['early_exit'] and value['forced_signal'] is None)


def receipt(value, expected):
    request(expected)
    exact(value, 'schema request helper status error raw writer counts')
    need(type(value['schema']) is int and value['schema'] == SCHEMA, 'SCHEMA_VERSION_INVALID')
    request(value['request']); identity(value['helper'])
    need(encode(value['request']) == encode(expected), 'RECEIPT_BINDING_MISMATCH')
    need(encode(value['helper']) == encode(expected['helper']), 'HELPER_IDENTITY_MISMATCH')
    need(value['status'] in ('FINALIZED', 'FAILED'), 'RECEIPT_STATUS_INVALID')
    accepted = False
    if value['writer'] is not None:
        accepted = writer_valid(value['writer'])
    if value['raw'] is not None:
        raw = value['raw']
        exact(raw, 'relative_name object sha256 bytes mode uid gid mtime_ns ctime_ns')
        need(raw['relative_name'] == expected['run_id'] + '/' + expected['capture_id'] + '/raw-hci',
             'RAW_NAME_INVALID')
        exact(raw['object'], 'device inode')
        need(all(integer(raw['object'][k]) for k in ('device', 'inode')), 'OBJECT_INVALID')
        need(matches(DIGEST, raw['sha256']) and integer(raw['bytes']), 'RAW_DIGEST_INVALID')
        need(raw['mode'] == '0600' and type(raw['uid']) is int and raw['uid'] == 0
             and type(raw['gid']) is int and raw['gid'] == 0, 'RAW_PERMISSION_INVALID')
        need(integer(raw['mtime_ns']) and integer(raw['ctime_ns']), 'RAW_STAT_INVALID')
    exact(value['counts'], 'pairing_requests pairing_responses security_requests')
    need(all(integer(x) for x in value['counts'].values()), 'COUNTS_INVALID')
    if value['status'] == 'FINALIZED':
        need(value['error'] is None and accepted and value['raw'] is not None
             and value['raw']['bytes'] > 0, 'WRITER_NOT_ACCEPTED')
    else:
        need(type(value['error']) is str and re.fullmatch(r'[A-Z_]{1,64}', value['error']) is not None,
             'EVIDENCE_ERROR_INVALID')
    return value
