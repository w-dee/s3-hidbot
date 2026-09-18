#!/usr/bin/env python3
"""One-time owner-authorized installation; never invoked by a qualification runner."""
import argparse
import hashlib
import os
from pathlib import Path
import sys

import evidence_contract as c
from privileged_evidence import INSTALL, EVIDENCE_ROOT


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--frozen-bundle',required=True,type=Path)
    args=parser.parse_args()
    c.need(os.geteuid()==0,'ROOT_INSTALL_ONLY')
    authority=c.decode((args.frozen_bundle/'FROZEN.json').read_bytes())
    identity=c.identity(authority['helper'])
    sources={name:(args.frozen_bundle/name).read_bytes() for name in ('privileged_evidence.py','evidence_contract.py')}
    c.need(hashlib.sha256(sources['privileged_evidence.py']).hexdigest()==identity['helper_sha256']
           and hashlib.sha256(sources['evidence_contract.py']).hexdigest()==identity['imports']['evidence_contract.py'],
           'INSTALL_DIGEST_MISMATCH')
    # Existing installs are immutable. A changed version requires a deliberate owner installation step.
    INSTALL.mkdir(mode=0o755,exist_ok=True)
    with c.Directory(INSTALL) as directory:
        for fd in directory.fds:
            s=os.fstat(fd); c.need(s.st_uid==0 and not s.st_mode & 0o022,'INSTALL_PARENT_INVALID')
        for name,data in sources.items():
            try:
                fd=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o400,dir_fd=directory.fd)
            except FileExistsError:
                c.need(c.read_bytes(directory,name)==data,'INSTALL_ALREADY_EXISTS_DIFFERENT')
            else:
                with os.fdopen(fd,'wb') as stream:
                    stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.fsync(directory.fd)
    with c.Directory(EVIDENCE_ROOT.parent) as directory:
        directory.child(EVIDENCE_ROOT.name,create=True)
        s=os.fstat(directory.fd)
        c.need(s.st_uid==0 and s.st_gid==0 and s.st_mode & 0o777==0o700,'ROOT_AUTHORITY_INVALID')
        root_object=c.object_identity(s)
    with c.Directory(INSTALL) as directory:
        c.write_once(directory,'root.json',root_object)
    print(c.encode({'installed':identity,'root_object':root_object}).decode(),end='')


if __name__=='__main__': main()
