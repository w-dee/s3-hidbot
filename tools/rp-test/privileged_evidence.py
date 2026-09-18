#!/usr/bin/env python3
"""Narrow root boundary for a bounded btmon capture and immutable receipt."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
import sys
import time
from typing import Any, Callable

from common import InfraError, atomic_json, fsync_dir, need, no_links, read_json


PRIVATE_BASE = Path('/srv/s3-hidbot-test/private')
PACKAGE_ID = re.compile(r'\d{8}T\d{6}Z-[0-9a-f]{12}\Z')
CAPTURE_TOKEN = re.compile(r'[0-9a-f]{32}\Z')
RELATIVE_ID = 'privileged/raw-hci'
RECEIPT_NAME = 'finalization.json'


def _authority(capsule: Path, capture_token: str) -> dict[str, Any]:
    value = read_json(capsule / 'authority.json')
    need(
        set(value) == {'schema', 'package_id', 'classification', 'capture_token', 'state'}
        and value['schema'] == 1
        and value['package_id'] == capsule.name
        and isinstance(value['classification'], str)
        and 0 < len(value['classification']) <= 128
        and value['capture_token'] == capture_token
        and value['state'] == 'CAPTURE_AUTHORIZED',
        'CAPTURE_AUTHORITY_INVALID',
    )
    return value


def validate_capsule_path(capsule: Path | str, *, private_base: Path = PRIVATE_BASE) -> Path:
    """Accept one new run directory exactly two levels below the private base."""

    base = no_links(private_base)
    path = no_links(capsule)
    need(base.is_absolute() and path.is_absolute(), 'CAPSULE_PATH_INVALID')
    try:
        relative = path.relative_to(base)
    except ValueError as exc:
        raise InfraError('CAPSULE_PATH_INVALID') from exc
    need(len(relative.parts) == 2 and PACKAGE_ID.fullmatch(relative.parts[1]), 'CAPSULE_PATH_INVALID')
    need(path.is_dir(), 'CAPSULE_PATH_INVALID')
    return path


def _stat_identity(value: os.stat_result) -> tuple[int, ...]:
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_nlink,
        value.st_uid,
        value.st_gid,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _stable_hash_fd(fd: int) -> tuple[str, os.stat_result]:
    before = os.fstat(fd)
    need(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, 'CAPTURE_FILE_INVALID')
    digest = hashlib.sha256()
    os.lseek(fd, 0, os.SEEK_SET)
    for chunk in iter(lambda: os.read(fd, 1024 * 1024), b''):
        digest.update(chunk)
    after = os.fstat(fd)
    need(_stat_identity(before) == _stat_identity(after), 'CAPTURE_CHANGED_DURING_FINALIZATION')
    return digest.hexdigest(), after


def validate_termination(value: dict[str, Any]) -> None:
    need(
        set(value) == {
            'pid', 'exit_code', 'requested_signal', 'forced_signal',
            'termination_confirmed', 'early_exit',
        }
        and type(value['pid']) is int and value['pid'] > 0
        and type(value['exit_code']) is int
        and value['requested_signal'] in ('SIGINT', None)
        and value['forced_signal'] in ('SIGTERM', 'SIGKILL', None)
        and type(value['termination_confirmed']) is bool
        and type(value['early_exit']) is bool,
        'CAPTURE_TERMINATION_INVALID',
    )
    need(value['termination_confirmed'], 'CAPTURE_WRITER_STILL_RUNNING')


def validate_receipt(
    value: dict[str, Any],
    *,
    capture_token: str,
    expected_sha256: str | None = None,
    expected_bytes: int | None = None,
    require_root_owner: bool = True,
) -> dict[str, Any]:
    need(
        isinstance(value, dict)
        and set(value) == {
            'schema', 'capture_token', 'relative_id', 'sha256', 'bytes', 'mode',
            'owner_uid', 'owner_gid', 'stable_stat', 'writer',
        }
        and value['schema'] == 1
        and value['capture_token'] == capture_token
        and value['relative_id'] == RELATIVE_ID
        and re.fullmatch(r'[0-9a-f]{64}', value['sha256'])
        and type(value['bytes']) is int and value['bytes'] > 0
        and value['mode'] == '0600'
        and type(value['owner_uid']) is int and value['owner_uid'] >= 0
        and type(value['owner_gid']) is int and value['owner_gid'] >= 0
        and value['stable_stat'] is True,
        'CAPTURE_RECEIPT_INVALID',
    )
    if require_root_owner:
        need(value['owner_uid'] == 0, 'CAPTURE_OWNER_INVALID')
    validate_termination(value['writer'])
    if expected_sha256 is not None:
        need(value['sha256'] == expected_sha256, 'CAPTURE_DIGEST_MISMATCH')
    if expected_bytes is not None:
        need(value['bytes'] == expected_bytes, 'CAPTURE_SIZE_MISMATCH')
    return value


def finalize_stopped_capture(
    raw: Path | str,
    *,
    capture_token: str,
    termination: dict[str, Any],
    hash_fd: Callable[[int], tuple[str, os.stat_result]] = _stable_hash_fd,
    open_file: Callable[..., int] = os.open,
) -> dict[str, Any]:
    """Finalize only after the owning controller has reaped the writer."""

    need(CAPTURE_TOKEN.fullmatch(capture_token), 'CAPTURE_TOKEN_INVALID')
    validate_termination(termination)
    try:
        fd = open_file(no_links(raw), os.O_RDONLY | os.O_NOFOLLOW)
    except FileNotFoundError as exc:
        raise InfraError('CAPTURE_MISSING') from exc
    except OSError as exc:
        raise InfraError('CAPTURE_STAT_FAILED') from exc
    try:
        os.fchmod(fd, 0o600)
        try:
            digest, final = hash_fd(fd)
        except InfraError:
            raise
        except OSError as exc:
            raise InfraError('CAPTURE_HASH_FAILED') from exc
        need(final.st_size > 0, 'CAPTURE_EMPTY')
        need(stat.S_IMODE(final.st_mode) == 0o600, 'CAPTURE_PERMISSION_INVALID')
        need(final.st_uid == os.geteuid(), 'CAPTURE_OWNER_INVALID')
    finally:
        os.close(fd)
    receipt = {
        'schema': 1,
        'capture_token': capture_token,
        'relative_id': RELATIVE_ID,
        'sha256': digest,
        'bytes': final.st_size,
        'mode': '0600',
        'owner_uid': final.st_uid,
        'owner_gid': final.st_gid,
        'stable_stat': True,
        'writer': termination,
    }
    return validate_receipt(
        receipt, capture_token=capture_token,
        require_root_owner=os.geteuid() == 0,
    )


def _stop_writer(process: subprocess.Popen[bytes], *, early_exit: bool) -> dict[str, Any]:
    forced: str | None = None
    requested: str | None = None
    if process.poll() is None:
        requested = 'SIGINT'
        os.killpg(process.pid, signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            forced = 'SIGTERM'
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                forced = 'SIGKILL'
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)
    return {
        'pid': process.pid,
        'exit_code': process.returncode,
        'requested_signal': requested,
        'forced_signal': forced,
        'termination_confirmed': process.poll() is not None,
        'early_exit': early_exit,
    }


def verify_finalized(
    capsule: Path | str,
    *,
    capture_token: str,
    private_base: Path = PRIVATE_BASE,
    require_root: bool = True,
) -> dict[str, Any]:
    """Re-hash one already-finalized object without capture or mutation."""

    if require_root:
        need(os.geteuid() == 0, 'PRIVILEGED_HELPER_REQUIRES_ROOT')
    need(CAPTURE_TOKEN.fullmatch(capture_token), 'CAPTURE_TOKEN_INVALID')
    path = validate_capsule_path(capsule, private_base=private_base)
    _authority(path, capture_token)
    privileged = no_links(path / 'privileged')
    raw = no_links(path / RELATIVE_ID)
    receipt_path = no_links(privileged / RECEIPT_NAME)
    need(receipt_path.is_file(), 'FINALIZATION_RECEIPT_MISSING')
    receipt = validate_receipt(
        read_json(receipt_path), capture_token=capture_token,
        require_root_owner=require_root,
    )
    fd = os.open(raw, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        digest, current = _stable_hash_fd(fd)
    finally:
        os.close(fd)
    need(
        digest == receipt['sha256'] and current.st_size == receipt['bytes']
        and stat.S_IMODE(current.st_mode) == 0o600
        and current.st_uid == receipt['owner_uid'] and current.st_gid == receipt['owner_gid'],
        'FINALIZED_CAPTURE_CHANGED',
    )
    return receipt


def capture_and_finalize(
    capsule: Path | str,
    *,
    capture_token: str,
    duration: float,
    private_base: Path = PRIVATE_BASE,
    command: tuple[str, ...] = ('/usr/bin/btmon',),
    process_factory: Callable[..., subprocess.Popen[bytes]] = subprocess.Popen,
    sleeper: Callable[[float], None] = time.sleep,
    require_root: bool = True,
) -> dict[str, Any]:
    """Own the writer from exclusive creation through wait, hash and receipt."""

    if require_root:
        need(os.geteuid() == 0, 'PRIVILEGED_HELPER_REQUIRES_ROOT')
    need(CAPTURE_TOKEN.fullmatch(capture_token), 'CAPTURE_TOKEN_INVALID')
    need(0 < duration <= 3600, 'CAPTURE_DURATION_INVALID')
    path = validate_capsule_path(capsule, private_base=private_base)
    _authority(path, capture_token)
    privileged = no_links(path / 'privileged')
    raw = no_links(path / RELATIVE_ID)
    receipt_path = no_links(privileged / RECEIPT_NAME)
    if receipt_path.is_file():
        return verify_finalized(
            path, capture_token=capture_token, private_base=private_base,
            require_root=require_root,
        )
    need(not privileged.exists(), 'PRIVILEGED_EVIDENCE_STATE_INVALID')
    privileged.mkdir(mode=0o700)
    need(not raw.exists(), 'CAPTURE_ALREADY_EXISTS')
    fd = os.open(raw, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(fd)
    process = process_factory(
        [*command, '-w', str(raw)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    early = False
    try:
        sleeper(duration)
        early = process.poll() is not None
    finally:
        termination = _stop_writer(process, early_exit=early)
    need(not early, 'CAPTURE_WRITER_EXITED_EARLY')
    receipt = finalize_stopped_capture(
        raw, capture_token=capture_token, termination=termination,
    )
    atomic_json(receipt_path, receipt)
    os.chmod(receipt_path, 0o600)
    fsync_dir(privileged)
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='command', required=True)
    capture = sub.add_parser('capture')
    capture.add_argument('--capsule', required=True, type=Path)
    capture.add_argument('--capture-token', required=True)
    capture.add_argument('--duration', required=True, type=float)
    verify = sub.add_parser('verify-finalized')
    verify.add_argument('--capsule', required=True, type=Path)
    verify.add_argument('--capture-token', required=True)
    args = parser.parse_args()
    if args.command == 'capture':
        receipt = capture_and_finalize(
            args.capsule, capture_token=args.capture_token, duration=args.duration,
        )
    else:
        receipt = verify_finalized(args.capsule, capture_token=args.capture_token)
    print(json.dumps(receipt, sort_keys=True, separators=(',', ':')))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        code = str(exc) if isinstance(exc, InfraError) else 'PRIVILEGED_EVIDENCE_FAILED'
        print(json.dumps({'schema': 1, 'error': code}), file=sys.stderr)
        sys.exit(1)
