"""Protected source runtime and immutable v3 authority vocabulary (no site startup)."""
from __future__ import annotations
import hashlib
import importlib.abc
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import stat
import sys
import evidence_contract as c

INSTALL = Path('/usr/local/lib/s3-hidbot-authority-v3')
STATE = Path('/var/lib/s3-hidbot-authority-v3')
ENGINE = ('authority_service.py', 'authority_runtime.py', 'runtime_launcher.py',
          'evidence_contract.py', 'privileged_evidence.py')
PRODUCT = dict(commit='8ca6a1e0ce9eea88ec15a716fffa89ffeff0bfad',
               tree='02d69f4bbf833c0315c1f557d4e56fa85bb239b9',
               firmware='851e92881463f23fce51cf7eb150465ad9b59f4e',
               host='9d756cdefd9a041288a0eb87df905f4f0a166e71',
               archive='db7c9afec9ba2a6ec210ebffc030ac61542079d7e4a29db5d026d3c542e3ece9',
               bin='0459b4604d2ede52db0d6690049623a2a8b73bab262f5e45ac3bb0c081400f60',
               elf='3b913b0ee1873e861d11ecf2dfbe88f285d60b2e58356cf3b7826ab00354c998')
CLASSIFICATION = 'EVIDENCE_PIPELINE_REHEARSAL / NOT_QUALIFICATION'
FILES = ('authority.json', 'request.json', 'test.json', 'evidence.json', 'manifest.json', 'index.json')


def sha(data): return hashlib.sha256(data).hexdigest()


def protected(directory, owner, mode=None):
    directory.check()
    s = os.fstat(directory.fd)
    c.need(s.st_uid == owner and not s.st_mode & 0o022, 'DIRECTORY_AUTHORITY_INVALID')
    if mode is not None:
        c.need(stat.S_IMODE(s.st_mode) == mode, 'DIRECTORY_MODE_INVALID')


def source(path, owner=0):
    with c.Directory(path.parent) as d:
        protected(d, owner)
        fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=d.fd)
        try:
            d.check_file(path.name, fd, owner)
            c.need(not os.fstat(fd).st_mode & 0o222, 'SOURCE_MODE_INVALID')
            digest, before = c.stable_hash(fd)
            os.lseek(fd, 0, 0)
            with os.fdopen(fd, 'rb', closefd=False) as stream: data = stream.read()
            c.need(sha(data) == digest and c.signature(before) == c.signature(os.fstat(fd)), 'SOURCE_CHANGED')
            d.check_file(path.name, fd, owner)
            return data
        finally: os.close(fd)


def engine_identity(install=INSTALL, owner=0):
    return {name: sha(source(install / name, owner)) for name in ENGINE}


def membership(root, owner=None):
    rows = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            c.need(name != '__pycache__' and not name.endswith(('.pyc', '.pyo')), 'BYTECODE_FORBIDDEN')
            s = path.lstat()
            c.need(not stat.S_ISLNK(s.st_mode), 'RUNTIME_LINK_FORBIDDEN')
            if owner is not None:
                c.need(s.st_uid == owner and not s.st_mode & 0o222, 'RUNTIME_PERMISSION_INVALID')
            if path.is_dir(): continue
            c.regular(s, owner)
            relative = path.relative_to(root).as_posix()
            if relative == 'BUNDLE.json': continue
            rows[relative] = sha(path.read_bytes()) if owner is None else sha(source(path, owner))
    return rows


def runtime(root, expected_id, expected=None, *, owner=0):
    c.need(c.matches(c.DIGEST, expected_id), 'RUNTIME_ID_INVALID')
    with c.Directory(root) as d: protected(d, owner, 0o555)
    data = source(root / 'BUNDLE.json', owner)
    c.need(sha(data) == expected_id, 'RUNTIME_AUTHORITY_CONFLICT')
    value = c.decode(data)
    c.exact(value, 'schema files engine product coordinator plan frozen')
    c.need(type(value['schema']) is int and value['schema'] == 3 and value['product'] == PRODUCT,
           'RUNTIME_SCHEMA_INVALID')
    if expected is not None:
        c.need(value == expected, 'RUNTIME_AUTHORITY_CONFLICT')
    c.need(membership(root, owner) == value['files'], 'RUNTIME_MEMBERSHIP_CHANGED')
    c.need(value['coordinator'] == value['files']['official_campaign.py'] and
           value['plan'] == value['files']['qualification-plan.md'] and
           value['frozen'] == value['files']['FROZEN.json'], 'RUNTIME_BINDING_INVALID')
    return value


def handle(value):
    c.exact(value, 'schema run_id capture_id runtime_id attempt_authority_sha256')
    c.need(type(value['schema']) is int and value['schema'] == 3, 'HANDLE_SCHEMA_INVALID')
    c.run_id(value['run_id']); c.capture_id(value['capture_id'])
    for key in ('runtime_id', 'attempt_authority_sha256'):
        c.need(c.matches(c.DIGEST, value[key]), 'HANDLE_DIGEST_INVALID')
    return value


def capture_request(value):
    c.exact(value, 'handle attempt_authority_sha256 request')
    handle(value['handle']); c.request(value['request'])
    h = value['handle']; req = value['request']
    c.need(value['attempt_authority_sha256'] == h['attempt_authority_sha256'] == req['authority_sha256']
           and req['attempt_id'] == req['run_id'] == h['run_id'] and req['capture_id'] == h['capture_id'],
           'AUTHORITY_CONFLICT')
    return value


class SourceLoader(importlib.machinery.SourceFileLoader):
    def get_code(self, fullname):
        data = source(Path(self.path))
        c.need(sha(data) == self.expected, 'EXECUTED_SOURCE_CHANGED')
        return compile(data, self.path, 'exec', dont_inherit=True)


class SourceFinder(importlib.abc.MetaPathFinder):
    def __init__(self, root, manifest): self.root, self.manifest = root, manifest
    def find_spec(self, fullname, path=None, target=None):
        locations = [self.root, self.root / 'host/src', self.root / 'tools'] if path is None else map(Path, path)
        for directory in locations:
            if not directory.is_relative_to(self.root): continue
            stem = directory / fullname.rsplit('.', 1)[-1]
            for candidate, package in ((stem / '__init__.py', True), (stem.with_suffix('.py'), False)):
                relative = candidate.relative_to(self.root).as_posix()
                if relative in self.manifest['files']:
                    loader = SourceLoader(fullname, str(candidate)); loader.expected = self.manifest['files'][relative]
                    return importlib.util.spec_from_file_location(fullname, candidate, loader=loader,
                        submodule_search_locations=[str(stem)] if package else None)
        return None


def isolate(root, manifest):
    # -I -S is mandatory at the launcher: no startup site/.pth/user/customize execution.
    c.need(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode, 'PYTHON_NOT_ISOLATED')
    trusted = [p for p in sys.path if p.startswith('/usr/lib/python3')]
    trusted += ['/usr/lib/python3/dist-packages']
    for path in trusted:
        p = Path(path)
        if p.exists():
            s = p.stat(); c.need(s.st_uid == 0 and not s.st_mode & 0o022, 'DEPENDENCY_AUTHORITY_INVALID')
    sys.path[:] = [str(root), str(root / 'host/src'), str(root / 'tools'), *trusted]
    sys.meta_path.insert(0, SourceFinder(root, manifest))
