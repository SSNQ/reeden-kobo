#!/usr/bin/env python3
"""Regression tests for Kobo Nickel ExcludeSyncFolders configuration in install.py and install.sh."""
import hashlib
import importlib.util
import os
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('install_mod', ROOT / 'install.py')
install_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_mod)

EXPECTED_RULE = r'(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)'
SOURCE_INSTALL_SH = (ROOT / 'scripts' / 'install.sh').read_text(encoding='utf-8')


def unescape_and_compile(conf_text: str) -> re.Pattern[str]:
    matches = re.findall(r'(?m)^\s*ExcludeSyncFolders\s*=(.*)$', conf_text)
    assert len(matches) == 1, f'Expected 1 ExcludeSyncFolders line, got {matches!r}'
    raw = matches[0].strip()
    unescaped = install_mod._unescape_qsettings_value(raw)
    return re.compile(unescaped)


def test_text_transforms() -> None:
    # 1. Empty file -> adds [FeatureSettings] and ExcludeSyncFolders
    out, status = install_mod.update_kobo_exclude_sync_folders_text('')
    assert status == 'added'
    assert out == f'[FeatureSettings]\nExcludeSyncFolders={EXPECTED_RULE}\n'
    rx = unescape_and_compile(out)
    assert rx.search('.adds') and rx.search('.adds/reeden-kobo') and rx.search('Books/.hidden')
    assert not rx.search('.kobo') and not rx.search('.adobe-digital-editions') and not rx.search('Books')

    # 2. Existing sections without [FeatureSettings] -> appends cleanly without altering prior keys
    orig = '[ApplicationPreferences]\nCurrentLocale=zh_CN\nFrontLightLevel=12\n'
    out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
    assert status == 'added'
    assert out.startswith(orig + '\n[FeatureSettings]\n')
    assert f'ExcludeSyncFolders={EXPECTED_RULE}\n' in out

    # 3. Existing [FeatureSettings] without ExcludeSyncFolders -> inserts inside existing section, no duplicate header
    orig = (
        '[ApplicationPreferences]\nCurrentLocale=zh_CN\n\n'
        '[FeatureSettings]\nExportHighlightsEnabled=true\n\n'
        '[PowerOptions]\nAutoOffMinutes=30\n'
    )
    out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
    assert status == 'added'
    assert out.count('[FeatureSettings]') == 1
    assert f'ExportHighlightsEnabled=true\nExcludeSyncFolders={EXPECTED_RULE}\n\n[PowerOptions]\n' in out

    # 4. Empty ExcludeSyncFolders= -> fills rule
    orig = '[FeatureSettings]\nExcludeSyncFolders=\nExportHighlightsEnabled=true\n'
    out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
    assert status == 'updated'
    assert out == f'[FeatureSettings]\nExcludeSyncFolders={EXPECTED_RULE}\nExportHighlightsEnabled=true\n'

    # 5. Already configured by KOReader (new & legacy syntax) -> untouched byte-for-byte
    for existing in (
        EXPECTED_RULE,
        r'\\.(?!kobo|adobe).*?',
        r'((KOReader)|\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)',
    ):
        orig = f'[FeatureSettings]\nExcludeSyncFolders={existing}\n'
        out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
        assert status == 'already_configured', existing
        assert out == orig

    # 6. Custom non-empty rule (e.g. manga) -> safely merges without losing custom exclusion
    orig = '[FeatureSettings]\nExcludeSyncFolders=(calibre|manga)\n'
    out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
    assert status == 'updated'
    rx = unescape_and_compile(out)
    assert rx.search('calibre') and rx.search('manga')
    assert rx.search('.adds') and rx.search('.adds/reeden-kobo')
    assert not rx.search('.kobo') and not rx.search('.adobe-digital-editions') and not rx.search('Books')

    # 7. Quoted or invalid regex -> manual_required, untouched
    for bad in ('"manga"', '[unclosed'):
        orig = f'[FeatureSettings]\nExcludeSyncFolders={bad}\n'
        out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
        assert status == 'manual_required', bad
        assert out == orig

    # 8. CRLF input normalized to LF when modified
    orig = '[FeatureSettings]\r\nExportHighlightsEnabled=true\r\n'
    out, status = install_mod.update_kobo_exclude_sync_folders_text(orig)
    assert status == 'added'
    assert '\r' not in out
    assert f'ExcludeSyncFolders={EXPECTED_RULE}\n' in out


