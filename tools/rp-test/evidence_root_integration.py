#!/usr/bin/env python3
"""Explicit hardware-free root/ordinary-user integration using synthetic writers.

Run with sudo only for this test. It installs nothing, opens no devices and
removes its temporary tree. Production CLI has no synthetic-writer option.
"""
import json
import os
from pathlib import Path
import shutil
import socket
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'qualification_campaign'))
import evidence_contract as c
import evidence_pipeline as p
import privileged_evidence as h
import official_campaign as coordinator
import q8_capture
from test_evidence_pipeline import IDENTITY, COUNTS, WRITER


def main():
    c.need(os.geteuid() == 0, 'ROOT_TEST_ONLY')
    base = Path(tempfile.mkdtemp(prefix='s3-evidence-root-test-')); base.chmod(0o755)
    root = base/'raw'; root.mkdir(mode=0o700)
    packages = base/'packages'; packages.mkdir(mode=0o700); os.chown(packages,65534,65534)
    writer = base/'writer.py'; writer.write_text(WRITER)
    parent, child = socket.socketpair(); parent.settimeout(15); child.settimeout(15)
    pid = os.fork()
    if pid == 0:
        parent.close(); os.setgroups([]); os.setgid(65534); os.setuid(65534)
        stream = child.makefile('rwb', buffering=0)
        def rpc(command, req):
            stream.write(c.encode({'command':command,'request':req}))
            return c.receipt(c.decode(stream.readline()),req)
        try:
            codes=[]
            for test_pass, mode in ((True,'normal'),(False,'normal'),(True,'bad')):
                package=p.create_package(packages,p.CLASSIFICATION,{'helper':IDENTITY})
                req=p.package_request(package)
                class Session:
                    def __init__(self, request): self.req=request
                    def start(self):
                        stream.write(c.encode({'command':mode,'request':self.req}))
                        ready=c.decode(stream.readline())
                        c.need(ready['event']=='READY' and ready['request']==self.req,'READY_INVALID')
                        self.object=ready['object']
                        return self
                    def stop(self):
                        stream.write(c.encode({'command':'STOP'}))
                        receipt=c.receipt(c.decode(stream.readline()),self.req)
                        c.need(receipt['raw']['object']==self.object,'OBJECT_CHANGED')
                        return receipt
                try:
                    q8_capture.capture_pair(req,lambda:None,_session=Session)
                except c.EvidenceError:
                    c.need(mode=='bad','UNEXPECTED_Q8_FAILURE')
                raw=root/req['run_id']/req['capture_id']/'raw-hci'
                for operation in (lambda:raw.read_bytes(),lambda:raw.chmod(0o644)):
                    try: operation()
                    except PermissionError: pass
                    else: raise AssertionError('ordinary user accessed root raw')
                terminal=coordinator.finish(package,test_pass,{'phase':'synthetic-q8'},_verify=lambda r:rpc('verify',r))
                c.need(coordinator.finish(package,test_pass,{'phase':'synthetic-q8'},_verify=lambda r:rpc('verify',r)) == terminal,
                       'REVERIFY_FAILED')
                codes.append(terminal['code'])
            c.need(codes==['SUCCESS','TEST_FAILED','EVIDENCE_FINALIZATION_FAILED'],'TERMINAL_POLICY_INVALID')
            stream.write(c.encode({'done':codes})); os._exit(0)
        except BaseException:
            import traceback
            traceback.print_exc(); os._exit(1)
    child.close(); stream=parent.makefile('rwb',buffering=0)
    try:
        summary=None
        while True:
            data=stream.readline()
            if not data: break
            value=c.decode(data)
            if 'done' in value: summary=value; break
            producer=h.Producer(root,IDENTITY,writer=(sys.executable,str(writer),
                                'bad' if value['command']=='bad' else 'normal'),decoder=lambda _:dict(COUNTS))
            result=producer.operate(value['request'],capture=value['command']!='verify',
                                    ready=lambda ready:stream.write(c.encode(ready)),
                                    control=lambda _:c.decode(stream.readline())=={'command':'STOP'})
            c.receipt(result,value['request'])
            c.need(result['raw']['uid']==0 and result['raw']['gid']==0,'ROOT_RAW_REQUIRED')
            stream.write(c.encode(result))
        _, status=os.waitpid(pid,0)
        c.need(status==0 and summary is not None,'ROOT_INTEGRATION_FAILED')
        print(json.dumps({'root_raw':True,'ordinary_user_read_denied':True,'ordinary_user_chmod_denied':True,
                          'coordinator_terminal_codes':summary['done']}))
    finally:
        stream.close(); parent.close(); shutil.rmtree(base)


if __name__=='__main__': main()
