import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import common
import evidence_pipeline as pipeline
import privileged_evidence as helper


TOKEN = '1' * 32


def termination(**changes):
    value = {
        'pid': 123,
        'exit_code': 0,
        'requested_signal': 'SIGINT',
        'forced_signal': None,
        'termination_confirmed': True,
        'early_exit': False,
    }
    value.update(changes)
    return value


def fixture_receipt(payload=b'private hci fixture', **changes):
    value = {
        'schema': 1,
        'capture_token': TOKEN,
        'relative_id': helper.RELATIVE_ID,
        'sha256': hashlib.sha256(payload).hexdigest(),
        'bytes': len(payload),
        'mode': '0600',
        'owner_uid': 0,
        'owner_gid': 0,
        'stable_stat': True,
        'writer': termination(),
    }
    value.update(changes)
    return value


class PrivilegedFinalizationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.raw = self.root / 'raw-hci'

    def tearDown(self):
        self.temporary.cleanup()

    def test_finalize_stopped_private_capture(self):
        payload = b'opaque capture bytes'
        self.raw.write_bytes(payload)
        receipt = helper.finalize_stopped_capture(
            self.raw, capture_token=TOKEN, termination=termination(),
        )
        self.assertEqual(receipt['sha256'], hashlib.sha256(payload).hexdigest())
        self.assertEqual(receipt['bytes'], len(payload))
        self.assertEqual(self.raw.stat().st_mode & 0o777, 0o600)

    def test_verify_finalized_is_read_only_and_reproducible(self):
        campaign = self.root / 'campaign'
        capsule = campaign / '20260918T010203Z-123456abcdef'
        privileged = capsule / 'privileged'
        privileged.mkdir(parents=True)
        (capsule / 'authority.json').write_text(json.dumps({
            'schema': 1,
            'package_id': capsule.name,
            'classification': pipeline.CLASSIFICATION,
            'capture_token': TOKEN,
            'state': 'CAPTURE_AUTHORIZED',
        }))
        payload = b'already finalized'
        raw = privileged / 'raw-hci'
        raw.write_bytes(payload)
        raw.chmod(0o600)
        receipt = fixture_receipt(
            payload, owner_uid=os.geteuid(), owner_gid=os.getegid(),
        )
        (privileged / helper.RECEIPT_NAME).write_text(json.dumps(receipt))
        first = helper.verify_finalized(
            capsule, capture_token=TOKEN, private_base=self.root,
            require_root=False,
        )
        second = helper.verify_finalized(
            capsule, capture_token=TOKEN, private_base=self.root,
            require_root=False,
        )
        self.assertEqual(first, second)
        self.assertEqual(raw.read_bytes(), payload)

    def test_missing_capture(self):
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_MISSING'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN, termination=termination(),
            )

    def test_empty_capture_is_invalid(self):
        self.raw.touch()
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_EMPTY'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN, termination=termination(),
            )

    def test_running_writer_is_rejected_before_file_access(self):
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_WRITER_STILL_RUNNING'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN,
                termination=termination(exit_code=-1, termination_confirmed=False),
            )

    def test_privileged_hash_failure_is_explicit(self):
        self.raw.write_bytes(b'x')
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_HASH_FAILED'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN, termination=termination(),
                hash_fd=lambda _: (_ for _ in ()).throw(OSError()),
            )

    def test_privileged_stat_failure_is_explicit(self):
        self.raw.write_bytes(b'x')
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_STAT_FAILED'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN, termination=termination(),
                open_file=lambda *_: (_ for _ in ()).throw(OSError()),
            )

    def test_change_during_hash_is_rejected(self):
        self.raw.write_bytes(b'first')

        def changing_hash(fd):
            before = os.fstat(fd)
            with self.raw.open('ab') as stream:
                stream.write(b'changed')
            digest = hashlib.sha256(os.read(fd, 1024)).hexdigest()
            after = os.fstat(fd)
            common.need(helper._stat_identity(before) == helper._stat_identity(after),
                        'CAPTURE_CHANGED_DURING_FINALIZATION')
            return digest, after

        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_CHANGED_DURING_FINALIZATION'):
            helper.finalize_stopped_capture(
                self.raw, capture_token=TOKEN, termination=termination(), hash_fd=changing_hash,
            )

    def test_inconsistent_helper_digest_is_rejected_for_controlled_fixture(self):
        payload = b'controlled fixture'
        wrong = fixture_receipt(payload, sha256='0' * 64)
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_DIGEST_MISMATCH'):
            helper.validate_receipt(
                wrong, capture_token=TOKEN,
                expected_sha256=hashlib.sha256(payload).hexdigest(),
                expected_bytes=len(payload),
            )

    def test_non_root_receipt_is_rejected_by_unprivileged_consumer(self):
        with self.assertRaisesRegex(common.InfraError, 'CAPTURE_OWNER_INVALID'):
            helper.validate_receipt(
                fixture_receipt(owner_uid=1000), capture_token=TOKEN,
            )


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.path, self.token = pipeline.create_package(self.base, pipeline.CLASSIFICATION)
        self.receipt = fixture_receipt(capture_token=self.token)

    def tearDown(self):
        for path in (self.path, self.path / 'privileged'):
            try:
                path.chmod(0o700)
            except FileNotFoundError:
                pass
        self.temporary.cleanup()

    def finalize(self, outcome='PASS', **values):
        return pipeline.finalize_terminal_package(
            self.path, classification=pipeline.CLASSIFICATION,
            test_outcome=outcome, capture_token=self.token, **values,
        )

    def test_root_owned_0600_opaque_raw_packages_without_runner_metadata_access(self):
        privileged = self.path / 'privileged'
        privileged.mkdir(mode=0o700)
        raw = privileged / 'raw-hci'
        raw.write_bytes(b'private hci fixture')
        raw.chmod(0o600)
        privileged.chmod(0)
        real_chmod = os.chmod

        def deny_raw(target, mode):
            if Path(target) == raw:
                raise PermissionError('ordinary runner cannot chmod raw')
            return real_chmod(target, mode)

        with mock.patch.object(pipeline.os, 'chmod', side_effect=deny_raw):
            result = self.finalize(receipt=self.receipt)
        self.assertEqual(result['manifest']['code'], 'SUCCESS')
        self.assertEqual(result['manifest']['evidence']['owner_uid'], 0)

    def test_manifest_finalizes_after_test_pass(self):
        result = self.finalize(receipt=self.receipt)
        self.assertEqual(result['manifest']['overall'], 'PASS')
        self.assertEqual(result['manifest']['code'], 'SUCCESS')
        self.assertEqual(common.read_json(self.path / 'retention.json')['state'], 'SEALED')

    def test_manifest_finalizes_after_test_fail(self):
        result = self.finalize(outcome='FAIL', receipt=self.receipt)
        self.assertEqual(result['manifest']['overall'], 'FAIL')
        self.assertEqual(result['manifest']['code'], 'TEST_FAILED')
        self.assertEqual(result['manifest']['evidence_status'], 'FINALIZED')

    def test_evidence_failure_after_test_fail_is_distinct_and_terminal(self):
        result = self.finalize(outcome='FAIL', evidence_error='CAPTURE_HASH_FAILED')
        self.assertEqual(result['manifest']['code'], 'EVIDENCE_FINALIZATION_FAILED')
        self.assertEqual(result['manifest']['test_outcome'], 'FAIL')
        self.assertTrue(result['manifest']['terminal'])
        self.assertTrue((self.path / 'index.json').is_file())

    def test_finalization_is_idempotent(self):
        first = self.finalize(receipt=self.receipt)
        second = self.finalize(receipt=self.receipt)
        self.assertEqual(first, second)

    def test_conflicting_repeat_is_rejected(self):
        self.finalize(receipt=self.receipt)
        with self.assertRaisesRegex(common.InfraError, 'PACKAGE_FINALIZATION_CONFLICT'):
            self.finalize(outcome='FAIL', receipt=self.receipt)


class SourceBoundaryTests(unittest.TestCase):
    def test_unprivileged_pipeline_has_no_raw_metadata_mutator(self):
        source = Path(pipeline.__file__).read_text()
        self.assertNotIn('chown(', source)
        self.assertNotIn("chmod(path / 'privileged", source)
        self.assertNotIn("open(path / 'privileged", source)


if __name__ == '__main__':
    unittest.main()
