"""Unprivileged terminal packaging for privileged opaque HCI evidence."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import stat
import subprocess
from typing import Any, Sequence

from common import InfraError, atomic_json, fsync_dir, need, no_links, read_json, sha
from privileged_evidence import CAPTURE_TOKEN, PACKAGE_ID, validate_receipt


CLASSIFICATION = 'EVIDENCE_PIPELINE_REHEARSAL / NOT_QUALIFICATION'
FINALIZATION_ERRORS = {
    'CAPTURE_HELPER_FAILED', 'CAPTURE_MISSING', 'CAPTURE_EMPTY',
    'CAPTURE_WRITER_STILL_RUNNING', 'CAPTURE_HASH_FAILED', 'CAPTURE_STAT_FAILED',
    'CAPTURE_CHANGED_DURING_FINALIZATION', 'CAPTURE_RECEIPT_INVALID',
    'CAPTURE_DIGEST_MISMATCH', 'CAPTURE_SIZE_MISMATCH',
}


def utc() -> str:
    return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')


def create_package(base: Path | str, classification: str) -> tuple[Path, str]:
    base_path = no_links(base)
    base_path.mkdir(mode=0o700, parents=True, exist_ok=True)
    need(base_path.is_dir(), 'PACKAGE_BASE_INVALID')
    os.chmod(base_path, 0o700)
    package_id = utc() + '-' + secrets.token_hex(6)
    path = no_links(base_path / package_id)
    path.mkdir(mode=0o700)
    token = secrets.token_hex(16)
    atomic_json(path / 'authority.json', {
        'schema': 1,
        'package_id': package_id,
        'classification': classification,
        'capture_token': token,
        'state': 'CAPTURE_AUTHORIZED',
    })
    return path, token


def invoke_privileged_capture(
    helper: Path | str,
    capsule: Path | str,
    *,
    capture_token: str,
    duration: float,
    sudo: Sequence[str] = ('sudo', '-n'),
) -> dict[str, Any]:
    """Invoke one fixed helper argv; no shell and no raw-file operation here."""

    need(CAPTURE_TOKEN.fullmatch(capture_token), 'CAPTURE_TOKEN_INVALID')
    command = [*sudo, '/usr/bin/python3', str(no_links(helper)), 'capture',
               '--capsule', str(no_links(capsule)), '--capture-token', capture_token,
               '--duration', str(duration)]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=duration + 15)
    need(completed.returncode == 0, 'CAPTURE_HELPER_FAILED')
    try:
        value = json.loads(completed.stdout)
    except (json.JSONDecodeError, TypeError) as exc:
        raise InfraError('CAPTURE_RECEIPT_INVALID') from exc
    return validate_receipt(value, capture_token=capture_token)


def _json_digest(path: Path) -> str:
    return sha(path)


def _existing_terminal(path: Path, requested: dict[str, Any]) -> dict[str, Any] | None:
    manifest_path = path / 'manifest.json'
    index_path = path / 'index.json'
    if not manifest_path.exists() and not index_path.exists():
        return None
    need(manifest_path.is_file() and index_path.is_file(), 'PACKAGE_PARTIAL_TERMINAL_STATE')
    manifest = read_json(manifest_path)
    need(manifest == requested, 'PACKAGE_FINALIZATION_CONFLICT')
    need(read_json(path / 'result.json') == {
        'schema': 1,
        'classification': requested['classification'],
        'test_outcome': requested['test_outcome'],
        'overall': requested['overall'],
        'code': requested['code'],
        'evidence_status': requested['evidence_status'],
    }, 'PACKAGE_RESULT_INVALID')
    need(read_json(path / 'retention.json') == {
        'schema': 1, 'state': 'SEALED', 'automatic_purge': False,
    }, 'PACKAGE_RETENTION_INVALID')
    index = read_json(index_path)
    need(
        set(index) == {'schema', 'files', 'raw_evidence'} and index['schema'] == 1
        and index['files'] == {
            name: _json_digest(path / name)
            for name in ('authority.json', 'result.json', 'retention.json', 'manifest.json')
        }
        and index['raw_evidence'] == requested['evidence'],
        'PACKAGE_INDEX_INVALID',
    )
    need(stat.S_IMODE(path.stat().st_mode) == 0o500, 'PACKAGE_PERMISSION_INVALID')
    for name in ('authority.json', 'result.json', 'retention.json', 'manifest.json', 'index.json'):
        need(stat.S_IMODE((path / name).stat().st_mode) == 0o400, 'PACKAGE_PERMISSION_INVALID')
    return {'manifest': manifest, 'index': index}


def finalize_terminal_package(
    capsule: Path | str,
    *,
    classification: str,
    test_outcome: str,
    capture_token: str,
    receipt: dict[str, Any] | None = None,
    evidence_error: str | None = None,
    expected_sha256: str | None = None,
    expected_bytes: int | None = None,
) -> dict[str, Any]:
    """Seal PASS, TEST_FAILED, or evidence-failed output without opening raw HCI."""

    path = no_links(capsule)
    need(path.is_dir() and PACKAGE_ID.fullmatch(path.name), 'PACKAGE_INVALID')
    need(test_outcome in ('PASS', 'FAIL'), 'TEST_OUTCOME_INVALID')
    authority = read_json(path / 'authority.json')
    need(
        set(authority) == {'schema', 'package_id', 'classification', 'capture_token', 'state'}
        and authority.get('schema') == 1
        and authority.get('package_id') == path.name
        and authority.get('state') == 'CAPTURE_AUTHORIZED'
        and authority.get('capture_token') == capture_token
        and authority.get('classification') == classification,
        'PACKAGE_AUTHORITY_INVALID',
    )
    need((receipt is None) != (evidence_error is None), 'EVIDENCE_RESULT_INVALID')
    if receipt is not None:
        evidence = validate_receipt(
            receipt,
            capture_token=capture_token,
            expected_sha256=expected_sha256,
            expected_bytes=expected_bytes,
        )
        code = 'SUCCESS' if test_outcome == 'PASS' else 'TEST_FAILED'
        overall = test_outcome
        evidence_status = 'FINALIZED'
    else:
        need(evidence_error in FINALIZATION_ERRORS, 'EVIDENCE_ERROR_INVALID')
        evidence = {'status': 'FINALIZATION_FAILED', 'code': evidence_error}
        code = 'EVIDENCE_FINALIZATION_FAILED'
        overall = 'FAIL'
        evidence_status = 'FINALIZATION_FAILED'
    terminal = {
        'schema': 1,
        'package_id': path.name,
        'classification': classification,
        'terminal': True,
        'test_outcome': test_outcome,
        'overall': overall,
        'code': code,
        'evidence_status': evidence_status,
        'evidence': evidence,
    }
    existing = _existing_terminal(path, terminal)
    if existing is not None:
        return existing
    atomic_json(path / 'result.json', {
        'schema': 1, 'classification': classification, 'test_outcome': test_outcome,
        'overall': overall, 'code': code, 'evidence_status': evidence_status,
    })
    atomic_json(path / 'retention.json', {
        'schema': 1, 'state': 'SEALED', 'automatic_purge': False,
    })
    atomic_json(path / 'manifest.json', terminal)
    index = {
        'schema': 1,
        'files': {
            name: _json_digest(path / name)
            for name in ('authority.json', 'result.json', 'retention.json', 'manifest.json')
        },
        'raw_evidence': evidence,
    }
    atomic_json(path / 'index.json', index)
    for name in ('authority.json', 'result.json', 'retention.json', 'manifest.json', 'index.json'):
        os.chmod(path / name, 0o400)
    fsync_dir(path)
    os.chmod(path, 0o500)
    fsync_dir(path.parent)
    return {'manifest': terminal, 'index': index}


def tree_digest(path: Path | str) -> str:
    """Digest only public package metadata; raw authority comes from its receipt."""

    root = no_links(path)
    values = {name: sha(root / name) for name in (
        'authority.json', 'result.json', 'retention.json', 'manifest.json', 'index.json'
    )}
    return hashlib.sha256(
        json.dumps(values, sort_keys=True, separators=(',', ':')).encode()
    ).hexdigest()
