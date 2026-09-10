"""Explicit cold preparation; never imported by warm preflight for installs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import subprocess
import sys

from common import ROOT, MARKER, MARKER_VALUE, LAYOUT, InfraError, atomic_json, cli_error, need, no_links, private_dir, sha, verify_marker

PACKAGES = ('python3', 'python3-venv', 'python3-pip', 'rsync', 'openssh-client', 'git', 'coreutils', 'usbutils', 'udev', 'bluez', 'util-linux')
SOURCE = Path(__file__).resolve().parent

def host_facts():
    release = platform.freedesktop_os_release()
    model = Path('/proc/device-tree/model')
    return {'os': release.get('ID'), 'os_version': release.get('VERSION_ID'), 'arch': platform.machine(),
            'pi4': model.is_file() and model.read_bytes().startswith(b'Raspberry Pi 4')}

def compatible(facts):
    return facts['os'] in ('debian', 'raspbian') and facts['os_version'] in ('12', '13') and facts['arch'] == 'aarch64' and facts['pi4'] is True

def package_versions():
    out = {}
    for name in PACKAGES:
        cp = subprocess.run(['dpkg-query', '-W', '-f=${db:Status-Status} ${Version}', name], capture_output=True, text=True)
        value = cp.stdout.strip()
        out[name] = value[10:] if cp.returncode == 0 and value.startswith('installed ') else None
    return out

def tooling_digest():
    h = hashlib.sha256()
    for p in sorted(SOURCE.iterdir()):
        if p.suffix in ('.py', '.sh', '.lock', '.json'):
            h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()

def runtime(root=ROOT):
    python = root / 'toolchains' / 'qualification' / 'bin' / 'python'
    cp = subprocess.run([str(python), '-I', '-c',
        'import sys,json,importlib.metadata as m; import esptool,serial; '
        'print(json.dumps({"python":sys.version.split()[0],"esptool":esptool.__version__,"dependencies":sorted((d.metadata["Name"].lower(),d.version) for d in m.distributions())}))'],
        capture_output=True, text=True, timeout=15)
    need(cp.returncode == 0, 'TOOLCHAIN_CACHE_STALE')
    return json.loads(cp.stdout)

def desired_dependencies():
    return sorted(tuple(line.strip().lower().split('==')) for line in (SOURCE / 'requirements.lock').read_text().splitlines() if line.strip())

def runtime_matches(value):
    actual = dict(value['dependencies'])
    return value['esptool'] == '4.12.0' and all(actual.get(name) == version for name, version in desired_dependencies())

def stamp_value(root=ROOT):
    h = hashlib.sha256()
    venv = root / 'toolchains/qualification'
    for path in sorted(venv.rglob('*')):
        # Python's bytecode cache is reproducible and may be lazily generated.
        if '__pycache__' in path.parts or path.suffix == '.pyc':
            continue
        s = path.lstat()
        h.update(json.dumps([str(path.relative_to(venv)), s.st_size, s.st_mtime_ns, s.st_ctime_ns,
                             s.st_mode, s.st_ino, s.st_uid, s.st_gid]).encode())
    return {'schema': 1, 'host': host_facts(), 'packages': package_versions(), 'runtime': runtime(root),
            'requirements_sha256': sha(SOURCE / 'requirements.lock'), 'bootstrap_sha256': tooling_digest(),
            'venv_fingerprint': h.hexdigest()}

def run_quiet(command, code, env=None):
    cp = subprocess.run(command, capture_output=True, env=env)
    need(cp.returncode == 0, code)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--initialize-host', action='store_true')
    args = parser.parse_args()
    need(os.geteuid() == 0, 'BOOTSTRAP_REQUIRES_SUDO')
    need(compatible(host_facts()), 'HOST_INCOMPATIBLE')
    # Only a sudo caller with a normal dedicated account may own the appliance.
    uid = int(os.environ.get('SUDO_UID', '0'))
    need(uid >= 1000, 'DEDICATED_OWNER_REQUIRED')
    owner = pwd.getpwuid(uid)
    if MARKER.exists():
        verify_marker()
    else:
        need(args.initialize_host, 'HOST_INITIALIZATION_REQUIRED')
        no_links(MARKER)
        fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
        with os.fdopen(fd, 'w') as stream:
            json.dump(MARKER_VALUE, stream, sort_keys=True)
            stream.flush()
            os.fsync(stream.fileno())
        verify_marker()
    # Marker contains only public appliance facts. umask 077 must not make the
    # root-owned marker unreadable to the unprivileged doctor.
    os.chmod(MARKER, 0o644)
    for sub in ('', *LAYOUT):
        path = private_dir(ROOT / sub)
        os.chown(path, uid, owner.pw_gid)
    missing = [name for name, version in package_versions().items() if version is None]
    if missing:
        environment = dict(os.environ, DEBIAN_FRONTEND= 'noninteractive')
        run_quiet(['apt-get', 'update'], 'APT_INDEX_FAILED', environment)
        run_quiet(['apt-get', 'install', '-y', '--no-install-recommends', *missing], 'APT_INSTALL_FAILED', environment)
    venv = ROOT / 'toolchains' / 'qualification'
    no_links(venv)
    install = True
    if (venv / 'bin/python').exists():
        try:
            install = not runtime_matches(runtime())
        except (InfraError, OSError, ValueError):
            pass
    if install:
        # venv creation/update only; no recursive removal or package uninstall.
        run_quiet([sys.executable, '-m', 'venv', str(venv)], 'VENV_CREATE_FAILED')
        run_quiet([str(venv / 'bin/python'), '-m', 'pip', '--disable-pip-version-check', 'install', '--no-cache-dir', '-r', str(SOURCE / 'requirements.lock')], 'PIP_INSTALL_FAILED')
        for path in [venv, *venv.rglob('*')]:
            os.chown(path, uid, owner.pw_gid, follow_symlinks=False)
    need(runtime_matches(runtime()), 'TOOLCHAIN_CACHE_STALE')
    run_quiet([str(venv / 'bin/python'), '-m', 'pip', '--disable-pip-version-check', 'check'], 'DEPENDENCIES_INCONSISTENT')
    stamp = stamp_value()
    stamp_path = ROOT / 'state/toolchain.json'
    from common import read_json
    changed = not stamp_path.exists() or read_json(stamp_path) != stamp
    if changed:
        atomic_json(stamp_path, stamp)
        os.chown(stamp_path, uid, owner.pw_gid)
    print(json.dumps({'schema': 1, 'ok': True, 'packages_installed': len(missing), 'toolchain_installed': install,
                      'stamp_changed': changed, 'versions': stamp['runtime'], 'package_versions': stamp['packages']}))
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        sys.exit(cli_error(exc))
