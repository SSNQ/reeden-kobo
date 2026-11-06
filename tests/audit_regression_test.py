#!/usr/bin/env python3
"""Regression checks for upgrade failures, truthful integrity checks and safe SSH."""
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def manifest(pkg):
    files = sorted(p for p in pkg.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (pkg / 'SHA256SUMS').write_text(''.join(
        f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(pkg).as_posix()}\n' for p in files))


def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#!/bin/sh\n' + text)
    path.chmod(0o755)


for failure in ('copy', 'promote', 'menu', 'space'):
    with tempfile.TemporaryDirectory(prefix='reeden-fault-') as tmp:
        base = Path(tmp); mount = base / 'kobo'; pkg = base / 'pkg'; pkg.mkdir()
        (mount / '.kobo').mkdir(parents=True); (mount / '.adds/nm').mkdir(parents=True)
        (mount / '.kobo/version').write_text('SN,4.9.77,4.46.23836,000-393\n')
        (pkg / 'nickelmenu.conf').write_text('new-menu\n'); (pkg / 'payload').write_text('new')
        manifest(pkg)
        dest = mount / '.adds/reeden-kobo'; (dest / 'userdata').mkdir(parents=True)
        (dest / 'userdata/progress').write_text('old-progress'); (dest / 'payload').write_text('old')
        # The previous install shipped the stock menu, so the live entry is replaceable;
        # a customised or unreadable entry is intentionally kept instead.
        (dest / 'nickelmenu.conf').write_text('old-menu\n')
        entry = mount / '.adds/nm/reeden-kobo'; entry.write_text('old-menu\n')
        real_copy = installer.copytree_fat32_safe; real_rename = Path.rename

        def copying(src, dst):
            if failure == 'copy' and src.name == 'userdata':
                raise OSError(28, 'injected ENOSPC')
            return real_copy(src, dst)

        def renaming(src, dst):
            if failure == 'promote' and src.name.startswith('.reeden-kobo-stage-'):
                raise OSError(5, 'injected promotion I/O failure')
            return real_rename(src, dst)

        patches = [mock.patch.object(installer, 'copytree_fat32_safe', copying),
                   mock.patch.object(Path, 'rename', renaming)]
        if failure == 'menu':
            patches.append(mock.patch.object(installer.os, 'replace', side_effect=OSError(5, 'injected menu failure')))
        if failure == 'space':
            patches.append(mock.patch.object(installer.shutil, 'disk_usage', return_value=type('Disk', (), {'free': 0})()))
        from contextlib import ExitStack, redirect_stdout
        with ExitStack() as stack:
            for patch in patches: stack.enter_context(patch)
            try:
                with redirect_stdout(io.StringIO()):
                    installer.install_to_usb_mount(pkg, mount, True)
                raise AssertionError('Expected injected failure')
            except (OSError, RuntimeError):
                pass
        assert (dest / 'payload').read_text() == 'old', failure
        assert (dest / 'userdata/progress').read_text() == 'old-progress', failure
        assert entry.read_text() == 'old-menu\n', failure
        assert not list((mount / '.adds').glob('.reeden-kobo-stage-*')), failure
        assert not list((mount / '.adds/nm').glob('.reeden-kobo-menu-*')), failure

with tempfile.TemporaryDirectory(prefix='reeden-check-') as tmp:
    app = Path(tmp)
    for d in ('bin', 'runtime', 'checks', 'state', 'core/lib', 'ui-assets/assets/fonts'):
        (app / d).mkdir(parents=True, exist_ok=True)
    for f in ('core/lib/libflutter_engine.so', 'core/lib/libapp.so', 'ui-assets/assets/fonts/KoboCJKMath-Regular.ttf'):
        (app / f).touch()
    for f in ('bin/fb-guard', 'runtime/reeden-kindle'): executable(app / f, 'exit 0\n')
    (app / 'payload').write_text('original'); manifest(app)
    (app / 'state/install-verified').touch()
    (app / 'install-verification.log').write_text('old historical success\n')
    script = (ROOT / 'scripts/precheck.sh').read_text().replace('/mnt/onboard', str(app / 'onboard')).replace('/sys/', str(app / 'sys') + '/')
    (app / 'precheck.sh').write_text(script)
    def check():
        return subprocess.run(['sh', str(app / 'precheck.sh')], capture_output=True, text=True, check=True).stdout
    assert '[OK] Package SHA-256: current files verified' in check()
    (app / 'payload').write_text('corrupted')
    assert '[FAIL] Package SHA-256: current file checksum failed' in check()
    assert '[OK] Package SHA-256' not in check()
    (app / 'SHA256SUMS').unlink()
    assert '[FAIL] Package SHA-256: manifest missing' in check()

with tempfile.TemporaryDirectory(prefix='reeden-ssh-') as tmp:
    app = Path(tmp); server = app / 'diagnostics/ssh/reeden-ssh'
    executable(server, 'printf "%s\\n" "$@" > "' + str(app / 'arguments') + '"\n')
    executable(app / 'ifconfig', 'echo "inet addr:192.0.2.10"\n')
    script = (ROOT / 'scripts/reeden-ssh.sh').read_text().replace('/mnt/onboard/.adds/reeden-kobo', str(app)).replace('/tmp/dropbear_reeden_diag.pid', str(app / 'pid')).replace('/sbin/ifconfig', str(app / 'ifconfig'))
    (app / 'ssh.sh').write_text(script)
    r = subprocess.run(['sh', str(app / 'ssh.sh'), 'start'], capture_output=True, text=True)
    assert r.returncode != 0 and not (app / 'arguments').exists()
    keys = app / 'diagnostics/ssh/settings/SSH/authorized_keys'; keys.parent.mkdir(parents=True)
    keys.write_text('ssh-ed25519 AAAATEST synthetic-fixture\n')
    subprocess.run(['sh', str(app / 'ssh.sh'), 'start'], check=True)
    args = (app / 'arguments').read_text().splitlines()
    assert '-s' in args and '-n' not in args and '192.0.2.10:2223' in args
    (app / 'arguments').unlink()
    subprocess.run(['sh', str(app / 'ssh.sh')], check=True, capture_output=True)
    subprocess.run(['sh', str(app / 'ssh.sh'), 'stop'], check=True, capture_output=True)
    assert not (app / 'arguments').exists(), 'status/stop must not start a server'

# Exercise the early version check before annotations are evaluated.
r = subprocess.run(['python3', '-c', 'import runpy,sys;sys.version_info=(3,9,0);runpy.run_path(sys.argv[1])', str(ROOT / 'install.py')], capture_output=True, text=True)
assert r.returncode != 0 and 'Python 3.10' in r.stderr and 'Traceback' not in r.stderr
print('PASS: USB ENOSPC/promotion/menu rollback and space preflight; current checksums; key-only optional SSH; Python version message')

# NickelMenu cmd_output accepts only 0 < timeout_ms < 10000. Validate the
# single config used by both assemblers against this external interface.
menu = (ROOT / 'scripts/nickelmenu.conf').read_text()
for line in menu.splitlines():
    fields = line.split(':', 4)
    assert len(fields) == 5 and fields[0] == 'menu_item'
    if fields[3] == 'cmd_output':
        timeout, command = fields[4].split(':', 1)
        assert 0 < int(timeout) < 10000, line
        assert command.startswith('/mnt/onboard/.adds/reeden-kobo/')
assert ':Reeden SSH:' not in menu and 'reeden-ssh.sh start' not in menu
print('PASS: NickelMenu timeout limits and no default SSH start action')
