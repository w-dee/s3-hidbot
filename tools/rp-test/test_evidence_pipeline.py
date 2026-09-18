"""Adversarial authority tests: real descriptors/writers, explicit protocol fixtures."""
import copy
import hashlib
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'qualification_campaign'))
import evidence_contract as c
import evidence_pipeline as p
import privileged_evidence as h
import official_campaign as coordinator
import q8_capture

IDENTITY = {'version': 2, 'helper_sha256': 'a' * 64, 'imports': {'evidence_contract.py': 'b' * 64}}
REQ = {'schema': 2, 'attempt_id': '20260918T120000Z-0123456789ab',
       'run_id': '20260918T120000Z-0123456789ab', 'capture_id': 'c' * 32,
       'kind': 'Q8_HCI', 'authority_sha256': 'd' * 64, 'helper': IDENTITY, 'max_seconds': 2}
COUNTS = {'pairing_requests': 1, 'pairing_responses': 1, 'security_requests': 0}
WRITER = '''import os,signal,sys,time
mode=sys.argv[1]
def stop(*args):
    if mode == 'delay': time.sleep(.06)
    raise SystemExit(23 if mode == 'bad' else 0)
signal.signal(signal.SIGINT, signal.SIG_IGN if mode in ('term','kill') else stop)
if mode == 'kill': signal.signal(signal.SIGTERM,signal.SIG_IGN)
f=open(sys.argv[-1],'wb',buffering=0);f.write(b'capture-header-0123456789');os.fsync(f.fileno())
if mode == 'early': raise SystemExit(0)
while True: time.sleep(.01)
'''


def fixture(req=REQ, *, status='FINALIZED'):
    """Schema fixture only; real root/nonroot separation has a separate process test."""
    return {'schema': 2, 'request': copy.deepcopy(req), 'helper': copy.deepcopy(req['helper']),
            'status': status, 'error': None if status == 'FINALIZED' else 'WRITER_NOT_ACCEPTED',
            'raw': {'relative_name': req['run_id'] + '/' + req['capture_id'] + '/raw-hci',
                    'object': {'device': 1, 'inode': 9}, 'sha256': 'e' * 64, 'bytes': 24,
                    'mode': '0600', 'uid': 0, 'gid': 0, 'mtime_ns': 1, 'ctime_ns': 1},
            'writer': {'pid': 123, 'exit_code': 0 if status == 'FINALIZED' else 23,
                       'requested_stop': True, 'forced_signal': None, 'reaped': True, 'early_exit': False},
            'counts': dict(COUNTS)}


