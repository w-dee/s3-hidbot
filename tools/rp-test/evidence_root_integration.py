#!/usr/bin/env python3
"""Explicit root/nobody authority integration. Synthetic writers; no device or install."""
import json
import os
from pathlib import Path
import socket
import sys
import evidence_contract as c
import authority_runtime as a
import evidence_pipeline as p
import privileged_evidence as h
from test_attempt_authority import AuthorityTests, coordinator, q8_capture
from test_evidence_pipeline import WRITER, COUNTS


def main():
    c.need(os.getuid()==0,'ROOT_TEST_ONLY')
    fixture=AuthorityTests(); fixture.setUp(); fixture.base.chmod(0o755)
    writer=fixture.base/'writer.py'; writer.write_text(WRITER)
    parent,child=socket.socketpair(); parent.settimeout(40); child.settimeout(40)
    pid=os.fork()
    if pid==0:
        parent.close(); os.setgroups([]); os.setgid(65534); os.setuid(65534)
        stream=child.makefile('rwb',buffering=0)
        def rpc(operation,handle,payload=None):
            stream.write(c.encode({'operation':operation,'handle':handle,'payload':payload}))
            result=c.decode(stream.readline())
            c.need('error' not in result,'RPC_FAILED'); return result
        p.rpc=rpc; a.STATE=fixture.state; sys._s3_runtime=(fixture.runtime_id,fixture.snapshot['runtime'])
        try:
            codes=[]
            for outcome,mode in ((True,'normal'),(False,'normal'),(True,'bad')):
                handle=p.create_package(a.CLASSIFICATION,max_seconds=2)
                env=p.package_request(handle)
                class Session:
                    def __init__(self,request): self.request=request
                    def start(self):
                        stream.write(c.encode({'operation':'capture-'+mode,'handle':handle,'payload':self.request}))
                        ready=c.decode(stream.readline()); c.need(ready['request']==self.request['request'],'READY_INVALID')
                        return self
                    def stop(self):
                        stream.write(c.encode({'command':'STOP'}))
                        return c.receipt(c.decode(stream.readline()),self.request['request'])
                try: q8_capture.capture_pair(env,lambda:None,_session=Session)
                except c.EvidenceError: c.need(mode=='bad','CAPTURE_FAILED')
                raw=fixture.state/'captures'/handle['run_id']/handle['capture_id']/'raw-hci'
                final=fixture.state/'attempts'/handle['run_id']/'package'/'index.json'
                for operation in (lambda:raw.read_bytes(),lambda:raw.chmod(0o644),
                                  lambda:(fixture.root/'q8_capture.py').write_text('bad'),
                                  lambda:(fixture.root/'injected.pyc').write_bytes(b'bad')):
                    try: operation()
                    except PermissionError: pass
                    else: raise AssertionError('ordinary user changed protected authority')
                commit=coordinator.finish(handle,outcome,{'synthetic':'root-nobody'})
                c.need(coordinator.finish(handle,outcome,{'synthetic':'root-nobody'})==commit,'RETRY_CHANGED')
                for operation in (lambda:final.read_bytes(),lambda:final.chmod(0o644),lambda:final.write_bytes(b'bad')):
                    try: operation()
                    except PermissionError: pass
                    else: raise AssertionError('ordinary user accessed private sealed package')
                codes.append(commit['code'])
            c.need(codes==['SUCCESS','TEST_FAILED','EVIDENCE_FINALIZATION_FAILED'],'OUTCOME_INVALID')
            stream.write(c.encode({'done':codes})); os._exit(0)
        except BaseException:
            import traceback
            traceback.print_exc(); os._exit(1)
    child.close(); stream=parent.makefile('rwb',buffering=0); summary=None
    try:
        while True:
            data=stream.readline()
            if not data: break
            value=c.decode(data)
            if 'done' in value: summary=value; break
            operation=value['operation']; handle=value['handle']; payload=value['payload']; service=fixture.service
            if operation=='begin':
                result=service.begin(payload['runtime_id'],handle['run_id'],payload['classification'],65534,payload['max_seconds'])
            elif operation=='load': result=service.load(handle,65534)
            elif operation.startswith('capture-'):
                req=payload['request']
                service.producer_override=h.Producer(fixture.state/'captures',req['helper'],
                    writer=(sys.executable,str(writer),operation.removeprefix('capture-')),decoder=lambda _:dict(COUNTS))
                result=service.capture(payload,65534,capture=True,ready=lambda ready:stream.write(c.encode(ready)),
                    control=lambda _:c.decode(stream.readline())=={'command':'STOP'})
            elif operation=='record': result=service.record(handle,65534,**payload)
            elif operation=='prepare': result=service.prepare(handle,65534)
            elif operation=='seal': result=service.seal(handle,65534,payload)
            else: raise AssertionError(operation)
            stream.write(c.encode(result))
        _,status=os.waitpid(pid,0); c.need(status==0 and summary is not None,'ROOT_INTEGRATION_FAILED')
        print(json.dumps({'root_raw':True,'ordinary_raw_read_chmod_denied':True,'runtime_write_denied':True,
                          'final_package_read_write_chmod_denied':True,'coordinator_terminal_codes':summary['done']}))
    finally:
        stream.close(); parent.close(); fixture.doCleanups()

if __name__=='__main__': main()
