"""Atomic content-addressed import and sealed-tree warm verification."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile

from common import ROOT, DIGEST, InfraError, atomic_json, check_mode, cli_error, fingerprint, fsync_dir, lock, need, no_links, private_dir, read_json, sha, tree_files, verify_marker

KINDS = {'artifacts': 'cache/artifacts', 'tooling': 'cache/tooling', 'references': 'private/references'}

def destination(root, kind, digest):
    need(kind in KINDS and DIGEST.fullmatch(digest), 'CACHE_KEY_INVALID')
    return no_links(Path(root) / KINDS[kind] / digest)

def verify_full(payload, digest, count=None, size=None):
    files = tree_files(payload)
    need(sha(payload / 'SHA256SUMS') == digest, 'MANIFEST_DIGEST_MISMATCH')
    names = set()
    for line in (payload / 'SHA256SUMS').read_text().splitlines():
        need(len(line) > 66 and line[64:66] in ('  ', ' *'), 'MANIFEST_INVALID')
        expected, name = line[:64], line[66:]
        path = PurePosixPath(name)
        need(DIGEST.fullmatch(expected) and not path.is_absolute() and '..' not in path.parts
             and str(path) == name and name != 'SHA256SUMS' and name not in names, 'MANIFEST_INVALID')
        names.add(name)
        need(sha(payload / name) == expected, 'CACHE_CONTENT_MISMATCH')
    need(names | {'SHA256SUMS'} == {str(p.relative_to(payload)) for p in files}, 'CACHE_FILE_SET_MISMATCH')
    actual_size = sum(p.stat().st_size for p in files)
    need(count is None or count == len(files), 'CACHE_FILE_COUNT_MISMATCH')
    need(size is None or size == actual_size, 'CACHE_BYTE_COUNT_MISMATCH')
    return len(files), actual_size

def modes(payload, kind):
    file_mode, dir_mode = (0o600, 0o700) if kind == 'references' else (0o400, 0o500)
    for p in [payload, *payload.rglob('*')]:
        check_mode(p, dir_mode if p.is_dir() else file_mode)

def verify(root, kind, digest):
    entry = destination(root, kind, digest)
    need(entry.is_dir(), 'CACHE_MISS')
    check_mode(entry, 0o700)
    payload, marker = entry / 'payload', entry / 'VERIFIED.json'
    full = True
    metadata = None
    if marker.exists():
        try:
            check_mode(marker, 0o600)
            metadata = read_json(marker)
            need(set(metadata) == {'schema', 'digest', 'kind', 'files', 'bytes', 'fingerprint'}, 'CACHE_METADATA_INVALID')
            modes(payload, kind)
            full = not (metadata['schema'] == 1 and metadata['digest'] == digest and metadata['kind'] == kind
                        and metadata['fingerprint'] == fingerprint(payload))
        except (InfraError, OSError, ValueError):
            full = True
    if full:
        # Never repair permissions or bless altered bytes in a warm path.
        count, size = verify_full(payload, digest)
        modes(payload, kind)
        if marker.exists():
            check_mode(marker, 0o600)
        metadata = {'schema': 1, 'digest': digest, 'kind': kind, 'files': count, 'bytes': size, 'fingerprint': fingerprint(payload)}
        atomic_json(marker, metadata)
    return {'digest': digest, 'kind': kind, 'classification': 'CACHE_FULL_VERIFIED' if full else 'CACHE_HIT',
            'files': metadata['files'], 'bytes': metadata['bytes']}

def import_tree(root, kind, digest, source, count=None, size=None):
    source = no_links(source)
    final = destination(root, kind, digest)
    with lock(root, 'cache-import'):
        if final.exists():
            result = verify(root, kind, digest)
            need(count is None or count == result['files'], 'CACHE_FILE_COUNT_MISMATCH')
            need(size is None or size == result['bytes'], 'CACHE_BYTE_COUNT_MISMATCH')
            return result
        files, total = verify_full(source, digest, count, size)
        temp = Path(tempfile.mkdtemp(prefix='.import-', dir=final.parent))
        try:
            payload = temp / 'payload'
            shutil.copytree(source, payload, symlinks=True)
            verify_full(payload, digest, files, total)
            file_mode, dir_mode = (0o600, 0o700) if kind == 'references' else (0o400, 0o500)
            for p in [payload, *payload.rglob('*')]:
                os.chmod(p, dir_mode if p.is_dir() else file_mode)
                if p.is_file():
                    with p.open('rb') as stream:
                        os.fsync(stream.fileno())
            for p in reversed([payload, *sorted(q for q in payload.rglob('*') if q.is_dir())]):
                fsync_dir(p)
            atomic_json(temp / 'VERIFIED.json', {'schema': 1, 'digest': digest, 'kind': kind,
                         'files': files, 'bytes': total, 'fingerprint': fingerprint(payload)})
            os.rename(temp, final)
            fsync_dir(final.parent)
        finally:
            if temp.exists():
                for p in [temp, *temp.rglob('*')]:
                    if p.is_dir() and not p.is_symlink():
                        os.chmod(p, 0o700)
                shutil.rmtree(temp)
        return verify(root, kind, digest)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('import', 'verify'))
    parser.add_argument('kind', choices=KINDS)
    parser.add_argument('digest')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--files', type=int)
    parser.add_argument('--bytes', type=int)
    args = parser.parse_args()
    verify_marker()
    if args.command == 'import':
        need(args.source is not None, 'SOURCE_REQUIRED')
        value = import_tree(ROOT, args.kind, args.digest, args.source, args.files, args.bytes)
    else:
        value = verify(ROOT, args.kind, args.digest)
    print(json.dumps({'schema': 1, 'ok': True, **value}))
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        sys.exit(cli_error(exc))
