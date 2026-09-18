#!/usr/bin/env python3
"""Explicit administrator preparation only. Fixed destinations; no attempt is started."""
import argparse
import os
from pathlib import Path
import shutil
import stat
import evidence_contract as c
import authority_runtime as a


def ancestor(path):
    for p in (path,*path.parents):
        s=p.lstat()
        c.need(s.st_uid == 0 and not s.st_mode & 0o022 and not stat.S_ISLNK(s.st_mode), 'INSTALL_PARENT_INVALID')


def install(bundle):
    c.need(os.getuid() == 0, 'ROOT_REQUIRED')
    manifest=c.decode((bundle/'BUNDLE.json').read_bytes()); identity=c.digest(manifest)
    c.need(a.membership(bundle) == manifest['files'], 'INPUT_CHANGED')
    ancestor(a.INSTALL.parent); ancestor(a.STATE.parent)
    a.INSTALL.mkdir(mode=0o755,exist_ok=True); ancestor(a.INSTALL)
    for name in a.ENGINE:
        data=(bundle/name).read_bytes(); c.need(a.sha(data) == manifest['engine'][name], 'ENGINE_INPUT_CHANGED')
        target=a.INSTALL/name
        if target.exists(): c.need(a.source(target) == data, 'INSTALLED_ENGINE_CONFLICT')
        else:
            with target.open('xb') as stream:
                stream.write(data); stream.flush(); os.fchmod(stream.fileno(),0o444); os.fsync(stream.fileno())
    runtimes=a.INSTALL/'runtimes'; runtimes.mkdir(mode=0o755,exist_ok=True); ancestor(runtimes)
    target=runtimes/identity
    c.need(not target.exists(), 'RUNTIME_ALREADY_INSTALLED')
    # Only enumerated bytes copied, never links or cached executable artifacts.
    target.mkdir(mode=0o700)
    for name,digest in {**manifest['files'],'BUNDLE.json':identity}.items():
        relative=Path(name)
        c.need(not relative.is_absolute() and '..' not in relative.parts, 'MEMBER_INVALID')
        data=(bundle/relative).read_bytes(); c.need(a.sha(data) == digest, 'INPUT_CHANGED')
        out=target/relative; out.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
        with out.open('xb') as stream:
            stream.write(data); stream.flush(); os.fchmod(stream.fileno(),0o444); os.fsync(stream.fileno())
    for directory,_,_ in os.walk(target,topdown=False):
        os.chmod(directory,0o555)
        with c.Directory(Path(directory)) as d: os.fsync(d.fd)
    a.runtime(target,identity); c.need(a.engine_identity() == manifest['engine'], 'ENGINE_AUTHORITY_CONFLICT')
    a.STATE.mkdir(mode=0o755,exist_ok=True); ancestor(a.STATE)
    for name,mode in (('attempts',0o700),('captures',0o700),('staging',0o755)):
        p=a.STATE/name; p.mkdir(mode=mode,exist_ok=True); ancestor(p)
        c.need(stat.S_IMODE(p.stat().st_mode) == mode, 'STATE_MODE_INVALID')
    with c.Directory(a.INSTALL) as d:
        c.write_once(d,'roots.json',{name:c.object_identity(path.stat()) for name,path in
            (('state',a.STATE),('captures',a.STATE/'captures'))})
        os.fsync(d.fd)
    with c.Directory(runtimes) as d: os.fsync(d.fd)
    with c.Directory(a.STATE) as d: os.fsync(d.fd)
    print(c.encode({'runtime_id':identity,'files':len(manifest['files']),'engine':manifest['engine']}).decode(),end='')

if __name__ == '__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--bundle',type=Path,required=True)
    install(parser.parse_args().bundle)
