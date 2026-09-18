#!/usr/bin/env python3
"""Only installed, isolated, source-only qualification entrypoint."""
import os
from pathlib import Path
import stat
import sys
import types


def bootstrap(name):
    path = Path('/usr/local/lib/s3-hidbot-authority-v3') / (name + '.py')
    for parent in (path, *path.parents):
        s = parent.lstat()
        if s.st_uid != 0 or s.st_mode & 0o022 or stat.S_ISLNK(s.st_mode):
            raise RuntimeError('INSTALL_AUTHORITY_INVALID')
    if path.stat().st_mode & 0o222 or path.stat().st_nlink != 1:
        raise RuntimeError('INSTALL_AUTHORITY_INVALID')
    module = types.ModuleType(name); module.__file__ = str(path); sys.modules[name] = module
    exec(compile(path.read_bytes(), str(path), 'exec'), module.__dict__)
    return module


def main():
    c = bootstrap('evidence_contract'); a = bootstrap('authority_runtime')
    c.need(len(sys.argv) >= 4 and Path(__file__) == a.INSTALL / 'runtime_launcher.py', 'LAUNCH_INVALID')
    runtime_id, entry, context = sys.argv[1:4]
    root = a.INSTALL / 'runtimes' / runtime_id
    manifest = a.runtime(root, runtime_id)
    c.need(manifest['engine'] == a.engine_identity(), 'ENGINE_AUTHORITY_CONFLICT')
    a.isolate(root, manifest)
    os.environ['PATH'] = '/usr/bin:/bin'
    sys._s3_runtime = (runtime_id, manifest)
    import evidence_pipeline as p
    if context != '-':
        handle = a.handle(c.decode(context)); c.need(handle['runtime_id'] == runtime_id, 'AUTHORITY_CONFLICT')
        p.verify_attempt(handle)
        sys._s3_attempt = handle
    else:
        c.need(entry in ('official_campaign.py', 'evidence_rehearsal.py', 'runtime_probe.py', 'evidence_resume.py'), 'ATTEMPT_REQUIRED')
    c.need(entry in manifest['files'] and '/' not in entry and entry.endswith('.py'), 'ENTRY_INVALID')
    path = root / entry; data = a.source(path)
    c.need(a.sha(data) == manifest['files'][entry], 'EXECUTED_SOURCE_CHANGED')
    sys.argv = [str(path), *sys.argv[4:]]
    exec(compile(data, str(path), 'exec'), {'__name__': '__main__', '__file__': str(path), '__package__': None})


if __name__ == '__main__': main()
