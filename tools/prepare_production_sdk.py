#!/usr/bin/env python3
"""Prepare or verify the exact isolated production SDK. Never patches input SDK."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

NIMBLE = Path('components/bt/host/nimble/nimble')
CONTROLLER = Path('components/bt/controller/lib_esp32c3_family')
SOURCE = NIMBLE / 'nimble/host/src/ble_gap.c'
ARCHIVE = CONTROLLER / 'esp32s3/libbtdm_app.a'
ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(root, *args):
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    if result.returncode:
        raise ValueError('SDK Git operation failed')
    return result.stdout.strip()


def contract(root=ROOT):
    lock = json.loads((root / 'firmware/production-sdk.lock.json').read_text())
    patch = root / 'tools/sdk-patches/nimble-dle-last.patch'
    if digest(patch) != lock['patch_sha256']:
        raise ValueError('wrong patch hash')
    return lock, patch


def repositories(sdk, revision):
    yield sdk, revision
    for line in git(sdk, 'ls-tree', '-r', revision).splitlines():
        metadata, name = line.split('\t', 1)
        mode, _, commit = metadata.split()
        if mode == '160000':
            yield from repositories(sdk / name, commit)


def verify(sdk, root=ROOT, patched=True):
    sdk = sdk.resolve()
    lock, _ = contract(root)
    for path, key in ((Path('.'), 'idf_revision'), (NIMBLE, 'nimble_revision'),
                      (CONTROLLER, 'controller_revision')):
        if git(sdk / path, 'rev-parse', 'HEAD') != lock[key]:
            raise ValueError('wrong dependency base')
    if digest(sdk / ARCHIVE) != lock['controller_archive_sha256']:
        raise ValueError('wrong controller archive')
    expected = lock['post_source_sha256' if patched else 'pre_source_sha256']
    if digest(sdk / SOURCE) != expected:
        raise ValueError('wrong source identity / unexpected patch state')
    for repo, revision in repositories(sdk, lock['idf_revision']):
        if git(repo, 'rev-parse', 'HEAD') != revision:
            raise ValueError('unexpected submodule revision')
        allowed = ['nimble/host/src/ble_gap.c'] if patched and repo == sdk / NIMBLE else []
        changed = git(repo, 'diff', 'HEAD', '--name-only', '--ignore-submodules=all').splitlines()
        if changed != allowed or git(repo, 'diff', '--cached', '--name-only'):
            raise ValueError('additional dependency modification')
        if git(repo, 'ls-files', '--others', '--exclude-standard'):
            raise ValueError('unapproved untracked dependency source')
    return lock


def clone(source, target, revision):
    # Local object transfer does not carry source/index modifications. No shared
    # object alternates: the prepared tree remains independent of its input.
    subprocess.run(['git', 'clone', '--no-hardlinks', '--no-checkout', str(source), str(target)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    git(target, 'checkout', '--detach', revision)
    for line in git(source, 'ls-tree', '-r', revision).splitlines():
        metadata, name = line.split('\t', 1)
        mode, _, commit = metadata.split()
        if mode == '160000':
            clone(source / name, target / name, commit)


def prepare(source, target, root=ROOT):
    source, target = source.resolve(), target.resolve()
    if target.exists() or source == target or source in target.parents:
        raise ValueError('output must be a new isolated SDK directory')
    lock = verify(source, root, patched=False)
    clone(source, target, lock['idf_revision'])
    git(target, 'submodule', 'absorbgitdirs')
    _, patch = contract(root)
    git(target / NIMBLE, 'apply', '--check', str(patch))
    git(target / NIMBLE, 'apply', str(patch))
    return verify(target, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.verify and not (args.source or args.output):
        verify(args.verify)
    elif args.source and args.output and not args.verify:
        prepare(args.source, args.output)
    else:
        parser.error('use --verify SDK or --source STOCK_SDK --output NEW_SDK')
    print('PASS: exact production DLE-LAST dependency')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f'production SDK verification failed: {exc}', file=sys.stderr)
        sys.exit(1)
