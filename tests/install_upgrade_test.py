#!/usr/bin/env python3
"""Exercise the on-device install.sh (fresh, refusal, upgrade, rollback) with a
mocked /mnt/onboard tree. No hardware, network, or Reeden binaries involved."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SCRIPT = (ROOT / 'scripts' / 'install.sh').read_text(encoding='utf-8')


def build_stage(base: Path, stamp: str, marker: str) -> Path:
    stage = base / 'mnt' / 'onboard' / '.adds' / f'.reeden-kobo-stage-{stamp}'
    for d in ('bin', 'runtime', 'checks', 'state'):
        (stage / d).mkdir(parents=True)
    (stage / 'nickelmenu.conf').write_text('menu_item:main:Test:cmd_spawn:quiet:exec /bin/true\n', encoding='utf-8')
    (stage / f'payload-{marker}.txt').write_text(f'{marker}\n', encoding='utf-8')
    (stage / 'bin/fb-guard').write_text('#!/bin/sh\n[ "$1" = --inspect ] && exit 0\nexit 1\n', encoding='utf-8')
    (stage / 'bin/lipc-get-prop').write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    (stage / 'bin/lipc-set-prop').write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    (stage / 'runtime/loader').write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    (stage / 'checks/probe').write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    (stage / 'SHA256SUMS').write_text(
        ''.join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(stage).as_posix()}\n" for p in files),
        encoding='utf-8',
    )
    return stage


with tempfile.TemporaryDirectory(prefix='reeden-install-sh-test-') as tmp:
    base = Path(tmp).resolve()  # resolve() matches what `pwd` reports (macOS /var -> /private/var)
    onboard = base / 'mnt' / 'onboard'
    (onboard / '.adds' / 'nm').mkdir(parents=True)
    (onboard / '.kobo').mkdir()
    (onboard / '.kobo' / 'version').write_text(
        'N367,4.9.77,4.46.23836,4.9.77,4.46.23836,0000000000393\n', encoding='utf-8')

    # Path substitution mirrors how power/native tests sandbox hardware paths.
    script = SOURCE_SCRIPT.replace('/mnt/onboard', str(onboard))
    dest = onboard / '.adds' / 'reeden-kobo'
    entry = onboard / '.adds' / 'nm' / 'reeden-kobo'

    stubs = base / 'stubs'
    stubs.mkdir()
    for command in ('cp', 'mv'):
        wrapper = stubs / command
        wrapper.write_text('''#!/bin/sh
case "$FAULT:$0" in
  copy:*/cp) case "$*" in */userdata/.*) exit 1;; esac;;
  promote:*/mv) [ "$1" != "$TEST_STAGE" ] || exit 1;;
  menu:*/mv) [ "$1" != -f ] || exit 1;;
esac
exec /bin/COMMAND "$@"
'''.replace('COMMAND', command))
        wrapper.chmod(0o755)

    def run(stage: Path, *args, fault=''):
        staged = stage / 'install.sh'
        staged.write_text(script, encoding='utf-8')
        staged.chmod(0o755)
        return subprocess.run(['sh', str(staged), *args], capture_output=True, text=True,
                              env={**os.environ, 'PATH': str(stubs) + ':' + os.environ['PATH'],
                                   'FAULT': fault, 'TEST_STAGE': str(stage)})

    # 1. Fresh install
    stage1 = build_stage(base, '20260101-000001', 'v1')
    r = run(stage1)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert (dest / 'payload-v1.txt').is_file() and entry.is_file()
    assert not stage1.exists(), 'stage dir should have been moved away'

    # 2. Refuse to overwrite without --upgrade
    stage2 = build_stage(base, '20260101-000002', 'v2')
    r = run(stage2)
    assert r.returncode == 1 and 'Existing installation preserved' in r.stdout + r.stderr
    assert (dest / 'payload-v1.txt').is_file(), 'existing install must be untouched'

    # 3. Refuse while a Reeden session is active
    (dest / 'state' / 'active').mkdir()
    stage2b = build_stage(base, '20260101-000003', 'v2b')
    r = run(stage2b, '--upgrade')
    assert r.returncode == 1 and 'currently active' in r.stdout + r.stderr
    (dest / 'state' / 'active').rmdir()

    # 4. Upgrade preserves userdata/storage/diagnostics/ssh and leaves a backup
    (dest / 'userdata').mkdir()
    (dest / 'userdata' / 'kobo-light.conf').write_text('warmth=40\n', encoding='utf-8')
    (dest / 'storage' / 'books').mkdir(parents=True)
    (dest / 'storage' / 'books' / 'novel.epub').write_text('book-bytes\n', encoding='utf-8')
    (dest / 'diagnostics' / 'ssh').mkdir(parents=True)
    (dest / 'diagnostics' / 'ssh' / 'reeden-ssh').write_text('dropbear-bin\n', encoding='utf-8')
    stage3 = build_stage(base, '20260101-000004', 'v3')
    r = run(stage3, '--upgrade')
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert (dest / 'payload-v3.txt').is_file()
    assert (dest / 'userdata' / 'kobo-light.conf').read_text(encoding='utf-8') == 'warmth=40\n'
    assert (dest / 'storage' / 'books' / 'novel.epub').read_text(encoding='utf-8') == 'book-bytes\n'
    assert (dest / 'diagnostics' / 'ssh' / 'reeden-ssh').read_text(encoding='utf-8') == 'dropbear-bin\n'
    backups = sorted((onboard / '.adds').glob('reeden-kobo-backup-*'))
    assert len(backups) == 1 and (backups[0] / 'payload-v1.txt').is_file()
    assert entry.is_file(), 'menu entry must survive upgrade'

    # 5. Corrupted stage refuses to install (checksum guard)
    stage4 = build_stage(base, '20260101-000005', 'v4')
    (stage4 / 'payload-v4.txt').write_text('tampered\n', encoding='utf-8')
    r = run(stage4, '--upgrade')
    assert r.returncode == 1 and 'Checksum failed' in r.stdout + r.stderr
    assert (dest / 'payload-v3.txt').is_file(), 'install must be untouched after checksum failure'

    # Failures before and during commit must preserve both data and menu.
    old_menu = entry.read_bytes()
    for index, fault in enumerate(('copy', 'promote', 'menu')):
        stage = build_stage(base, f'20260101-00001{index}', fault)
        r = run(stage, '--upgrade', fault=fault)
        assert r.returncode != 0, (fault, r.stdout, r.stderr)
        assert (dest / 'payload-v3.txt').is_file(), fault
        assert (dest / 'userdata/kobo-light.conf').read_text() == 'warmth=40\n', fault
        assert entry.read_bytes() == old_menu, fault
        assert not list(entry.parent.glob('.reeden-kobo-menu-*')), fault

    entry.write_bytes(b'my extra commands\n')
    stage = build_stage(base, 'custom-menu', 'v5')
    r = run(stage, '--upgrade')
    assert r.returncode == 0 and entry.read_bytes() == b'my extra commands\n', (r.stdout, r.stderr)
    assert (dest / 'nickelmenu.conf').read_bytes() == old_menu

print('PASS: install.sh fresh install, overwrite refusal, active-session guard, '
      'data preservation, checksum refusal, and copy/promotion/menu rollback')
