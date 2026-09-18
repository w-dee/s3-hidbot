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
HANDLE = {'schema':3,'run_id':REQ['run_id'],'capture_id':REQ['capture_id'], 'runtime_id':'a'*64,'attempt_authority_sha256':REQ['authority_sha256']}
ENVELOPE = {'handle':HANDLE,'attempt_authority_sha256':REQ['authority_sha256'],'request':REQ}

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
        argv = p.command('capture', HANDLE)
        self.assertEqual(argv[:7], ['/usr/bin/sudo','-n','/usr/bin/python3','-I','-S','-B',
                                   '/usr/local/lib/s3-hidbot-authority-v3/authority_service.py'])
        self.assertEqual(argv[7:], ['capture', REQ['run_id']])
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
                with self.assertRaises(c.EvidenceError): p.verify_capture(ENVELOPE)
            self.assertFalse(marker.exists())

    def test_untrusted_stdout(self):
        for stdout in (b'{"schema":2,"schema":2}', c.encode(fixture({**REQ, 'capture_id':'f'*32})),
                       c.encode(fixture()) + c.encode(fixture())):
            with mock.patch.object(p.subprocess, 'run', return_value=subprocess.CompletedProcess([],0,stdout,b'')):
                with self.assertRaises(c.EvidenceError): p.verify_capture(ENVELOPE)


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

    def test_real_process_exit_and_parent_death(self):
        for phase in ('writer_stop','receipt'):
            root=self.base/('exit-'+phase); root.mkdir(mode=0o700)
            producer=self.make(); producer.root=root
            pid=os.fork()
            if pid==0:
                producer.cut=lambda point: os._exit(77) if point==phase else None
                self.run_capture(producer); os._exit(9)
            self.assertEqual(os.waitpid(pid,0)[1],77<<8)
            value=producer.operate(REQ)
            self.assertEqual(value['status'],'FINALIZED'); self.assertEqual(producer.operate(REQ),value)
        read,write=os.pipe(); pid=os.fork()
        if pid==0:
            os.close(read)
            def control(process):
                os.write(write,str(process.pid).encode()+b'\n'); time.sleep(10); return False
            self.run_capture(control=control); os._exit(9)
        os.close(write); writer_pid=int(os.read(read,64)); os.close(read)
        with self.assertRaisesRegex(c.EvidenceError,'CAPTURE_ACTIVE'): self.producer.operate(REQ)
        os.kill(pid,signal.SIGKILL); os.waitpid(pid,0)
        deadline=time.monotonic()+2; state='unknown'
        while time.monotonic()<deadline:
            try: state=Path('/proc/'+str(writer_pid)+'/stat').read_text().split()[2]
            except (FileNotFoundError, ProcessLookupError): state='GONE'
            if state in ('GONE','Z'): break
            time.sleep(.01)
        self.assertIn(state,('GONE','Z'))
        value=self.producer.operate(REQ)
        self.assertEqual(value['status'],'FAILED'); self.assertEqual(value['error'],'CAPTURE_INTERRUPTED')


if __name__ == '__main__': unittest.main()