def test_filesystem_and_usb_guards() -> None:
    with tempfile.TemporaryDirectory(prefix='reeden-exclude-fs-') as tmp:
        base = pathlib.Path(tmp)
        mount = base / 'KOBOeReader'
        conf_dir = mount / '.kobo' / 'Kobo'
        conf_dir.mkdir(parents=True)
        conf_path = conf_dir / 'Kobo eReader.conf'

        # Preserve arbitrary non-UTF-8 bytes in Qt @ByteArray(...) entries
        raw_orig = b'[ApplicationPreferences]\nBlob=@ByteArray(\x80\xffabc)\n\n[FeatureSettings]\nExportHighlightsEnabled=true\n'
        conf_path.write_bytes(raw_orig)
        # Simulate macOS AppleDouble file next to conf
        ad_file = conf_dir / '._Kobo eReader.conf'
        ad_file.write_bytes(b'\x00\x05\x16\x07appledouble')

        status = install_mod.ensure_kobo_exclude_sync_folders(mount)
        assert status == 'added'
        result_bytes = conf_path.read_bytes()
        assert b'Blob=@ByteArray(\x80\xffabc)\n' in result_bytes
        assert f'ExcludeSyncFolders={EXPECTED_RULE}\n'.encode('ascii') in result_bytes
        assert not ad_file.exists(), 'AppleDouble file next to Kobo eReader.conf must be cleaned'

        # Second call is idempotent
        status2 = install_mod.ensure_kobo_exclude_sync_folders(mount)
        assert status2 == 'already_configured'
        assert conf_path.read_bytes() == result_bytes

        # Symlink guard refuses before touching anything during USB install
        if os.name != 'nt':
            sym_mount = base / 'SymMount'
            (sym_mount / '.kobo' / 'Kobo').mkdir(parents=True)
            (sym_mount / '.adds' / 'nm').mkdir(parents=True)
            (sym_mount / '.kobo' / 'version').write_text(
                'SN,4.9.77,4.46.23836,4.9.77,4.46.23836,000-393\n', encoding='utf-8'
            )
            outside = base / 'outside.conf'
            outside.write_text('[General]\nsecret=1\n', encoding='utf-8')
            (sym_mount / '.kobo' / 'Kobo' / 'Kobo eReader.conf').symlink_to(outside)

            pkg = base / 'pkg'
            pkg.mkdir()
            (pkg / 'nickelmenu.conf').write_text('menu_item:main:Test:cmd_spawn:quiet:exec /bin/true\n', encoding='utf-8')
            files = sorted(p for p in pkg.rglob('*') if p.is_file())
            (pkg / 'SHA256SUMS').write_text(
                ''.join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(pkg).as_posix()}\n" for p in files),
                encoding='utf-8',
            )
            try:
                install_mod.install_to_usb_mount(pkg, sym_mount, allow_upgrade=False)
                raise AssertionError('Expected symlink refusal on Kobo eReader.conf')
            except RuntimeError as exc:
                assert '符号链接' in str(exc)
            assert not (sym_mount / '.adds' / 'reeden-kobo').exists()
            assert outside.read_text(encoding='utf-8') == '[General]\nsecret=1\n'


def test_install_sh_exclude_sync() -> None:
    if os.name == 'nt':
        return
    with tempfile.TemporaryDirectory(prefix='reeden-exclude-sh-') as tmp:
        base = pathlib.Path(tmp).resolve()
        onboard = base / 'mnt' / 'onboard'
        (onboard / '.adds' / 'nm').mkdir(parents=True)
        (onboard / '.kobo' / 'Kobo').mkdir(parents=True)
        (onboard / '.kobo' / 'version').write_text(
            'N367,4.9.77,4.46.23836,4.9.77,4.46.23836,0000000000393\n', encoding='utf-8'
        )
        conf = onboard / '.kobo' / 'Kobo' / 'Kobo eReader.conf'
        script = SOURCE_INSTALL_SH.replace('/mnt/onboard', str(onboard))

        def run_case(stamp: str, initial_conf: str | None, upgrade: bool) -> subprocess.CompletedProcess[str]:
            if initial_conf is None:
                conf.unlink(missing_ok=True)
            else:
                conf.write_text(initial_conf, encoding='utf-8')
            stage = onboard / '.adds' / f'.reeden-kobo-stage-{stamp}'
            for d in ('bin', 'runtime', 'checks', 'state'):
                (stage / d).mkdir(parents=True)
            (stage / 'nickelmenu.conf').write_text('menu_item:main:Test:cmd_spawn:quiet:exec /bin/true\n', encoding='utf-8')
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
            staged = stage / 'install.sh'
            staged.write_text(script, encoding='utf-8')
            staged.chmod(0o755)
            args = ['sh', str(staged)] + (['--upgrade'] if upgrade else [])
            return subprocess.run(args, capture_output=True, text=True)

        # Case 1: existing [FeatureSettings] without ExcludeSyncFolders
        r1 = run_case(
            '20260101-000001',
            '[ApplicationPreferences]\nCurrentLocale=zh_CN\n\n[FeatureSettings]\nExportHighlightsEnabled=true\n\n[PowerOptions]\nAutoOffMinutes=15\n',
            upgrade=False,
        )
        assert r1.returncode == 0, (r1.returncode, r1.stdout, r1.stderr)
        c1 = conf.read_text(encoding='utf-8')
        assert c1.count('[FeatureSettings]') == 1
        assert f'ExcludeSyncFolders={EXPECTED_RULE}\n' in c1
        assert 'ExportHighlightsEnabled=true\n' in c1 and 'AutoOffMinutes=15\n' in c1

        # Case 2: already configured -> unchanged
        kor = f'[FeatureSettings]\nExcludeSyncFolders={EXPECTED_RULE}\n'
        r2 = run_case('20260101-000002', kor, upgrade=True)
        assert r2.returncode == 0, (r2.returncode, r2.stdout, r2.stderr)
        assert conf.read_text(encoding='utf-8') == kor

        # Case 3: custom rule -> merged
        r3 = run_case('20260101-000003', '[FeatureSettings]\nExcludeSyncFolders=manga\n', upgrade=True)
        assert r3.returncode == 0, (r3.returncode, r3.stdout, r3.stderr)
        rx3 = unescape_and_compile(conf.read_text(encoding='utf-8'))
        assert rx3.search('manga') and rx3.search('.adds') and not rx3.search('.kobo')

        # Case 4: missing conf file -> created
        r4 = run_case('20260101-000004', None, upgrade=True)
        assert r4.returncode == 0, (r4.returncode, r4.stdout, r4.stderr)
        assert conf.read_text(encoding='utf-8') == f'[FeatureSettings]\nExcludeSyncFolders={EXPECTED_RULE}\n'


test_text_transforms()
test_filesystem_and_usb_guards()
test_install_sh_exclude_sync()
print('PASS: Kobo ExcludeSyncFolders configuration, preservation, merge, symlink guard, and install.sh coverage')
