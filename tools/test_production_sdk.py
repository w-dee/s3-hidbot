#!/usr/bin/env python3
"""Synthetic negative tests for the real SDK verifier and source contract."""
import json
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import patch

import prepare_production_sdk as sdk


class VerifierTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sdk = self.root / 'sdk'
        self.files = {
            self.sdk / sdk.SOURCE: b'patched-source',
            self.sdk / sdk.ARCHIVE: b'controller-fixture',
            self.root / 'tools/sdk-patches/nimble-dle-last.patch': b'patch-fixture',
        }
        for path, data in self.files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.lock = dict(idf_revision='1' * 40, nimble_revision='2' * 40,
                         controller_revision='3' * 40,
                         controller_archive_sha256=sdk.digest(self.sdk / sdk.ARCHIVE),
                         patch_sha256=sdk.digest(self.root / 'tools/sdk-patches/nimble-dle-last.patch'),
                         pre_source_sha256='4' * 64,
                         post_source_sha256=sdk.digest(self.sdk / sdk.SOURCE))
        (self.root / 'firmware').mkdir()
        (self.root / 'firmware/production-sdk.lock.json').write_text(json.dumps(self.lock))
        self.bad_base = False
        self.extra = False

    def fake_git(self, root, *args):
        if args == ('rev-parse', 'HEAD'):
            if self.bad_base:
                return '0' * 40
            return {self.sdk: '1' * 40, self.sdk / sdk.NIMBLE: '2' * 40,
                    self.sdk / sdk.CONTROLLER: '3' * 40}[root]
        if args[:2] == ('diff', 'HEAD'):
            if self.extra:
                return 'unapproved.c'
            return 'nimble/host/src/ble_gap.c' if root == self.sdk / sdk.NIMBLE else ''
        return ''

    def verify(self, **kwargs):
        repos = [(self.sdk, '1' * 40), (self.sdk / sdk.NIMBLE, '2' * 40),
                 (self.sdk / sdk.CONTROLLER, '3' * 40)]
        with patch.object(sdk, 'git', side_effect=self.fake_git), \
             patch.object(sdk, 'repositories', return_value=repos):
            return sdk.verify(self.sdk, self.root, **kwargs)

    def test_expected(self):
        self.assertEqual(self.verify(), self.lock)

    def test_wrong_base(self):
        self.bad_base = True
        with self.assertRaises(ValueError): self.verify()

    def test_extra_modification(self):
        self.extra = True
        with self.assertRaises(ValueError): self.verify()

    def test_hash_mismatches(self):
        for path, data in self.files.items():
            with self.subTest(file=path.name):
                path.write_bytes(b'wrong')
                with self.assertRaises(ValueError): self.verify()
                path.write_bytes(data)

    def test_already_patched_preparation_input(self):
        with self.assertRaises(ValueError): self.verify(patched=False)

    def test_unpatched_or_wrong_post_source(self):
        (self.sdk / sdk.SOURCE).write_bytes(b'stock-source')
        with self.assertRaises(ValueError): self.verify()

    def test_fixed_source_contract(self):
        root = Path(__file__).resolve().parents[1]
        lock, source_patch = sdk.contract(root)
        text = source_patch.read_text()
        removed = [line[1:] for line in text.splitlines() if line.startswith('-') and not line.startswith('---')]
        added = [line for line in text.splitlines() if line.startswith('+') and not line.startswith('+++')]
        self.assertEqual(len(removed), 4)
        self.assertFalse(added)
        self.assertIn('ble_gap_event_connect_call', text)
        transport = (root / 'firmware/components/ble_transport/ble_transport.cpp').read_text()
        self.assertIn('#include "host/ble_esp_gap.h"', transport)
        self.assertEqual(transport.count('ble_hs_hci_util_set_data_len('), 1)
        self.assertIn('ble_hs_hci_util_set_data_len(connection_handle, 251, 0x4290)', transport)
        self.assertNotIn('ble_gap_set_data_len(', transport)

    def test_manifest_v1_preserved_and_v2_strict(self):
        from test_firmware_artifact import _build_synthetic_bundle, _rewrite_manifest
        from firmware_artifact import verify_bundle_directory, ArtifactError
        with tempfile.TemporaryDirectory() as temporary:
            bundle = _build_synthetic_bundle(Path(temporary))
            verify_bundle_directory(bundle)  # historical schema remains valid
            production, _ = sdk.contract()
            def upgrade(manifest):
                manifest['artifact_manifest_version'] = 2
                manifest['firmware']['idf_version'] = 'v5.5.4-dirty'
                manifest['provenance']['production_sdk'] = production
            _rewrite_manifest(bundle, upgrade)
            manifest = verify_bundle_directory(bundle)
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host/src'))
            self.addCleanup(sys.path.pop, 0)
            from hidbot.firmware_verification import artifact_identity_from_verified_manifest, compare_firmware_identity
            from hidbot.protocol import SystemInfo, FirmwareIdentity
            artifact = artifact_identity_from_verified_manifest(manifest)
            runtime = SystemInfo(project=artifact.project, target=artifact.target,
                protocol_version=artifact.protocol_version, idf_version='v5.5.4-dirty',
                firmware=FirmwareIdentity(version=artifact.version,
                    source_revision=artifact.source_revision,
                    app_elf_sha256=artifact.app_elf_sha256, build_profile=artifact.build_profile))
            self.assertTrue(compare_firmware_identity(artifact, ('firmware.identity-v1',), runtime).match)
            def corrupt(manifest):
                manifest['provenance']['production_sdk']['patch_sha256'] = 'bad'
            _rewrite_manifest(bundle, corrupt)
            with self.assertRaises(ArtifactError): verify_bundle_directory(bundle)


if __name__ == '__main__':
    unittest.main()
