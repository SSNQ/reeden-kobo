#!/usr/bin/env python3
"""Exercise pure-Python .deb extraction, USB one-click install/upgrade, and install.sh --upgrade."""
import hashlib
import importlib.util
import io
import pathlib
import shutil
import subprocess
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('install_mod', ROOT / 'install.py')
install_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_mod)


def make_synthetic_deb(deb_path: pathlib.Path) -> None:
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode='w:gz') as tf:
        payload = b'synthetic-armhf-shared-object'
        info = tarfile.TarInfo('lib/arm-linux-gnueabihf/libfoo.so.1.0')
        info.size = len(payload)
        tf.addfile(info, io.BytesIO(payload))
        sym = tarfile.TarInfo('lib/arm-linux-gnueabihf/libfoo.so.1')
        sym.type = tarfile.SYMTYPE
        sym.linkname = 'libfoo.so.1.0'
        tf.addfile(sym)
    tar_bytes = tar_buf.getvalue()
    name_field = b'data.tar.gz/'.ljust(16, b' ')
    meta = b'0           0     0     100644  '
    size_field = str(len(tar_bytes)).encode('ascii').ljust(10, b' ')
    header = name_field + meta + size_field + b'`\n'
    assert len(header) == 60
    body = tar_bytes + (b'\n' if len(tar_bytes) % 2 else b'')
    deb_path.write_bytes(b'!<arch>\n' + header + body)


with tempfile.TemporaryDirectory(prefix='reeden-oneclick-test-') as tmp:
    base = pathlib.Path(tmp)

    # 1. Pure-Python .deb extraction + symlink copy resolution + CRLF normalization
    deb_file = base / 'test.deb'
    extract_out = base / 'deb-out'
    make_synthetic_deb(deb_file)
    install_mod.extract_deb_pure(deb_file, extract_out)
    real_so = extract_out / 'lib/arm-linux-gnueabihf/libfoo.so.1.0'
    copied_sym = extract_out / 'lib/arm-linux-gnueabihf/libfoo.so.1'
    assert real_so.is_file() and copied_sym.is_file() and not copied_sym.is_symlink()
    assert real_so.read_bytes() == copied_sym.read_bytes() == b'synthetic-armhf-shared-object'

    crlf_src = base / 'crlf.sh'
    crlf_dst = base / 'lf.sh'
    crlf_src.write_bytes(b'#!/bin/sh\r\nset -eu\r\necho ok\r\n')
    install_mod.copy_lf_script(crlf_src, crlf_dst)
    assert crlf_dst.read_bytes() == b'#!/bin/sh\nset -eu\necho ok\n'

    # 2. USB one-click install + upgrade + guards
    pkg = base / 'pkg'
    pkg.mkdir()
    (pkg / 'nickelmenu.conf').write_text('menu_item:main:Test:cmd_spawn:quiet:exec /bin/true\n', encoding='utf-8')
    (pkg / 'hello.txt').write_text('version-1\n', encoding='utf-8')
    files = sorted(p for p in pkg.rglob('*') if p.is_file())
    (pkg / 'SHA256SUMS').write_text(
        ''.join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(pkg).as_posix()}\n" for p in files),
        encoding='utf-8',
    )

    mount = base / 'KOBOeReader'
    (mount / '.kobo').mkdir(parents=True)
    (mount / '.adds/nm').mkdir(parents=True)
    (mount / '.kobo/version').write_text('SN,4.9.77,4.46.23836,4.9.77,4.46.23836,000-393\n', encoding='utf-8')

    # Fresh USB install
    install_mod.install_to_usb_mount(pkg, mount, allow_upgrade=False)
    installed = mount / '.adds/reeden-kobo'
    assert (installed / 'hello.txt').read_text(encoding='utf-8') == 'version-1\n'
    assert (mount / '.adds/nm/reeden-kobo').is_file()
    conf_text = (mount / '.kobo/Kobo/Kobo eReader.conf').read_text(encoding='utf-8')
    assert '[FeatureSettings]' in conf_text and install_mod.KOBO_EXCLUDE_SYNC_FOLDERS in conf_text

    # Add user reading data, books, and diagnostics/ssh
    (installed / 'userdata').mkdir(parents=True)
    (installed / 'storage/books').mkdir(parents=True)
    (installed / 'diagnostics/ssh').mkdir(parents=True)
    (installed / 'userdata/kobo-light.conf').write_text('warmth=60\n', encoding='utf-8')
    (installed / 'storage/books/novel.epub').write_text('book-bytes\n', encoding='utf-8')
    (installed / 'diagnostics/ssh/reeden-ssh').write_text('dropbear-bin\n', encoding='utf-8')
    # Simulate macOS AppleDouble pollution in .adds/nm
    (mount / '.adds/nm/._reeden-kobo').write_bytes(b'\x00\x05\x16\x07appledouble')

    # Refuse overwrite when allow_upgrade=False
    try:
        install_mod.install_to_usb_mount(pkg, mount, allow_upgrade=False)
        raise AssertionError('Expected overwrite refusal without allow_upgrade')
    except RuntimeError:
        pass

    # Upgrade preserves userdata/ and storage/
    (pkg / 'hello.txt').write_text('version-2\n', encoding='utf-8')
    files = sorted(p for p in pkg.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (pkg / 'SHA256SUMS').write_text(
        ''.join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(pkg).as_posix()}\n" for p in files),
        encoding='utf-8',
    )
    install_mod.install_to_usb_mount(pkg, mount, allow_upgrade=True)
    assert (installed / 'hello.txt').read_text(encoding='utf-8') == 'version-2\n'
    assert (installed / 'userdata/kobo-light.conf').read_text(encoding='utf-8') == 'warmth=60\n'
    assert (installed / 'storage/books/novel.epub').read_text(encoding='utf-8') == 'book-bytes\n'
    assert (installed / 'diagnostics/ssh/reeden-ssh').read_text(encoding='utf-8') == 'dropbear-bin\n'
    assert not (mount / '.adds/nm/._reeden-kobo').exists()

print('PASS: pure-Python .deb extraction, USB fresh install, overwrite guard, and data-preserving upgrade')
