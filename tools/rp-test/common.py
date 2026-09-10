"""Private filesystem primitives. No hardware or network access."""
from __future__ import annotations

import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile

ROOT = Path('/srv/s3-hidbot-test')
MARKER = Path('/etc/s3-hidbot-test-host')
MARKER_VALUE = {'schema': 1, 'role': 's3-hidbot-qualification', 'hardware': 'raspberry-pi-4', 'arch': 'aarch64'}
LAYOUT = ('toolchains', 'cache', 'cache/artifacts', 'cache/tooling', 'private', 'private/references', 'runs', 'state', 'tmp')
MIN_FREE = 2 * 1024 ** 3
DIGEST = re.compile(r'[0-9a-f]{64}\Z')
TOKEN = re.compile(r'[A-Za-z][A-Za-z0-9_.-]{0,63}\Z')

class InfraError(RuntimeError):
    pass

def need(value, code):
    if not value:
        raise InfraError(code)

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()

def no_links(path):
    path = Path(path).absolute()
    for parent in (*reversed(path.parents), path):
        need(not parent.is_symlink(), 'SYMLINK_REFUSED')
    return path

def private_dir(path):
    path = no_links(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    need(path.is_dir(), 'DIRECTORY_REQUIRED')
    os.chmod(path, 0o700)
    return path

def check_mode(path, mode):
    path = no_links(path)
    need(path.exists() and stat.S_IMODE(path.stat().st_mode) == mode, 'PERMISSION_INVALID')

def read_json(path):
    path = no_links(path)
    def pairs(items):
        out = {}
        for key, value in items:
            need(key not in out, 'DUPLICATE_JSON_KEY')
            out[key] = value
        return out
    return json.loads(path.read_text(), object_pairs_hook=pairs)

def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def atomic_json(path, value):
    path = no_links(path)
    private_dir(path.parent)
    data = (json.dumps(value, sort_keys=True, allow_nan=False, separators=(',', ':')) + '\n').encode()
    fd, temporary = tempfile.mkstemp(prefix='.atomic-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fsync_dir(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def tree_files(root):
    root = no_links(root)
    need(root.is_dir(), 'TREE_MISSING')
    files = []
    for p in sorted(root.rglob('*')):
        need(not p.is_symlink(), 'SYMLINK_REFUSED')
        need(p.is_file() or p.is_dir(), 'SPECIAL_FILE_REFUSED')
        if p.is_file():
            need(p.stat().st_nlink == 1, 'HARDLINK_REFUSED')
            files.append(p)
    return files

def fingerprint(root):
    """Detect inode, content metadata, permission and tree membership changes."""
    root = no_links(root)
    tree_files(root)
    h = hashlib.sha256()
    for p in [root, *sorted(root.rglob('*'))]:
        s = p.stat()
        row = [str(p.relative_to(root)), s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns, s.st_mode, s.st_uid, s.st_gid]
        h.update(json.dumps(row).encode())
    return h.hexdigest()

def verify_marker(marker=MARKER):
    marker = no_links(marker)
    need(marker.is_file(), 'HOST_MARKER_MISSING')
    s = marker.stat()
    need(s.st_uid == 0 and not s.st_mode & 0o022, 'HOST_MARKER_UNSAFE')
    need(read_json(marker) == MARKER_VALUE, 'HOST_MARKER_MISMATCH')

@contextlib.contextmanager
def lock(root, name, blocking=False):
    need(TOKEN.fullmatch(name), 'LOCK_NAME_INVALID')
    path = no_links(Path(root) / 'state' / (name + '.lock'))
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        need(stat.S_IMODE(os.fstat(fd).st_mode) == 0o600, 'LOCK_PERMISSION_INVALID')
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        except BlockingIOError:
            raise InfraError('PHYSICAL_RUN_LOCKED' if name == 'physical' else 'INFRA_LOCKED') from None
        yield fd
    finally:
        os.close(fd)

def cli_error(exc):
    code = str(exc) if isinstance(exc, InfraError) and TOKEN.fullmatch(str(exc)) else 'INFRA_FAILURE'
    print(json.dumps({'schema': 1, 'ok': False, 'classification': code}))
    return 2
