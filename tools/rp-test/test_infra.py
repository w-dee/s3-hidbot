"""Host-only regression tests. All appliance roots and protocol peers are fake."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import bootstrap
import cache
import common
import doctor
import preflight
import run_capsule as capsule
import startup_probe as startup
import serial_readiness as serial_startup


class Fixture(unittest.TestCase):
    def setUp(self):
        # Synthetic fixtures must not depend on the workstation's /tmp capacity
        # (Pi OS commonly places /tmp on a small tmpfs). Disk refusal is tested
        # separately; production doctor still measures the persistent /srv root.
        disk = patch.object(shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(8 * 1024**3, 0, 8 * 1024**3))
        disk.start()
        self.addCleanup(disk.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.root = self.base / 'appliance'
        for sub in ('', *common.LAYOUT):
            common.private_dir(self.root / sub)
        self.source = common.private_dir(self.base / 'source')
        (self.source / 'runner.py').write_text('pass\n')
        self.digest = self.manifest(self.source)
        self.digests = dict.fromkeys(('functional', 'firmware', 'parser'), self.digest)

    def tearDown(self):
        for path in self.base.rglob('*'):
            if path.is_dir() and not path.is_symlink():
                path.chmod(0o700)
        self.tmp.cleanup()

    def manifest(self, source):
        files = sorted(p for p in source.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
        (source / 'SHA256SUMS').write_text(''.join(common.sha(p) + '  ' + str(p.relative_to(source)) + '\n' for p in files))
        return common.sha(source / 'SHA256SUMS')

    def create(self, profile='synthetic'):
        return capsule.create(self.root, profile, self.source, self.digests)['run_id']

    def entry(self):
        cache.import_tree(self.root, 'tooling', self.digest, self.source)
        return cache.destination(self.root, 'tooling', self.digest)


class BootstrapTests(Fixture):
    def test_host_facts(self):
        good = {'os': 'debian', 'os_version': '13', 'arch': 'aarch64', 'pi4': True}
        self.assertTrue(bootstrap.compatible(good))
        for key, value in [('os', 'ubuntu'), ('arch', 'x86_64'), ('pi4', False), ('os_version', '11')]:
            self.assertFalse(bootstrap.compatible(dict(good, **{key: value})))

    def test_missing_marker(self):
        with self.assertRaisesRegex(common.InfraError, 'HOST_MARKER_MISSING'):
            common.verify_marker(self.base / 'missing')

    def test_marker_symlink(self):
        marker = self.base / 'marker'
        marker.symlink_to(self.source / 'runner.py')
        with self.assertRaisesRegex(common.InfraError, 'SYMLINK'):
            common.verify_marker(marker)

    def test_marker_wrong_content(self):
        marker = self.base / 'marker'
        marker.write_text('{}')
        marker.chmod(0o644)
        # Owner validation is tested with a synthetic root-owned stat only.
        real = Path.stat
        def stat(path, **kwargs):
            s = real(path, **kwargs)
            if path == marker:
                values = list(s)
                values[4] = 0
                return os.stat_result(values)
            return s
        with patch.object(Path, 'stat', stat), self.assertRaisesRegex(common.InfraError, 'MARKER_MISMATCH'):
            common.verify_marker(marker)

    def test_runtime_lock(self):
        value = {'esptool': '4.12.0', 'dependencies': bootstrap.desired_dependencies()}
        self.assertTrue(bootstrap.runtime_matches(value))
        self.assertFalse(bootstrap.runtime_matches(dict(value, esptool='5.0')))

    def test_toolchain_stamp(self):
        value = {'runtime': {'esptool': '4.12.0', 'python': '3.13.5', 'dependencies': [list(p) for p in bootstrap.desired_dependencies()]}}
        common.atomic_json(self.root / 'state/toolchain.json', value)
        self.assertEqual(doctor.toolchain_check(self.root, current=lambda r: value)['esptool'], '4.12.0')
        with self.assertRaisesRegex(common.InfraError, 'TOOLCHAIN_CACHE_STALE'):
            doctor.toolchain_check(self.root, current=lambda r: dict(value, changed=True))

    def test_idempotent_install_decision(self):
        value = {'esptool': '4.12.0', 'dependencies': bootstrap.desired_dependencies()}
        self.assertFalse(not bootstrap.runtime_matches(value))
        self.assertTrue(not bootstrap.runtime_matches(dict(value, dependencies=[])))

    def test_bootstrap_has_guard_before_install(self):
        source = Path(bootstrap.__file__).read_text()
        self.assertLess(source.index('need(compatible(host_facts())'), source.index("['apt-get', 'install'"))
        self.assertIn('if missing:', source)
        self.assertIn('if install:', source)
        for forbidden in ('dist-upgrade', 'systemctl', 'rfkill', 'set-property'):
            self.assertNotIn(forbidden, source)

    def test_bootstrap_two_passes_synthetic(self):
        marker = self.base / 'marker'
        marker.write_text('{}')
        calls = []
        installed = [False]
        value = {'python': '3.13.5', 'esptool': '4.12.0', 'dependencies': [list(p) for p in bootstrap.desired_dependencies()]}
        stamp = {'schema': 1, 'runtime': value, 'packages': dict.fromkeys(bootstrap.PACKAGES, '1')}
        def run(command, category, env=None):
            calls.append(category)
            if category == 'VENV_CREATE_FAILED':
                p = common.private_dir(self.root / 'toolchains/qualification/bin') / 'python'
                p.touch()
            if category == 'PIP_INSTALL_FAILED':
                installed[0] = True
        from types import SimpleNamespace
        with patch.object(bootstrap, 'ROOT', self.root), patch.object(bootstrap, 'MARKER', marker), \
             patch.object(bootstrap, 'verify_marker'), patch.object(bootstrap, 'compatible', return_value=True), \
             patch.object(bootstrap, 'host_facts', return_value={}), patch.object(os, 'geteuid', return_value=0), \
             patch.dict(os.environ, {'SUDO_UID': '1000'}), patch.object(bootstrap.pwd, 'getpwuid', return_value=SimpleNamespace(pw_gid=1000)), \
             patch.object(os, 'chown'), patch.object(bootstrap, 'package_versions', return_value=stamp['packages']), \
             patch.object(bootstrap, 'runtime', side_effect=lambda: value if installed[0] else {}), \
             patch.object(bootstrap, 'stamp_value', return_value=stamp), patch.object(bootstrap, 'run_quiet', side_effect=run), \
             patch.object(sys, 'argv', ['bootstrap']), contextlib.redirect_stdout(io.StringIO()):
            bootstrap.main()
            first = list(calls)
            calls.clear()
            bootstrap.main()
        self.assertIn('PIP_INSTALL_FAILED', first)
        self.assertEqual(calls, ['DEPENDENCIES_INCONSISTENT'])

    def test_marker_guard_prevents_host_mutation(self):
        with patch.object(bootstrap, 'compatible', return_value=False), patch.object(bootstrap, 'host_facts', return_value={}), \
             patch.object(os, 'geteuid', return_value=0), patch.object(sys, 'argv', ['bootstrap', '--initialize-host']), \
             patch.object(bootstrap, 'private_dir') as mkdir, self.assertRaises(common.InfraError):
            bootstrap.main()
        mkdir.assert_not_called()


class CacheTests(Fixture):
    def test_import_count_size(self):
        result = cache.import_tree(self.root, 'tooling', self.digest, self.source, 2, sum(p.stat().st_size for p in self.source.iterdir()))
        self.assertEqual(result['files'], 2)
        self.assertEqual(result['classification'], 'CACHE_HIT')

    def test_warm_no_content_hash(self):
        self.entry()
        with patch.object(cache, 'verify_full', side_effect=AssertionError('full')):
            self.assertEqual(cache.verify(self.root, 'tooling', self.digest)['classification'], 'CACHE_HIT')

    def test_hit_does_not_transfer(self):
        self.entry()
        with patch.object(shutil, 'copytree', side_effect=AssertionError('copy')):
            cache.import_tree(self.root, 'tooling', self.digest, self.source)

    def test_missing_marker_full(self):
        entry = self.entry()
        (entry / 'VERIFIED.json').unlink()
        self.assertEqual(cache.verify(self.root, 'tooling', self.digest)['classification'], 'CACHE_FULL_VERIFIED')

    def test_metadata_change_full(self):
        entry = self.entry()
        p = entry / 'payload/runner.py'
        os.utime(p, None)
        self.assertEqual(cache.verify(self.root, 'tooling', self.digest)['classification'], 'CACHE_FULL_VERIFIED')

    def test_corruption_rejected(self):
        entry = self.entry()
        p = entry / 'payload/runner.py'
        p.chmod(0o600)
        p.write_text('nope\n')
        p.chmod(0o400)
        with self.assertRaisesRegex(common.InfraError, 'CONTENT_MISMATCH'):
            cache.verify(self.root, 'tooling', self.digest)

    def test_permission_rejected(self):
        entry = self.entry()
        (entry / 'payload/runner.py').chmod(0o644)
        with self.assertRaisesRegex(common.InfraError, 'PERMISSION_INVALID'):
            cache.verify(self.root, 'tooling', self.digest)

    def test_partial_import(self):
        (self.source / 'runner.py').unlink()
        with self.assertRaises((common.InfraError, OSError)):
            cache.import_tree(self.root, 'tooling', self.digest, self.source)
        self.assertFalse(cache.destination(self.root, 'tooling', self.digest).exists())

    def test_extra_file(self):
        (self.source / 'extra').write_text('extra')
        with self.assertRaisesRegex(common.InfraError, 'FILE_SET'):
            cache.import_tree(self.root, 'tooling', self.digest, self.source)

    def test_wrong_count(self):
        with self.assertRaisesRegex(common.InfraError, 'FILE_COUNT'):
            cache.import_tree(self.root, 'tooling', self.digest, self.source, count=4)

    def test_wrong_bytes(self):
        with self.assertRaisesRegex(common.InfraError, 'BYTE_COUNT'):
            cache.import_tree(self.root, 'tooling', self.digest, self.source, size=1)

    def test_wrong_digest(self):
        with self.assertRaisesRegex(common.InfraError, 'DIGEST_MISMATCH'):
            cache.import_tree(self.root, 'tooling', '0' * 64, self.source)

    def test_source_symlink(self):
        (self.source / 'escape').symlink_to(self.base)
        with self.assertRaisesRegex(common.InfraError, 'SYMLINK'):
            cache.import_tree(self.root, 'tooling', self.digest, self.source)

    def test_manifest_traversal(self):
        (self.source / 'SHA256SUMS').write_text('0' * 64 + '  ../outside\n')
        digest = common.sha(self.source / 'SHA256SUMS')
        with self.assertRaisesRegex(common.InfraError, 'MANIFEST_INVALID'):
            cache.import_tree(self.root, 'tooling', digest, self.source)

    def test_reference_permissions(self):
        cache.import_tree(self.root, 'references', self.digest, self.source)
        cache.modes(cache.destination(self.root, 'references', self.digest) / 'payload', 'references')


class DoctorTests(Fixture):
    def fake_doctor(self, root):
        return {'ok': True, 'physical_run_locked': False, 'timing_ms': {'host_doctor': 1, 'bootstrap_stamp': 2}}

    def test_json_schema(self):
        result = doctor.doctor(self.root, host=lambda r: {'physical_run_locked': False}, toolchain=lambda r: {'esptool': '4.12.0'})
        self.assertEqual(set(result), {'schema', 'ok', 'classification', 'hardware_operations', 'physical_run_locked', 'timing_ms', 'toolchain'})
        self.assertTrue(json.loads(json.dumps(result))['ok'])

    def test_host_only_profile(self):
        result = preflight.preflight('host-only', self.root, self.fake_doctor, lambda *a: self.fail('cache queried'))
        self.assertTrue(result['ok'])
        self.assertFalse(result['mission_authorized'])
        self.assertEqual(result['hardware_operations'], 0)

    def test_missing_assets_cold(self):
        digest = 'a' * 64
        definitions = {'schema': 1,
                       'assets': {'artifact': ['artifacts', digest]},
                       'profiles': {'fixture': {
                           'assets': ['artifact'], 'python_adapter': None,
                           'startup_attempts': 1,
                           'opportunistic_probe_seconds': 2.0,
                           'reset_readiness_seconds': 12.0,
                           'start_state': 'unknown', 'hardware_checks': [],
                       }}}
        config = self.base / 'profiles.json'
        config.write_text(json.dumps(definitions))
        with patch.object(preflight, 'PROFILES', config):
            result = preflight.preflight('fixture', self.root, self.fake_doctor)
            self.assertEqual(result['classification'], 'COLD_PREPARATION_REQUIRED')

    def test_all_profile_names(self):
        profiles = common.read_json(preflight.PROFILES)['profiles']
        self.assertEqual(set(profiles), {'host-only', 'reference'})
        self.assertEqual(profiles['reference']['startup_attempts'], 1)
        self.assertEqual(profiles['reference']['opportunistic_probe_seconds'], 2.0)
        self.assertEqual(profiles['reference']['reset_readiness_seconds'], 12.0)
        self.assertIsNone(profiles['reference']['python_adapter'])

    def test_cached_python_adapter_source_contract(self):
        digest = 'a' * 64
        definitions = {'assets': {'qualification': ['artifacts', digest]}}
        spec = {'assets': ['qualification'], 'python_adapter': ['qualification', 'adapters']}
        root = self.root / 'cache/artifacts' / digest / 'payload/adapters'
        for name in preflight.ADAPTER_MODULES:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('# fixture\n')
        self.assertEqual(preflight.validate_python_adapter(self.root, definitions, spec),
                         'CACHE_SOURCE_READY')

    def test_cached_python_adapter_source_missing_fails_closed(self):
        digest = 'b' * 64
        definitions = {'assets': {'qualification': ['artifacts', digest]}}
        spec = {'assets': ['qualification'], 'python_adapter': ['qualification', 'adapters']}
        with self.assertRaisesRegex(common.InfraError, 'PYTHON_ADAPTER_SOURCE_MISSING'):
            preflight.validate_python_adapter(self.root, definitions, spec)

    def test_timings(self):
        result = preflight.preflight('host-only', self.root, self.fake_doctor)
        self.assertEqual(set(result['timing_ms']), {'bootstrap_stamp', 'host_doctor', 'artifact_cache', 'tooling_cache', 'private_reference', 'mission_static_checks', 'hardware_checks', 'total'})
        self.assertTrue(all(t >= 0 for t in result['timing_ms'].values()))

    def test_no_network_install_in_warm(self):
        for module in (doctor, preflight, cache):
            source = Path(module.__file__).read_text()
            for forbidden in ("'apt-get'", "'pip'", "'fetch'", "'clone'", 'socket.', 'requests.', 'serial.Serial', 'set-property'):
                self.assertNotIn(forbidden, source)

    def test_disk_guard(self):
        facts = lambda: {'os': 'debian', 'os_version': '13', 'arch': 'aarch64', 'pi4': True}
        with patch.object(shutil, 'which', return_value='/bin/tool'), self.assertRaisesRegex(common.InfraError, 'FORENSIC_STORAGE_LOW'):
            doctor.host_checks(self.root, lambda: None, facts, lambda _: shutil._ntuple_diskusage(10, 10, 0))

    def test_latency_warning(self):
        self.assertFalse(preflight.latency_warning(120, {'a': 1})['preoperation_slow'])
        self.assertEqual(preflight.latency_warning(121, {'a': 1})['phase_timing_ms'], {'a': 1})


class Clock:
    def __init__(self):
        self.now = 0
    def __call__(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds

class Peer:
    def __init__(self, error=None):
        self.error = error
        self.closed = False
    def sync(self, remaining):
        pass
    def hello(self, remaining):
        if self.error:
            raise self.error
        return 'correlated'
    def info(self, remaining):
        return 'exact'
    def close(self):
        self.closed = True

class StartupTests(unittest.TestCase):
    def ready(self, factory, matches=lambda h, i: True):
        clock = Clock()
        return startup.readiness(factory, matches, timeout=1, interval=.25, clock=clock, sleeper=clock.sleep)

    def test_first_timeout_then_ready(self):
        peers = [Peer(startup.RetryableRead('timeout')), Peer()]
        queue = iter(peers)
        result = self.ready(lambda _: next(queue))
        self.assertEqual(result['classification'], 'IDENTITY_VERIFIED')
        self.assertEqual(result['attempts'], 2)
        self.assertTrue(all(p.closed for p in peers))

    def test_serial_disappearance_reappearance(self):
        queue = iter([None, Peer(startup.RetryableRead('disappeared')), None, Peer()])
        self.assertEqual(self.ready(lambda _: next(queue))['classification'], 'IDENTITY_VERIFIED')

    def test_absent_serial(self):
        self.assertEqual(self.ready(lambda _: None)['classification'], 'SERIAL_NOT_READY')

    def test_deadline(self):
        result = self.ready(lambda _: Peer(startup.RetryableRead('timeout')))
        self.assertEqual(result['classification'], 'PROTOCOL_READINESS_TIMEOUT')
        self.assertEqual(result['attempts'], 4)

    def test_identity_mismatch_no_retry(self):
        result = self.ready(lambda _: Peer(), lambda h, i: False)
        self.assertEqual(result['classification'], 'IDENTITY_MISMATCH')
        self.assertEqual(result['attempts'], 1)

    def test_malformed(self):
        self.assertEqual(self.ready(lambda _: Peer(startup.MalformedRead('bad')))['classification'], 'PROTOCOL_MALFORMED')

    def test_malformed_then_resync(self):
        queue = iter([Peer(startup.MalformedRead('partial')), Peer()])
        self.assertEqual(self.ready(lambda _: next(queue))['classification'], 'IDENTITY_VERIFIED')

    def test_read_only_retry(self):
        clock, calls = Clock(), []
        def action(remaining):
            calls.append(remaining)
            if len(calls) == 1:
                raise startup.RetryableRead()
            return True
        self.assertTrue(startup.bounded_read('ble.bond.list', action, clock=clock, sleeper=clock.sleep))
        self.assertEqual(len(calls), 2)

    def test_side_effect_cannot_use_retry(self):
        for operation in startup.SIDE_EFFECTS:
            with self.assertRaises(common.InfraError):
                startup.bounded_read(operation, lambda _: self.fail('invoked'))

    def test_delete_timeout_consumes_budget(self):
        budget = startup.OperationBudget(startup_limit=2)
        def fail():
            raise TimeoutError()
        with self.assertRaises(TimeoutError):
            budget.invoke_once('bond.delete', fail)
        with self.assertRaises(common.InfraError):
            budget.invoke_once('ble.bond.remove', lambda: self.fail('repeat'))
        with self.assertRaises(common.InfraError):
            budget.startup(lambda: self.fail('reset'))

    def test_two_startups(self):
        budget = startup.OperationBudget(startup_limit=2)
        calls = []
        for _ in range(2):
            budget.startup(lambda: calls.append(True))
        with self.assertRaises(common.InfraError):
            budget.startup(lambda: calls.append(True))
        self.assertEqual(len(calls), 2)

    def test_commit_failure_prevents_side_effect(self):
        def fail(*args):
            raise OSError()
        budget = startup.OperationBudget(commit=fail)
        with self.assertRaises(OSError):
            budget.invoke_once('Pair', lambda: self.fail('invoked'))

    def test_full_startup_sequence(self):
        clock, calls = Clock(), []
        budget = startup.OperationBudget(startup_limit=2)
        result = startup.startup_sequence(budget, lambda: calls.append(True), lambda _: None, lambda h, i: True,
            timeout=1, interval=.25, clock=clock, sleeper=clock.sleep)
        self.assertEqual(len(calls), 2)
        self.assertEqual(result['states'][0], 'RESET_REQUESTED')

    def test_firmware_retention_policy(self):
        value = dict(exact_identity=True, understood_state=True, pending_recovery=False, contained=True)
        self.assertTrue(startup.qualification_may_remain(**value))
        for key in ('explicit_restore', 'milestone_ended', 'recovery_requires_restore'):
            self.assertFalse(startup.qualification_may_remain(**value, **{key: True}))


class CapsuleTests(Fixture):
    def test_fixed_capsule_profile_policy(self):
        expected = ('bond-delete', 'combined-fresh-reconnect', 'recovery', 'synthetic', 'usb-sequence-v1')
        self.assertEqual(capsule.CAPSULE_PROFILES, expected)
        for profile in expected:
            path = capsule.run_path(self.root, self.create(profile))
            self.assertEqual(capsule.validate_capsule(path)['profile'], profile)

    def test_unknown_capsule_profile_fails_closed(self):
        with self.assertRaisesRegex(common.InfraError, 'PROFILE_INVALID'):
            self.create('unknown-fixture')
        path = capsule.run_path(self.root, self.create())
        manifest = common.read_json(path / 'manifest.json')
        manifest['profile'] = 'unknown-fixture'
        common.atomic_json(path / 'manifest.json', manifest)
        with self.assertRaisesRegex(common.InfraError, 'PROFILE_INVALID'):
            capsule.validate_capsule(path)

    def test_invalid_capsule_profile_token_fails_closed(self):
        with self.assertRaisesRegex(common.InfraError, 'PROFILE_INVALID'):
            self.create('invalid/profile')
        path = capsule.run_path(self.root, self.create())
        manifest = common.read_json(path / 'manifest.json')
        manifest['profile'] = 'invalid/profile'
        common.atomic_json(path / 'manifest.json', manifest)
        with self.assertRaisesRegex(common.InfraError, 'PROFILE_INVALID'):
            capsule.validate_capsule(path)

    def test_capsule_and_warm_preflight_profile_namespaces_are_separate(self):
        warm_profiles = common.read_json(preflight.PROFILES)['profiles']
        self.assertIn('synthetic', capsule.CAPSULE_PROFILES)
        self.assertNotIn('synthetic', warm_profiles)
        self.assertIn('reference', warm_profiles)
        self.assertNotIn('reference', capsule.CAPSULE_PROFILES)

    def test_capsule_low_space_refuses_creation(self):
        with patch.object(shutil, 'disk_usage', return_value=shutil._ntuple_diskusage(1, 1, 0)), self.assertRaisesRegex(common.InfraError, 'FORENSIC_STORAGE_LOW'):
            self.create()
        self.assertEqual(list((self.root / 'runs').iterdir()), [])

    def test_creation_exact_runner(self):
        rid = self.create()
        path = capsule.run_path(self.root, rid)
        capsule.validate_capsule(path)
        self.assertEqual(capsule.hash_tree(path / 'runner'), capsule.hash_tree(self.source))
        (self.source / 'runner.py').write_text('changed')
        self.assertNotEqual(common.sha(self.source / 'runner.py'), common.sha(path / 'runner/runner.py'))

    def test_permission_enforcement(self):
        path = capsule.run_path(self.root, self.create())
        for f in common.tree_files(path):
            expected = 0o400 if f.is_relative_to(path / 'runner') else 0o600
            self.assertEqual(f.stat().st_mode & 0o777, expected)
        for d in [path, *path.rglob('*')]:
            if d.is_dir():
                expected = 0o500 if d.is_relative_to(path / 'runner') else 0o700
                self.assertEqual(d.stat().st_mode & 0o777, expected)

    def test_cleanup_retains_unresolved(self):
        rid = self.create()
        path = capsule.run_path(self.root, rid)
        (path / 'evidence/raw-nvs').write_bytes(b'synthetic')
        (path / 'evidence/raw-nvs').chmod(0o600)
        (path / 'scratch/temporary').write_bytes(b'x')
        capsule.transition(self.root, rid, 'UNRESOLVED')
        capsule.cleanup(self.root, rid)
        self.assertTrue((path / 'evidence/raw-nvs').exists())
        self.assertTrue((path / 'runner/runner.py').exists())
        self.assertEqual(list((path / 'scratch').iterdir()), [])

    def test_resolved_retained_after_ack(self):
        rid = self.create()
        capsule.commit_result(self.root, rid, 'PASS', 'SUCCESS')
        capsule.acknowledge(self.root, rid, 'RETRIEVED')
        capsule.acknowledge(self.root, rid, 'ACKNOWLEDGED')
        capsule.transition(self.root, rid, 'RESOLVED')
        capsule.cleanup(self.root, rid)
        self.assertTrue(capsule.run_path(self.root, rid).exists())
        with self.assertRaisesRegex(common.InfraError, 'PURGE_REFUSED'):
            capsule.purge(self.root, rid)

    def test_active_purge_refused_even_override(self):
        rid = self.create()
        with self.assertRaises(common.InfraError):
            capsule.purge(self.root, rid, True)

    def test_unresolved_purge_override(self):
        rid = self.create()
        capsule.transition(self.root, rid, 'UNRESOLVED')
        with self.assertRaises(common.InfraError):
            capsule.purge(self.root, rid)
        self.assertTrue(capsule.purge(self.root, rid, True)['deleted'])

    def test_normal_purge(self):
        rid = self.create()
        capsule.transition(self.root, rid, 'RESOLVED')
        capsule.transition(self.root, rid, 'PURGE_ELIGIBLE')
        capsule.purge(self.root, rid)
        self.assertFalse((self.root / 'runs' / rid).exists())

    def test_bad_retention_transition(self):
        rid = self.create()
        with self.assertRaises(common.InfraError):
            capsule.transition(self.root, rid, 'PURGE_ELIGIBLE')

    def test_purge_traversal(self):
        for rid in ('..', '../outside', '/', '*', 'invalid'):
            with self.assertRaises(common.InfraError):
                capsule.purge(self.root, rid, True)

    def test_purge_root_symlink(self):
        rid = self.create()
        path = capsule.run_path(self.root, rid)
        capsule.unseal_runner_directories_for_purge(path / 'runner')
        shutil.rmtree(path)
        path.symlink_to(self.source)
        with self.assertRaises(common.InfraError):
            capsule.purge(self.root, rid, True)
        self.assertTrue((self.source / 'runner.py').exists())

    def test_purge_nested_symlink(self):
        rid = self.create()
        path = capsule.run_path(self.root, rid)
        capsule.transition(self.root, rid, 'RESOLVED')
        capsule.transition(self.root, rid, 'PURGE_ELIGIBLE')
        (path / 'evidence/escape').symlink_to(self.source)
        with self.assertRaises(common.InfraError):
            capsule.purge(self.root, rid)
        self.assertTrue((self.source / 'runner.py').exists())

    def test_atomic_failure_preserves_ledger(self):
        rid = self.create()
        path = capsule.run_path(self.root, rid) / 'ledger.json'
        before = path.read_bytes()
        with patch.object(os, 'replace', side_effect=OSError()), self.assertRaises(OSError):
            capsule.append_event(self.root, rid, 'CHECKPOINT', 'identity')
        self.assertEqual(path.read_bytes(), before)

    def test_ledger_schema(self):
        rid = self.create()
        with self.assertRaises(common.InfraError):
            capsule.append_event(self.root, rid, 'private message', 'private message')

    def test_exactly_once_result(self):
        rid = self.create()
        capsule.commit_result(self.root, rid, 'UNRESOLVED', 'INTERRUPTED')
        with self.assertRaises(common.InfraError):
            capsule.commit_result(self.root, rid, 'PASS', 'SUCCESS')

    def test_lock(self):
        with common.lock(self.root, 'physical'):
            with self.assertRaisesRegex(common.InfraError, 'PHYSICAL_RUN_LOCKED'):
                with common.lock(self.root, 'physical'):
                    pass
        with common.lock(self.root, 'physical'):
            pass

    def test_lock_symlink(self):
        (self.root / 'state/physical.lock').symlink_to(self.source / 'runner.py')
        with self.assertRaises(common.InfraError):
            with common.lock(self.root, 'physical'):
                pass

    def test_sanitized_inventory(self):
        rid = self.create()
        item = capsule.inventory(self.root)[0]
        self.assertEqual(item['run_id'], rid)
        self.assertEqual(set(item), {'run_id', 'state', 'created', 'total_bytes', 'RAW_HCI_RETAINED', 'RAW_UART_RETAINED', 'RAW_NVS_RETAINED', 'RAW_UART_BYTES', 'RAW_UART_HAS_DATA'})
        self.assertEqual(item['RAW_UART_BYTES'], 0)
        self.assertFalse(item['RAW_UART_HAS_DATA'])

    def test_export_equality(self):
        rid = self.create()
        capsule.transition(self.root, rid, 'UNRESOLVED')
        destination = self.base / 'export'
        self.assertTrue(capsule.export(self.root, rid, destination)['export_verified'])
        self.assertEqual(capsule.hash_tree(destination), capsule.hash_tree(capsule.run_path(self.root, rid)))

    def test_export_symlink_parent(self):
        rid = self.create()
        capsule.transition(self.root, rid, 'UNRESOLVED')
        (self.base / 'link').symlink_to(self.source)
        with self.assertRaises(common.InfraError):
            capsule.export(self.root, rid, self.base / 'link/out')

    def test_exception_privacy(self):
        secret = '/' + 'ho' + 'me/' + 'private-' + 'user/secret ' + ':'.join(['01'] * 6)
        try:
            raise RuntimeError(secret)
        except RuntimeError as exc:
            result = capsule.sanitized_exception(exc, 'startup', 'query', ('test_infra.py',))
        self.assertNotIn(secret, json.dumps(result))
        self.assertEqual(result['source'], 'test_infra.py')
        rid = self.create()
        capsule.commit_result(self.root, rid, 'FAIL', 'INFRA_ERROR', result)

    def test_budget_recovery_preserves_invocation(self):
        rid = self.create()
        capsule.append_event(self.root, rid, 'STARTUP_INVOKED', 'startup')
        capsule.append_event(self.root, rid, 'SIDE_EFFECT_INVOKED', 'ble.bond.remove')
        budget = capsule.resume_budget(self.root, rid, 2)
        with self.assertRaises(common.InfraError):
            budget.startup(lambda: self.fail('reset'))
        with self.assertRaises(common.InfraError):
            budget.invoke_once('bond.delete', lambda: self.fail('repeat'))

    def test_budget_unknown_outcome_neither_succeeds_nor_replays(self):
        rid = self.create()
        calls = []
        budget = capsule.resume_budget(self.root, rid, 2)
        with self.assertRaises(TimeoutError):
            budget.invoke_once('ble.enable', lambda: (calls.append('called'), (_ for _ in ()).throw(TimeoutError()))[1])
        self.assertEqual(calls, ['called'])
        self.assertFalse((capsule.run_path(self.root, rid) / 'result.json').exists())
        resumed = capsule.resume_budget(self.root, rid, 2)
        with self.assertRaises(common.InfraError):
            resumed.invoke_once('ble.enable', lambda: self.fail('replayed'))

    def test_startup_commit_precedes_action_and_resume_preserves_count(self):
        rid = self.create()
        observed = []
        budget = capsule.resume_budget(self.root, rid, 2)
        budget.startup(lambda: observed.append(common.read_json(
            capsule.run_path(self.root, rid) / 'ledger.json')['events'][-1]))
        self.assertEqual(observed, [{'event': 'STARTUP_INVOKED', 'operation': 'startup'}])
        self.assertEqual(capsule.resume_budget(self.root, rid, 2).startup_attempts, 1)

    def test_explicit_raw_ingest(self):
        rid = self.create()
        raw = self.base / 'synthetic-raw'
        raw.write_bytes(b'fixture only')
        self.assertEqual(capsule.retain_raw(self.root, rid, 'NVS', raw), {
            'RAW_NVS_RETAINED': True, 'RAW_NVS_FILE_PRESENT': True,
            'RAW_NVS_HAS_DATA': True, 'RAW_NVS_BYTES': 12,
        })
        path = capsule.run_path(self.root, rid) / 'evidence/raw-nvs'
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(common.sha(path), common.sha(raw))

    def test_physical_context_synthetic_only(self):
        with self.assertRaises(RuntimeError):
            with capsule.physical_run(self.root, 'synthetic', self.source, self.digests, lambda: {'ok': True}) as (manifest, budget):
                path = capsule.run_path(self.root, manifest['run_id'])
                self.assertTrue((path / 'runner/runner.py').exists())
                budget.invoke_once('Pair', lambda: None)  # Fake, no product adapter.
                self.assertEqual(common.read_json(path / 'ledger.json')['events'][0]['event'], 'SIDE_EFFECT_INVOKED')
                raise RuntimeError()
        self.assertEqual(capsule.inventory(self.root)[0]['state'], 'UNRESOLVED')

    def test_empty_uart_is_explicit(self):
        rid = self.create()
        raw = self.base / 'empty-uart'
        raw.touch()
        value = capsule.retain_raw(self.root, rid, 'UART', raw)
        self.assertTrue(value['RAW_UART_FILE_PRESENT'])
        self.assertFalse(value['RAW_UART_HAS_DATA'])
        self.assertEqual(value['RAW_UART_BYTES'], 0)

    def test_durable_startup_diagnostics(self):
        rid = self.create()
        clock = Clock()
        result = serial_startup.serial_readiness(
            lambda _: serial_startup.SerialResolution(False, False),
            lambda endpoint, remaining: self.fail('open'), lambda h, i: True,
            timeout=1, interval=.25, clock=clock, sleeper=clock.sleep,
        )
        record = dict(result, probe='opportunistic', reset_attempt=0, deadline_ms=1000)
        capsule.append_startup_attempt(self.root, rid, record)
        durable = common.read_json(capsule.run_path(self.root, rid) / 'startup.json')
        self.assertEqual(durable['attempts'][0]['serial']['open_attempt_count'], 0)
        self.assertEqual(durable['attempts'][0]['serial']['schema'], 2)
        self.assertNotIn('/dev/', json.dumps(durable))

    def test_durable_nested_open_exception_is_sanitized(self):
        rid = self.create()
        clock = Clock()
        root = PermissionError(13, '/private/device')
        outer = RuntimeError('USB_PRIVATE_SERIAL')
        outer.__cause__ = root
        result = serial_startup.serial_readiness(
            lambda _: serial_startup.SerialResolution(True, True, object()),
            lambda endpoint, remaining: (_ for _ in ()).throw(outer),
            lambda hello, info: True, timeout=1, interval=.25,
            clock=clock, sleeper=clock.sleep,
        )
        record = dict(result, probe='opportunistic', reset_attempt=0, deadline_ms=1000)
        capsule.append_startup_attempt(self.root, rid, record)
        durable = common.read_json(capsule.run_path(self.root, rid) / 'startup.json')
        serial = durable['attempts'][0]['serial']
        self.assertEqual(serial['outer_exception_class'], 'RuntimeError')
        self.assertEqual(serial['root_exception_class'], 'PermissionError')
        self.assertEqual(serial['wrapper_depth'], 1)
        self.assertEqual(serial['errno_category'], 'EACCES')
        self.assertEqual(serial['normalized_serial_category'], 'ACCESS_DENIED')
        self.assertTrue(serial['open_failure_terminal'])
        encoded = json.dumps(durable)
        self.assertNotIn('/private/device', encoded)
        self.assertNotIn('USB_PRIVATE_SERIAL', encoded)

    def test_runner_source_rejects_bytecode(self):
        generated = self.source / '__pycache__'
        generated.mkdir()
        (generated / 'runner.pyc').write_bytes(b'generated')
        with self.assertRaisesRegex(common.InfraError, 'RUNNER_GENERATED_FILE_REFUSED'):
            self.create()

    def test_runner_execution_copy_and_post_digest(self):
        (self.source / 'module.py').write_text('VALUE = 7\n')
        self.digest = self.manifest(self.source)
        self.digests = dict.fromkeys(('functional', 'firmware', 'parser'), self.digest)
        with capsule.physical_run(self.root, 'synthetic', self.source, self.digests,
                                  lambda: {'ok': True}) as (manifest, _budget):
            path = capsule.run_path(self.root, manifest['run_id'])
            with capsule.runner_execution(self.root, manifest['run_id']) as execution:
                spec = importlib.util.spec_from_file_location('synthetic_runner_module', execution / 'module.py')
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                self.assertEqual(module.VALUE, 7)
            self.assertFalse(any(p.name == '__pycache__' for p in (path / 'runner').rglob('*')))
            capsule.transition(self.root, manifest['run_id'], 'RESOLVED')
        status = common.read_json(path / 'checksums/post-run-runner.json')
        attribution = common.read_json(path / 'checksums/execution.json')
        self.assertTrue(status['exact'])
        self.assertTrue(attribution['matches_snapshot'])
        self.assertTrue(attribution['bytecode_disabled'])

    def test_physical_context_detects_snapshot_addition(self):
        with self.assertRaisesRegex(common.InfraError, 'RUNNER_SNAPSHOT_MUTATED'):
            with capsule.physical_run(self.root, 'synthetic', self.source, self.digests,
                                      lambda: {'ok': True}) as (manifest, _budget):
                path = capsule.run_path(self.root, manifest['run_id'])
                (path / 'runner').chmod(0o700)  # Simulate a privileged buggy executor.
                generated = path / 'runner/generated.pyc'
                generated.write_bytes(b'x')
                generated.chmod(0o600)
        item = capsule.inventory(self.root)[0]
        self.assertEqual(item['state'], 'UNRESOLVED')

    def test_fsync_file_and_directory(self):
        with patch.object(os, 'fsync', wraps=os.fsync) as synced:
            common.atomic_json(self.root / 'state/example.json', {'ok': True})
        self.assertEqual(synced.call_count, 2)


if __name__ == '__main__':
    unittest.main()