class ContractTests(unittest.TestCase):
    def test_namespace_grammar(self):
        for run in ('..', '.', '', '/tmp/x', 'private/../run', REQ['run_id'] + '/x',
                    REQ['run_id'] + '\\x', REQ['run_id'] + '\n', '２０２６0918T120000Z-0123456789ab'):
            with self.subTest(run=run), self.assertRaises(c.EvidenceError):
                c.run_id(run)
        self.assertEqual(c.run_id(REQ['run_id']), REQ['run_id'])

    def test_strict_json(self):
        for data in ('{"schema":2,"schema":2}', '{"x":{"a":1,"a":2}}', '{"x":NaN}'):
            with self.subTest(data=data), self.assertRaises(c.EvidenceError):
                c.decode(data)
        for field, val in (('schema', True), ('schema', 1), ('schema', '2')):
            receipt = fixture(); receipt[field] = val
            with self.assertRaises(c.EvidenceError): c.receipt(receipt, REQ)
        for change in ('unknown', 'missing', 'bool-size', 'bool-version', 'writer-bool'):
            value = fixture()
            if change == 'unknown': value['extra'] = 1
            if change == 'missing': del value['counts']
            if change == 'bool-size': value['raw']['bytes'] = True
            if change == 'bool-version': value['helper']['version'] = True
            if change == 'writer-bool': value['writer']['exit_code'] = False
            with self.subTest(change=change), self.assertRaises(c.EvidenceError): c.receipt(value, REQ)

    def test_binding(self):
        for key, val in (('run_id', '20260918T120001Z-0123456789ab'), ('capture_id', 'f'*32),
                         ('attempt_id', '20260918T120002Z-0123456789ab'), ('kind', 'REHEARSAL_HCI'),
                         ('authority_sha256', 'f'*64)):
            expected = copy.deepcopy(REQ); expected[key] = val
            with self.subTest(key=key), self.assertRaises(c.EvidenceError): c.receipt(fixture(), expected)
        for key in ('helper', 'import'):
            value = fixture()
            if key == 'helper': value['helper']['helper_sha256'] = 'f'*64
            else: value['helper']['imports']['evidence_contract.py'] = 'f'*64
            with self.assertRaises(c.EvidenceError): c.receipt(value, REQ)

    def test_writer_receipt_policy(self):
        for field, value in (('exit_code', 23), ('early_exit', True), ('reaped', False),
                             ('requested_stop', False), ('forced_signal', 'SIGTERM'), ('forced_signal', 'SIGKILL')):
            receipt = fixture(); receipt['writer'][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(c.EvidenceError):
                c.receipt(receipt, REQ)

    def test_exact_invocation(self):
        argv = p.command('capture', REQ)
        self.assertEqual(argv[:6], ['/usr/bin/sudo','-n','/usr/bin/python3','-I','-B',
                                   '/usr/local/lib/s3-hidbot-evidence-v2/privileged_evidence.py'])
        self.assertEqual(argv[6:], ['capture', REQ['run_id'], REQ['capture_id']])
        self.assertEqual(h.ENV, {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C'})

    def test_fake_path_sudo(self):
        with tempfile.TemporaryDirectory() as temp:
            fake = Path(temp) / 'sudo'; marker = Path(temp) / 'used'
            fake.write_text('#!/bin/sh\ntouch "' + str(marker) + '"\nprintf \'{}\\n\'\n'); fake.chmod(0o700)
            def intercept(argv, **kwargs):
                self.assertEqual(argv[0], '/usr/bin/sudo')
                self.assertEqual(kwargs['env'], h.ENV)
                return subprocess.CompletedProcess(argv, 1, b'', b'')
            with mock.patch.dict(os.environ, {'PATH': temp}), mock.patch.object(p.subprocess, 'run', intercept):
                with self.assertRaises(c.EvidenceError): p.verify_capture(REQ)
            self.assertFalse(marker.exists())

    def test_untrusted_stdout(self):
        for stdout in (b'{"schema":2,"schema":2}', c.encode(fixture({**REQ, 'capture_id':'f'*32})),
                       c.encode(fixture()) + c.encode(fixture())):
            with mock.patch.object(p.subprocess, 'run', return_value=subprocess.CompletedProcess([],0,stdout,b'')):
                with self.assertRaises(c.EvidenceError): p.verify_capture(REQ)


class ProducerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name); self.root = self.base / 'private'; self.root.mkdir(mode=0o700)
        self.writer = self.base / 'writer.py'; self.writer.write_text(WRITER)
        self.producer = self.make()

    def make(self, mode='normal', **kwargs):
        return h.Producer(self.root, IDENTITY, owner=os.getuid(),
                          writer=(sys.executable, str(self.writer), mode), decoder=lambda _: dict(COUNTS),
                          waits=(.15,.12,1), **kwargs)

    def run_capture(self, producer=None, **kwargs):
        control = kwargs.pop('control', lambda _: True)
        return (producer or self.producer).operate(REQ, capture=True, control=control, **kwargs)

    @property
    def raw(self): return self.root / REQ['run_id'] / REQ['capture_id'] / 'raw-hci'

    def test_real_writer_and_reverify(self):
        value = self.run_capture()
        self.assertEqual(value['status'], 'FINALIZED')
        self.assertEqual(value['writer']['exit_code'], 0)
        self.assertEqual(value['raw']['object'], c.object_identity(self.raw.stat()))
        self.assertEqual(value['raw']['sha256'], hashlib.sha256(self.raw.read_bytes()).hexdigest())
        self.assertEqual(self.producer.operate(REQ), value)
        self.assertEqual(self.run_capture(), value)  # capture ID is never reused to start a writer

    def test_writer_abnormal_and_escalated(self):
        for mode in ('bad', 'early', 'term', 'kill'):
            with self.subTest(mode=mode):
                root = self.base / mode; root.mkdir(mode=0o700)
                producer = self.make(mode); producer.root = root
                value = self.run_capture(producer, control=(lambda process: (process.wait(timeout=1), False)[1]) if mode == 'early' else lambda _: True)
                self.assertEqual(value['status'], 'FAILED')
                self.assertTrue(value['raw']['bytes'] > 0)
                self.assertTrue(value['writer']['reaped'])
                if mode in ('term','kill'):
                    self.assertEqual(value['writer']['forced_signal'], 'SIGTERM' if mode == 'term' else 'SIGKILL')
                if mode == 'bad': self.assertEqual(value['writer']['exit_code'], 23)
                if mode == 'early': self.assertTrue(value['writer']['early_exit'])

    def test_delayed_stop_and_alive(self):
        start = time.monotonic()
        def ready(_):
            with self.assertRaisesRegex(c.EvidenceError, 'CAPTURE_ACTIVE'): self.producer.operate(REQ)
            self.assertFalse((self.raw.parent / 'receipt.json').exists())
        value = self.run_capture(self.make('delay'), ready=ready)
        self.assertEqual(value['status'], 'FINALIZED'); self.assertGreater(time.monotonic()-start,.06)

    def test_wrong_helper_or_import_before_capture(self):
        for field in ('helper', 'import'):
            req = copy.deepcopy(REQ)
            if field == 'helper': req['helper']['helper_sha256'] = 'f'*64
            else: req['helper']['imports']['evidence_contract.py'] = 'f'*64
            with self.assertRaisesRegex(c.EvidenceError,'HELPER_IDENTITY_MISMATCH'):
                self.producer.operate(req, capture=True)
            self.assertFalse(self.raw.exists())

    def test_configured_root_object_cannot_be_replaced(self):
        producer = self.make(root_object=c.object_identity(self.root.stat()))
        self.root.rename(self.base / 'original-root'); self.root.mkdir(mode=0o700)
        with self.assertRaisesRegex(c.EvidenceError, 'ROOT_OBJECT_CHANGED'):
            self.run_capture(producer)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_symlink_parent_and_leaf(self):
        outside = self.base / 'outside'; outside.mkdir()
        (self.root / REQ['run_id']).symlink_to(outside)
        with self.assertRaises(OSError): self.run_capture()
        self.assertEqual(list(outside.iterdir()), [])
        (self.root / REQ['run_id']).unlink()
        capture = self.raw.parent; capture.mkdir(parents=True, mode=0o700); capture.parent.chmod(0o700)
        target = outside / 'target'; target.write_bytes(b'untouched'); target.chmod(0o640)
        self.raw.symlink_to(target)
        with self.assertRaises(OSError): self.run_capture()
        self.assertEqual(target.read_bytes(), b'untouched'); self.assertEqual(stat.S_IMODE(target.stat().st_mode),0o640)

    def test_hardlink_leaf_never_mutated(self):
        self.raw.parent.mkdir(parents=True,mode=0o700); self.raw.parent.parent.chmod(0o700)
        target=self.base/'external'; target.write_bytes(b'outside'); target.chmod(0o640)
        os.link(target,self.raw)
        before=c.signature(target.stat())
        with self.assertRaises(FileExistsError): self.run_capture()
        self.assertEqual(c.signature(target.stat()),before)
        self.assertEqual(target.read_bytes(),b'outside')

    def test_parent_swap_never_touches_external(self):
        outside = self.base / 'outside'; outside.mkdir()
        target = outside / 'raw-hci'; target.write_bytes(b'outside'); target.chmod(0o640)
        def swap(phase):
            if phase == 'writer_stop':
                self.raw.parent.rename(self.raw.parent.with_name('retained'))
                self.raw.parent.symlink_to(outside)
        with self.assertRaises((c.EvidenceError,OSError)): self.run_capture(self.make(cut=swap))
        self.assertEqual(target.read_bytes(), b'outside'); self.assertEqual(stat.S_IMODE(target.stat().st_mode),0o640)

    def test_raw_replacement_after_stop(self):
        def swap(phase):
            if phase == 'writer_stop':
                self.raw.rename(self.raw.with_name('original'))
                self.raw.write_bytes(b'replacement'); self.raw.chmod(0o640)
        with self.assertRaisesRegex(c.EvidenceError,'RAW_NAME_CHANGED'): self.run_capture(self.make(cut=swap))
        self.assertEqual(self.raw.read_bytes(), b'replacement'); self.assertEqual(stat.S_IMODE(self.raw.stat().st_mode),0o640)

    def test_hardlink_rejected_before_chmod(self):
        def link(phase):
            if phase == 'writer_stop':
                self.raw.chmod(0o640); os.link(self.raw, self.base/'linked')
        with mock.patch.object(h.os,'fchmod', wraps=os.fchmod) as chmod:
            with self.assertRaisesRegex(c.EvidenceError,'RAW_OBJECT_INVALID'): self.run_capture(self.make(cut=link))
        self.assertEqual(stat.S_IMODE((self.base/'linked').stat().st_mode),0o640)
        # fchmod calls on journals are allowed; none can target the hardlinked raw object.
        self.assertEqual(self.raw.stat().st_nlink,2)

    def test_directory_entry_must_match_held_fd(self):
        self.raw.parent.mkdir(parents=True, mode=0o700)
        self.raw.write_bytes(b'original')
        with c.Directory(self.raw.parent) as directory:
            fd=os.open(self.raw,os.O_RDONLY)
            try:
                self.raw.rename(self.raw.with_name('old')); self.raw.write_bytes(b'replaced')
                with self.assertRaisesRegex(c.EvidenceError,'RAW_NAME_CHANGED'):
                    directory.check_file('raw-hci',fd)
            finally: os.close(fd)

    def test_rename_during_hash(self):
        original = c.stable_hash
        def hash_then_swap(fd):
            result = original(fd); self.raw.rename(self.raw.with_name('retained')); self.raw.write_bytes(b'new')
            return result
        with mock.patch.object(c,'stable_hash',hash_then_swap), self.assertRaisesRegex(c.EvidenceError,'RAW_NAME_CHANGED'):
            self.run_capture()

    def test_mutation_during_hash(self):
        read = os.read; once = []
        def mutate(fd, size):
            data = read(fd,size)
            if size == 1024*1024 and data and not once:
                once.append(True)
                with self.raw.open('ab') as stream: stream.write(b'changed')
            return data
        with mock.patch.object(c.os,'read',mutate), self.assertRaisesRegex(c.EvidenceError,'RAW_CHANGED'):
            self.run_capture()

    def test_crash_writer_stop_and_receipt(self):
        for phase in ('writer_stop','receipt'):
            root = self.base/phase; root.mkdir(mode=0o700)
            def cut(point):
                if point == phase: raise InterruptedError('crash')
            producer = self.make(cut=cut); producer.root = root
            with self.assertRaises(InterruptedError): self.run_capture(producer)
            producer.cut = lambda _: None
            value = producer.operate(REQ)
            self.assertEqual(value['status'],'FINALIZED'); self.assertEqual(producer.operate(REQ),value)

    def test_interrupted_capture_never_recaptures(self):
        capture = self.raw.parent; capture.mkdir(parents=True, mode=0o700); capture.parent.chmod(0o700)
        (capture/'request.json').write_bytes(c.encode(REQ)); (capture/'lock').touch()
        value = self.run_capture()
        self.assertEqual(value['status'],'FAILED'); self.assertEqual(value['error'],'CAPTURE_INTERRUPTED')
        self.assertFalse(self.raw.exists())


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.cleanup)
        self.base = Path(self.temp.name)

    def cleanup(self):
        for path in self.base.rglob('*'):
            if path.is_dir(): path.chmod(0o700)
        self.temp.cleanup()

    def package(self): return p.create_package(self.base, p.CLASSIFICATION, {'helper': IDENTITY})
    def finish(self, package, passed=True, **kwargs):
        return coordinator.finish(package, passed, {'phase':'q8'}, _verify=fixture, **kwargs)

    def test_crash_matrix_and_test_immutability(self):
        for outcome in (True,False):
            for phase in ('test','manifest','index','permissions','before_commit','after_commit'):
                with self.subTest(outcome=outcome, phase=phase):
                    package = self.package()
                    def cut(point):
                        if point == phase: raise InterruptedError(point)
                    with self.assertRaises(InterruptedError): self.finish(package,outcome,_cut=cut)
                    commit = self.base/'commits'/(package.name+'.json')
                    self.assertEqual(commit.exists(),phase == 'after_commit')
                    for name in ('manifest.json','index.json'):
                        if (package/name).exists(): self.assertEqual(c.decode((package/name).read_bytes())['state'],'PREPARED')
                    with self.assertRaisesRegex(c.EvidenceError,'IMMUTABLE_CONFLICT'): self.finish(package,not outcome)
                    terminal = self.finish(package,outcome)
                    self.assertEqual(terminal['code'],'SUCCESS' if outcome else 'TEST_FAILED')
                    self.assertEqual(self.finish(package,outcome),terminal)
                    self.assertEqual(stat.S_IMODE(package.stat().st_mode),0o500)
                    for name in p.FILES: self.assertEqual(stat.S_IMODE((package/name).stat().st_mode),0o400)
                    self.assertEqual(p.tree_digest(package),p.tree_digest(package))

    def test_commit_is_last(self):
        package = self.package(); steps=[]
        def cut(point):
            steps.append(point)
            if point != 'after_commit': self.assertFalse((self.base/'commits'/(package.name+'.json')).exists())
            if point == 'before_commit':
                self.assertTrue((package/'index.json').is_file())
                self.assertEqual(stat.S_IMODE(package.stat().st_mode),0o500)
        self.finish(package,_cut=cut)
        self.assertEqual(steps,['test','manifest','index','permissions','before_commit','after_commit'])

    def test_interrupted_atomic_write_recovery(self):
        package = self.package()
        pending = package / ('.pending-' + 'a' * 24)
        pending.write_bytes(b'{"incomplete":'); pending.chmod(0o600)
        result = self.finish(package)
        self.assertEqual(result['code'], 'SUCCESS')
        self.assertFalse(pending.exists())
        self.assertEqual(set(x.name for x in package.iterdir()), set((*p.FILES, 'lock')))

    def test_pending_symlink_is_not_followed(self):
        package = self.package(); outside = self.base / 'outside'
        outside.write_bytes(b'untouched'); outside.chmod(0o640)
        (package / ('.pending-' + 'a' * 24)).symlink_to(outside)
        with self.assertRaises(OSError): self.finish(package)
        self.assertEqual(outside.read_bytes(), b'untouched')
        self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o640)

    def test_abnormal_receipt_never_success(self):
        package = self.package()
        result = coordinator.finish(package,True,{},_verify=lambda req:fixture(req,status='FAILED'))
        self.assertEqual(result['code'],'EVIDENCE_FINALIZATION_FAILED')
        self.assertEqual(coordinator.finish(package,True,{},_verify=fixture),result)

    def test_replay_or_helper_failure_not_success(self):
        for verify in (lambda _:fixture(), lambda req: (_ for _ in ()).throw(c.EvidenceError('FAILED'))):
            result=coordinator.finish(self.package(),True,{},_verify=verify)
            self.assertEqual(result['code'],'EVIDENCE_FINALIZATION_FAILED')

    def test_no_caller_supplied_receipt_api(self):
        with self.assertRaises(TypeError):
            coordinator.finish(self.package(), True, {}, receipt=fixture())

    def test_no_raw_access_and_q8_integration(self):
        for passed in (True,False):
            package = self.package(); req = p.package_request(package); events=[]
            class Session:
                def __init__(self, request): self.request=request
                def start(self): events.append('start'); return self
                def stop(self): events.append('stop'); return fixture(self.request)
            result=q8_capture.capture_pair(req,lambda:events.append('pair'),_session=Session)
            self.assertEqual(events,['start','pair','stop']); self.assertEqual(result['counts'],COUNTS)
            # Metadata tree contains no raw, recursive raw hashing cannot work here.
            terminal=self.finish(package,passed)
            self.assertEqual(terminal['code'],'SUCCESS' if passed else 'TEST_FAILED')
            self.assertEqual(c.decode((package/'index.json').read_bytes())['raw_evidence']['sha256'],'e'*64)

    def test_q8_pair_failure_stops_writer(self):
        events=[]
        class Session:
            def __init__(self, req): pass
            def start(self): return self
            def stop(self): events.append('stopped'); return fixture()
        with self.assertRaisesRegex(RuntimeError,'pair'):
            q8_capture.capture_pair(REQ,lambda: (_ for _ in ()).throw(RuntimeError('pair')),_session=Session)
        self.assertEqual(events,['stopped'])

    def test_q8_semantic_and_writer_failure(self):
        for abnormal in (False,True):
            value=fixture(status='FAILED' if abnormal else 'FINALIZED')
            if not abnormal: value['counts']['security_requests']=1
            class Session:
                def __init__(self,req): pass
                def start(self): return self
                def stop(self): return value
            with self.assertRaises(c.EvidenceError): q8_capture.capture_pair(REQ,lambda:None,_session=Session)

    def test_future_q8_uses_only_boundary(self):
        source=(Path(coordinator.__file__).parent/'q8_host_security.py').read_text()
        self.assertIn('capture_pair(package_request(package)',source)
        for obsolete in ('monitor=subprocess.Popen','os.chmod(capture','decoded_text','q8-host-security.btsnoop'):
            self.assertNotIn(obsolete,source)
        source=Path(coordinator.__file__).read_text()
        self.assertNotIn('capsule.rglob',source)
        self.assertIn('finalize_terminal_package',source)


if __name__ == '__main__': unittest.main()
