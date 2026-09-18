"""Production snapshot/sealer oracles; fixture capture metadata is explicitly synthetic."""
import copy
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import evidence_contract as c
import authority_runtime as a
import authority_service as s
import evidence_pipeline as p
from test_evidence_pipeline import fixture, REQ, ENVELOPE, COUNTS
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'qualification_campaign'))
import official_campaign as coordinator
import q8_capture

HERE=Path(__file__).resolve().parent


def replace(path,data):
    path.chmod(0o600); path.write_bytes(data); path.chmod(0o400 if path.name.endswith('.json') else 0o444)


class FixtureProducer:
    def operate(self,req,**kwargs): return fixture(req)


class AuthorityTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.cleanup)
        self.base=Path(self.temp.name); self.install=self.base/'install'; self.install.mkdir(mode=0o755)
        for name in a.ENGINE:
            shutil.copyfile(HERE/name,self.install/name); (self.install/name).chmod(0o444)
        self.state=self.base/'state'; self.state.mkdir(mode=0o755)
        for name,mode in (('attempts',0o700),('captures',0o700),('staging',0o755)): (self.state/name).mkdir(mode=mode)
        (self.install/'roots.json').write_bytes(c.encode({name:c.object_identity(path.stat()) for name,path in
            (('state',self.state),('captures',self.state/'captures'))}))
        (self.install/'roots.json').chmod(0o400)
        root=self.install/'runtimes'/'building'; root.mkdir(parents=True)
        for name in ('official_campaign.py','q8_capture.py'):
            shutil.copyfile(HERE.parent/'qualification_campaign'/name,root/name)
        (root/'qualification-plan.md').write_text('synthetic plan\n')
        (root/'FROZEN.json').write_bytes(c.encode({'schema':3,'description':'synthetic'}))
        rows=a.membership(root)
        manifest={'schema':3,'files':rows,'engine':a.engine_identity(self.install,os.getuid()),'product':a.PRODUCT,
                  'coordinator':rows['official_campaign.py'],'plan':rows['qualification-plan.md'],'frozen':rows['FROZEN.json']}
        (root/'BUNDLE.json').write_bytes(c.encode(manifest)); self.runtime_id=c.digest(manifest)
        self.root=root.with_name(self.runtime_id); root.rename(self.root)
        for path in self.root.iterdir(): path.chmod(0o444)
        self.root.chmod(0o555)
        self.service=s.Authority(self.state,self.install,owner=os.getuid(),producer=FixtureProducer())
        self.run=p.new_id(); response=self.service.begin(self.runtime_id,self.run,a.CLASSIFICATION,os.getuid(),2)
        self.handle=response['handle']; self.snapshot=response['snapshot']; self.uid=os.getuid()
        self.attempt=self.state/'attempts'/self.run; self.stage=self.state/'staging'/self.run
        self.package=self.attempt/'package'

    def cleanup(self):
        for directory,dirs,files in os.walk(self.base):
            os.chmod(directory,0o700)
            for name in files:
                if not (Path(directory)/name).is_symlink(): (Path(directory)/name).chmod(0o600)
        self.temp.cleanup()

    def prepare(self,outcome='PASS'):
        self.service.record(self.handle,self.uid,outcome,{'classification':a.CLASSIFICATION})
        value=self.service.prepare(self.handle,self.uid)
        for name,data in value['files'].items():
            path=self.stage/name
            if not path.exists(): path.write_bytes(c.encode(data)); path.chmod(0o400)
        return value

    def activate(self):
        return self.service.activate(self.handle, self.uid)

    def seal(self,value): return self.service.seal(self.handle,self.uid,c.digest(value))

    def no_commit(self): self.assertFalse((self.attempt/'commit.json').exists())

    def test_positive_final_bytes_and_retry(self):
        prepared=self.prepare(); commit=self.seal(prepared)
        self.assertEqual(commit['code'],'SUCCESS'); self.assertEqual(self.seal(prepared),commit)
        for name,record in commit['files'].items():
            path=self.package/name
            self.assertEqual(record['sha256'],a.sha(path.read_bytes()))
            self.assertEqual(record['object'],c.object_identity(path.stat()))
            self.assertEqual(stat.S_IMODE(path.stat().st_mode),0o400)
        self.assertEqual(commit['index_sha256'],a.sha((self.package/'index.json').read_bytes()))
        self.assertEqual(commit['handle']['attempt_authority_sha256'],c.digest(self.snapshot))

    def test_runtime_rebase_rejected(self):
        replace(self.root/'official_campaign.py',b"raise RuntimeError('replacement')\n")
        replace(self.root/'FROZEN.json',c.encode({'new':'authority'}))
        manifest=c.decode((self.root/'BUNDLE.json').read_bytes()); manifest['files']=a.membership(self.root)
        manifest['coordinator']=manifest['files']['official_campaign.py']; manifest['frozen']=manifest['files']['FROZEN.json']
        replace(self.root/'BUNDLE.json',c.encode(manifest))
        with self.assertRaises(c.EvidenceError): self.service.load(self.handle,self.uid)
        self.no_commit()

    def test_all_authority_mutations_rejected(self):
        for name in ('official_campaign.py','qualification-plan.md','FROZEN.json','BUNDLE.json'):
            path=self.root/name; original=path.read_bytes()
            replace(path,original+b'\n')
            with self.subTest(name=name),self.assertRaises(c.EvidenceError): self.service.load(self.handle,self.uid)
            replace(path,original)
        original=(self.attempt/'authority.json').read_bytes()
        changed=copy.deepcopy(self.snapshot); changed['classification']='replacement'
        replace(self.attempt/'authority.json',c.encode(changed))
        with self.assertRaises(c.EvidenceError): self.service.load(self.handle,self.uid)
        replace(self.attempt/'authority.json',original)
        self.assertEqual(self.service.load(self.handle,self.uid)['snapshot'],self.snapshot)
        for name in a.ENGINE:
            path=self.install/name; original=path.read_bytes(); replace(path,original+b'\n')
            with self.subTest(engine=name),self.assertRaises(c.EvidenceError): self.service.load(self.handle,self.uid)
            replace(path,original)

    def test_identical_frozen_rewrite_and_resume(self):
        path=self.root/'FROZEN.json'; replace(path,path.read_bytes())
        fresh=s.Authority(self.state,self.install,owner=self.uid,producer=FixtureProducer())
        self.assertEqual(fresh.load(self.handle,self.uid)['snapshot'],self.snapshot)

    def test_bytecode_forbidden_even_if_manifest_lists_it(self):
        # Rebuild expected manifest to isolate the bytecode policy from membership mismatch.
        for name in ('q8_capture.pyc','q8_capture.pyo','__pycache__'):
            with self.subTest(name=name):
                self.root.chmod(0o755); path=self.root/name
                if name=='__pycache__': path.mkdir(); path.chmod(0o555)
                else: path.write_bytes(b'unmeasured'); path.chmod(0o444)
                self.root.chmod(0o555)
                with self.assertRaisesRegex(c.EvidenceError,'BYTECODE_FORBIDDEN'): a.membership(self.root,self.uid)
                self.root.chmod(0o755)
                if path.is_dir(): path.rmdir()
                else: path.unlink()
                self.root.chmod(0o555)

    def test_source_finder_in_isolated_interpreter(self):
        program="""import sys,types
from pathlib import Path
base=Path(sys.argv[1]); root=Path(sys.argv[2])
for name in ('evidence_contract','authority_runtime'):
 module=types.ModuleType(name);module.__file__=str(base/(name+'.py'));sys.modules[name]=module
 exec(compile(Path(module.__file__).read_bytes(),module.__file__,'exec'),module.__dict__)
import authority_runtime as a
spec=a.SourceFinder(root,{'files':{'q8_capture.py':'0'*64}}).find_spec('q8_capture')
assert type(spec.loader).__name__=='SourceLoader'
print(spec.origin)
"""
        result=subprocess.run(['/usr/bin/python3','-I','-S','-B','-c',program,str(HERE),str(self.root)],
                              capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.strip(),str(self.root/'q8_capture.py'))

    def test_runtime_membership(self):
        self.root.chmod(0o755); path=self.root/'unknown.py'; path.write_text('raise SystemExit()'); path.chmod(0o444)
        self.root.chmod(0o555)
        with self.assertRaises(c.EvidenceError): self.service.load(self.handle,self.uid)

    def test_request_authority(self):
        self.activate()
        env=self.service.load(self.handle,self.uid)['request']; env=copy.deepcopy(env)
        env['attempt_authority_sha256']='f'*64
        with self.assertRaises(c.EvidenceError): a.capture_request(env)
        env=self.service.load(self.handle,self.uid)['request']; env=copy.deepcopy(env)
        env['request']['max_seconds']=3
        with self.assertRaises(c.EvidenceError): self.service.capture(env,self.uid)

    def test_persisted_request_cannot_rebase_snapshot(self):
        path=self.attempt/'request.json'; original=path.read_bytes()
        for field,value in (('kind','Q8_HCI'),('max_seconds',3),('helper',{'version':2,'helper_sha256':'f'*64,
                'imports':{'evidence_contract.py':'e'*64}})):
            changed=c.decode(original); changed['request'][field]=value
            replace(path,c.encode(changed))
            with self.subTest(field=field),self.assertRaisesRegex(c.EvidenceError,'AUTHORITY_CONFLICT'):
                self.service.load(self.handle,self.uid)
            replace(path,original)

    def test_stage_authority_replacement(self):
        prepared=self.prepare()
        for field in ('classification','runtime_id'):
            value=copy.deepcopy(self.snapshot); value[field]='replacement'
            replace(self.stage/'authority.json',c.encode(value))
            with self.subTest(field=field),self.assertRaises(c.EvidenceError): self.seal(prepared)
            self.no_commit()

    def test_all_staged_metadata_replacements(self):
        prepared=self.prepare()
        for name in a.FILES:
            path=self.stage/name; original=path.read_bytes(); replace(path,c.encode({'replacement':name}))
            with self.subTest(name=name),self.assertRaises(c.EvidenceError): self.seal(prepared)
            self.no_commit(); replace(path,original)
        self.assertEqual(self.seal(prepared)['code'],'SUCCESS')

    def final_fault(self,name,point='before_validation'):
        prepared=self.prepare()
        def cut(phase):
            if phase == point:
                replace(self.package/name,c.encode({'replacement':name}))
        self.service.cut=cut
        with self.assertRaises(c.EvidenceError): self.seal(prepared)
        self.no_commit()

    def test_final_index_rehash(self): self.final_fault('index.json')
    def test_final_manifest_rehash(self): self.final_fault('manifest.json')
    def test_change_after_first_final_hash(self): self.final_fault('index.json','validated')

    def test_terminal_corruption_rejected(self):
        prepared=self.prepare(); self.seal(prepared)
        for name in ('index.json','manifest.json','authority.json'):
            path=self.package/name; original=path.read_bytes(); replace(path,original+b' ')
            with self.subTest(name=name),self.assertRaises(c.EvidenceError): self.seal(prepared)
            replace(path,original)
        path=self.package/'index.json'; path.chmod(0o600)
        with self.assertRaises(c.EvidenceError): self.seal(prepared)

    def test_seal_does_not_chmod_untrusted_files(self):
        prepared=self.prepare(); outside=self.base/'outside'; outside.write_bytes(b'outside'); outside.chmod(0o640)
        (self.stage/'index.json').unlink(); (self.stage/'index.json').symlink_to(outside)
        with self.assertRaises(OSError): self.seal(prepared)
        self.assertEqual(outside.read_bytes(),b'outside'); self.assertEqual(stat.S_IMODE(outside.stat().st_mode),0o640)
        self.no_commit()

    def test_commit_last(self):
        prepared=self.prepare(); seen=[]
        def cut(phase):
            seen.append(phase)
            if phase != 'after_commit': self.no_commit()
            if phase == 'before_commit': self.assertTrue((self.package/'index.json').exists())
        self.service.cut=cut; self.seal(prepared)
        self.assertIn('before_validation',seen); self.assertEqual(seen[-1],'after_commit')

    def test_immutable_outcome(self):
        self.prepare('FAIL')
        with self.assertRaisesRegex(c.EvidenceError,'IMMUTABLE_CONFLICT'):
            self.service.record(self.handle,self.uid,'PASS',{'classification':a.CLASSIFICATION})

    def test_failure_never_upgrades(self):
        class Failed:
            def operate(self,req,**kwargs): return fixture(req,status='FAILED')
        self.activate(); self.service.producer_override=Failed(); prepared=self.prepare()
        self.service.producer_override=FixtureProducer()
        self.assertEqual(self.service.prepare(self.handle,self.uid),prepared)
        self.assertEqual(self.seal(prepared)['code'],'EVIDENCE_FINALIZATION_FAILED')

    def test_crash_matrix(self):
        # Subprocess exits are covered in the root integration; here replay every production seam.
        prepared=self.prepare('FAIL')
        for target in (*a.FILES,'staged','before_validation','validated','before_commit','after_commit'):
            def cut(point):
                if point==target: raise InterruptedError(point)
            self.service.cut=cut
            with self.subTest(target=target),self.assertRaises(InterruptedError): self.seal(prepared)
            if target!='after_commit': self.no_commit()
        self.service.cut=lambda _:None
        self.assertEqual(self.seal(prepared)['code'],'TEST_FAILED')

    def test_actual_process_crashes(self):
        prepared=self.prepare('FAIL')
        for target in ('staged',*a.FILES,'before_validation','validated','before_commit','after_commit'):
            pid=os.fork()
            if pid==0:
                self.service.cut=lambda point: os._exit(77) if point==target else None
                self.seal(prepared); os._exit(9)
            self.assertEqual(os.waitpid(pid,0)[1],77<<8)
            if target!='after_commit': self.no_commit()
        self.assertEqual(self.seal(prepared)['code'],'TEST_FAILED')
        self.assertEqual(self.seal(prepared)['code'],'TEST_FAILED')

    def test_atomic_write_process_crash(self):
        prepared=self.prepare('FAIL'); pid=os.fork()
        if pid==0:
            original=os.fchmod
            def die(fd,mode):
                if os.readlink('/proc/self/fd/'+str(fd)).split('/')[-1].startswith('.pending-'): os._exit(78)
                original(fd,mode)
            with mock.patch.object(c.os,'fchmod',die): self.seal(prepared)
            os._exit(9)
        self.assertEqual(os.waitpid(pid,0)[1],78<<8)
        self.assertTrue(list(self.package.glob('.pending-*')))
        self.assertEqual(self.seal(prepared)['code'],'TEST_FAILED')
        self.assertFalse(list(self.package.glob('.pending-*')))

    def test_resume_uses_original_journal(self):
        self.prepare('FAIL')
        restored=self.service.resume(self.run,self.uid)
        self.assertEqual(restored['handle'],self.handle)
        self.assertEqual(restored['snapshot'],self.snapshot)
        replace(self.root/'qualification-plan.md',b'new plan')
        with self.assertRaises(c.EvidenceError): self.service.resume(self.run,self.uid)

    def test_storage_identity(self):
        original=c.decode((self.install/'roots.json').read_bytes())
        changed=copy.deepcopy(original); changed['captures']['inode']+=1
        replace(self.install/'roots.json',c.encode(changed))
        with self.assertRaisesRegex(c.EvidenceError,'ROOT_OBJECT_CHANGED'): self.service.load(self.handle,self.uid)

    def test_stage_swap_during_seal(self):
        prepared=self.prepare()
        def cut(point):
            if point=='staged': replace(self.stage/'index.json',c.encode({'new':1}))
        self.service.cut=cut
        with self.assertRaises(c.EvidenceError): self.seal(prepared)
        self.no_commit()

    def test_q8_and_coordinator_real_boundary(self):
        events=[]; self.activate(); env=self.service.load(self.handle,self.uid)['request']
        class Session:
            def __init__(self,req): self.req=req
            def start(self): events.append('start'); return self
            def stop(self): events.append('stop'); return fixture(self.req['request'])
        value=q8_capture.capture_pair(env,lambda:events.append('pair'),_session=Session)
        self.assertEqual(events,['start','pair','stop']); self.assertEqual(value['counts'],COUNTS)
        def rpc(operation,handle,payload=None):
            if operation=='load': return self.service.load(handle,self.uid)
            if operation=='activate': return self.service.activate(handle,self.uid)
            if operation=='record': return self.service.record(handle,self.uid,**payload)
            if operation=='prepare': return self.service.prepare(handle,self.uid)
            if operation=='seal': return self.service.seal(handle,self.uid,payload)
            self.fail(operation)
        with mock.patch.object(a,'STATE',self.state),mock.patch.object(p,'rpc',rpc),mock.patch.object(sys,'_s3_runtime',(self.runtime_id,self.snapshot['runtime']),create=True):
            commit=coordinator.finish(self.handle,True,{'q8':'synthetic'})
            self.assertEqual(commit['code'],'SUCCESS')
            self.assertEqual(coordinator.finish(self.handle,True,{'q8':'synthetic'}),commit)
        source=(HERE.parent/'qualification_campaign'/'q8_host_security.py').read_text()
        self.assertIn('capture_pair(package_request()',source)
        self.assertNotIn('S3_EVIDENCE_PACKAGE',source)

    def test_empty_evidence_set_is_finalized_test_failure(self):
        prepared = self.prepare('FAIL')
        evidence = prepared['files']['evidence.json']
        self.assertEqual(evidence, {'handle': self.handle, 'required': False,
                                   'status': 'FINALIZED', 'receipt': None, 'error': None})
        commit = self.seal(prepared)
        self.assertEqual((commit['test'], commit['evidence'], commit['code']),
                         ('FAIL', 'FINALIZED', 'TEST_FAILED'))

    def test_q8_evidence_activation_is_immutable_and_required(self):
        request = self.activate()
        self.assertEqual(request, self.service.load(self.handle, self.uid)['request'])
        self.assertEqual(self.activate(), request)
        prepared = self.prepare('FAIL')
        self.assertTrue(prepared['files']['evidence.json']['required'])

    def test_required_finalized_evidence_and_test_failure_is_test_failed(self):
        self.activate()
        prepared = self.prepare('FAIL')
        self.assertEqual(prepared['files']['evidence.json']['status'], 'FINALIZED')
        self.assertTrue(prepared['files']['evidence.json']['required'])
        self.assertEqual(self.seal(prepared)['code'], 'TEST_FAILED')

    def test_required_evidence_failure_overrides_test_pass(self):
        class Failed:
            def operate(self, req, **kwargs): return fixture(req, status='FAILED')
        self.activate(); self.service.producer_override = Failed()
        prepared = self.prepare('PASS')
        self.assertEqual(self.seal(prepared)['code'], 'EVIDENCE_FINALIZATION_FAILED')

    def test_required_evidence_and_test_failure_preserves_both(self):
        class Failed:
            def operate(self, req, **kwargs): return fixture(req, status='FAILED')
        self.activate(); self.service.producer_override = Failed()
        prepared = self.prepare('FAIL')
        commit = self.seal(prepared)
        self.assertEqual((commit['test'], commit['evidence'], commit['code']),
                         ('FAIL', 'FAILED', 'EVIDENCE_FINALIZATION_FAILED'))

    def test_capture_without_activation_is_rejected(self):
        envelope = self.service.load(self.handle, self.uid)['request']
        with self.assertRaisesRegex(c.EvidenceError, 'EVIDENCE_NOT_REQUIRED'):
            self.service.capture(envelope, self.uid, capture=True)

    def test_official_pass_cannot_omit_q8_evidence_activation(self):
        run = p.new_id()
        response = self.service.begin(self.runtime_id, run, 'OFFICIAL_FNK0099_V0_4_0',
                                      self.uid, 2)
        handle = response['handle']
        self.service.record(handle, self.uid, 'PASS', {'phases': 'synthetic'})
        with self.assertRaisesRegex(c.EvidenceError, 'OFFICIAL_EVIDENCE_NOT_ACTIVATED'):
            self.service.prepare(handle, self.uid)

    def test_q8_stops_on_pair_failure(self):
        events=[]
        class Session:
            def __init__(self,req): pass
            def start(self): return self
            def stop(self): events.append('stop'); return fixture()
        with self.assertRaisesRegex(RuntimeError,'pair'):
            q8_capture.capture_pair(ENVELOPE,lambda:(_ for _ in ()).throw(RuntimeError('pair')),_session=Session)
        self.assertEqual(events,['stop'])

    def test_q8_policy(self):
        for abnormal in (False,True):
            value=fixture(status='FAILED' if abnormal else 'FINALIZED')
            if not abnormal: value['counts']['security_requests']=1
            class Session:
                def __init__(self,req): pass
                def start(self): return self
                def stop(self): return value
            with self.assertRaises(c.EvidenceError): q8_capture.capture_pair(ENVELOPE,lambda:None,_session=Session)

if __name__=='__main__': unittest.main()
