#!/usr/bin/env python3
"""Reeden Kobo (0.7.9) One-Click Builder & Installer.

Combines the verified MIT Kobo compatibility layer with the user's unmodified
official Reeden Kindle 1.43.1+780 or 1.43.2+791 core and pinned Debian Bullseye ARMHF runtime
packages, then installs to a Kobo Clara Colour (393) via USB or SSH (or outputs
a verified local package directory).

100% Python 3 standard library; works on macOS, Windows, and Linux without
requiring Zig, fontTools, ar, or external tar binaries.
"""
import argparse
import datetime
import hashlib
import http.client
import io
import json
import os
import pathlib
import platform
import posixpath
import re
import shutil
import ssl
import stat
import subprocess
import sys
if sys.version_info < (3, 10):
    sys.exit('需要 Python 3.10 或更新版本，请升级 Python 后重新运行安装器。')
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent
SUPPORTED_CORES = {
    '1.43.2+791': {
        'libflutter_engine.so': '854a11f2f649eeab245279f2b5b2c6db2df4f68ba2b28aa3e111ce834058bb9e',
        'libapp.so': 'daf57297da3db5293ced3f12c708d80c1fce18141da15e3b504070dd702b599a',
    },
    '1.43.1+780': {
        'libflutter_engine.so': '8eca49cb5a50573f61668706a99807e3987902b37c311d982872d31fd09e4f37',
        'libapp.so': '6662907460efb82361eb7c3bdffcb83735bda381f6d98445d61e00e10bc6930e',
    },
}
OFFICIAL_LATEST_URL = 'https://download.reeden.app/Reeden/latest/Reeden-kindle.zip'
PORT_VERSION = '0.7.9-experimental'
SUPPORTED_PRODUCT = '393'


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def copy_regular(src: pathlib.Path, dst: pathlib.Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src.resolve(), dst)


def copy_lf_script(src: pathlib.Path, dst: pathlib.Path, mode: int = 0o755) -> None:
    """Copy a shell script while normalizing CRLF -> LF (Windows autocrlf safety)."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    raw = src.resolve().read_bytes().replace(b'\r\n', b'\n')
    dst.write_bytes(raw)
    dst.chmod(mode)


def cleanup_appledouble(root_dir: pathlib.Path) -> None:
    """Remove macOS AppleDouble (._*) metadata files created on FAT32 volumes."""
    if not root_dir.exists():
        return
    if root_dir.is_file():
        if root_dir.name.startswith('._'):
            root_dir.unlink(missing_ok=True)
        return
    for p in list(root_dir.rglob('._*')):
        if p.is_file():
            p.unlink(missing_ok=True)


def copytree_fat32_safe(src: pathlib.Path, dst: pathlib.Path) -> None:
    """Copy directory contents using copyfile (omitting macOS xattrs on FAT32)."""
    shutil.copytree(src, dst, copy_function=shutil.copyfile, dirs_exist_ok=True)
    cleanup_appledouble(dst)


def verify_prebuilt(root: pathlib.Path) -> pathlib.Path:
    prebuilt = root / 'prebuilt'
    manifest = prebuilt / 'PREBUILT_SHA256SUMS'
    if not manifest.is_file():
        raise RuntimeError(f'缺少预编译清单: {manifest}')
    for line in manifest.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        digest, rel_str = line.split('  ', 1)
        rel = pathlib.PurePosixPath(rel_str)
        if rel.is_absolute() or '..' in rel.parts:
            raise ValueError(f'不安全的预编译路径: {rel_str}')
        target = prebuilt.joinpath(*rel.parts)
        if not target.is_file():
            raise FileNotFoundError(f'缺少预编译文件: {target}')
        if sha256_file(target) != digest:
            raise ValueError(f'预编译文件哈希不匹配: {rel_str}')
    return prebuilt


def find_core_dir_in(base: pathlib.Path) -> pathlib.Path | None:
    candidates = [
        base,
        base / 'reeden' / 'reeden',
        base / 'reeden',
    ]
    for cand in candidates:
        if (cand / 'lib' / 'libflutter_engine.so').is_file() and (cand / 'lib' / 'libapp.so').is_file():
            return cand
    return None


def discover_official_inputs(root: pathlib.Path) -> list[pathlib.Path]:
    """Enumerate candidates in priority order; validation happens before selection."""
    env_core = os.environ.get('REEDEN_OFFICIAL_CORE', '').strip()
    home = pathlib.Path.home()
    cwd = pathlib.Path.cwd()
    search_paths = ([pathlib.Path(env_core).expanduser()] if env_core else []) + [
        root / 'Reeden-kindle.zip',
        cwd / 'Reeden-kindle.zip',
        root / 'cache' / 'Reeden-kindle.zip',
        root / 'reeden' / 'reeden',
        cwd / 'reeden' / 'reeden',
        home / 'Downloads' / 'Reeden-kindle.zip',
        home / 'Downloads' / 'reeden' / 'reeden',
        home / 'Desktop' / 'Reeden-kindle.zip',
    ]
    # Include browser-renamed downloads, but never unfinished .part files.
    for folder in (root, cwd, home / 'Downloads', home / 'Desktop'):
        try:
            for item in sorted(folder.iterdir()):
                low = item.name.lower()
                if item.is_file() and low.startswith('reeden') and 'kindle' in low and low.endswith('.zip'):
                    search_paths.append(item)
        except OSError:
            continue
    candidates = []
    seen = set()
    for path in search_paths:
        key = path.absolute()
        if key not in seen and path.exists():
            seen.add(key)
            candidates.append(path)
    return candidates


def clean_dragged_path(raw: str) -> pathlib.Path:
    s = raw.strip()
    if (s.startswith('"') and s.endswith('"')) or (s.startswith("'") and s.endswith("'")):
        s = s[1:-1]
    elif platform.system() != 'Windows':
        s = s.replace('\\ ', ' ')
    return pathlib.Path(s).expanduser()


def archive_parts(name: str, allow_root: bool = False) -> tuple[str, ...]:
    """Accept a portable, relative archive path without rewriting unsafe names."""
    if not name or name.startswith('/') or any(c in name for c in '\\:<>"|?*') or any(ord(c) < 32 for c in name):
        raise ValueError(f'不安全的归档路径：{name!r}')
    parts = tuple(p for p in name.split('/') if p not in ('', '.'))
    reserved = {'CON', 'PRN', 'AUX', 'NUL', 'CONIN$', 'CONOUT$'}
    reserved.update(f'{prefix}{i}' for prefix in ('COM', 'LPT') for i in '123456789¹²³')
    for part in parts:
        if part == '..' or part.endswith((' ', '.')) or part.split('.')[0].upper() in reserved:
            raise ValueError(f'不安全的归档路径：{name!r}')
    if not parts and not allow_root:
        raise ValueError(f'空归档路径：{name!r}')
    return parts


def archive_target(root: pathlib.Path, parts: tuple[str, ...]) -> pathlib.Path:
    root = root.resolve()
    target = root
    for part in parts:
        target = target / part
        if target.is_symlink():
            raise ValueError('归档目标含符号链接，已拒绝解包')
        if target.is_file() and target.stat().st_nlink > 1:
            raise ValueError('归档目标含已有硬链接，已拒绝解包')
    if not target.resolve().is_relative_to(root):
        raise ValueError('归档目标越过解包根目录')
    return target


def validate_archive_layout(entries: list[tuple[tuple[str, ...], bool]]) -> None:
    """Reject duplicate/case-colliding files and non-directory parent entries."""
    types = {}
    spellings = {}
    for parts, is_dir in entries:
        for i in range(1, len(parts) + 1):
            prefix = parts[:i]
            key = tuple(p.casefold() for p in prefix)
            if key in spellings and spellings[key] != prefix:
                raise ValueError('归档含大小写冲突的路径')
            spellings[key] = prefix
        if parts in types and not (is_dir and types[parts]):
            raise ValueError('归档含重复文件路径')
        types[parts] = is_dir
    for parts in types:
        for i in range(1, len(parts)):
            if parts[:i] in types and not types[parts[:i]]:
                raise ValueError('归档文件或链接被用作父目录')


def extract_official_zip(archive: pathlib.Path, dest: pathlib.Path) -> None:
    with zipfile.ZipFile(archive) as zf:
        planned = []
        for info in zf.infolist():
            if info.orig_filename != info.filename:
                raise ValueError('ZIP 文件名含被截断的字符')
            parts = archive_parts(info.filename, allow_root=info.is_dir())
            mode = stat.S_IFMT(info.external_attr >> 16)
            if mode not in (0, stat.S_IFREG, stat.S_IFDIR) or (mode == stat.S_IFDIR and not info.is_dir()):
                raise ValueError('官方 ZIP 含链接或特殊文件，已拒绝解包')
            archive_target(dest, parts)
            planned.append((info, parts))
        validate_archive_layout([(parts, info.is_dir()) for info, parts in planned])
        # Validate the complete archive before writing the first payload.
        for info, parts in planned:
            target = archive_target(dest, parts)
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, target.open('wb') as out:
                    shutil.copyfileobj(src, out)


def validate_official_core(core_dir: pathlib.Path) -> str:
    """Identify immutable core libraries, then verify the matching resources."""
    if core_dir.is_symlink():
        raise ValueError('官方核心目录不允许符号链接')
    files = {}
    for path in core_dir.rglob('*'):
        rel = path.relative_to(core_dir).as_posix()
        archive_parts(rel)
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('官方核心目录含链接或特殊文件，已拒绝组包')
        if path.is_file():
            files[rel] = path
    validate_archive_layout([(tuple(rel.split('/')), False) for rel in files])
    actual_hashes = {name: sha256_file(core_dir / 'lib' / name)
                     for name in ('libflutter_engine.so', 'libapp.so')}
    matched_version = next(
        (ver for ver, expected in SUPPORTED_CORES.items() if actual_hashes == expected), None,
    )
    if matched_version is None:
        raise ValueError('官方核心版本不在已验证白名单内 (当前支持: '
                         + ' / '.join(SUPPORTED_CORES) + ')。请提供受支持的官方包。')
    for rel in ('data/flutter_assets', 'lib/glibc', 'licenses'):
        if not (core_dir / rel).is_dir():
            raise FileNotFoundError(f'缺少官方子目录: {rel}')
    manifest = ROOT / 'vendor' / f'official-core-{matched_version}.sha256'
    if matched_version == '1.43.2+791':
        # A missing manifest is an installation error, never a weaker fallback.
        expected = {}
        for line in manifest.read_text(encoding='utf-8').splitlines():
            digest, rel = line.split('  ', 1)
            archive_parts(rel)
            if len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest) or rel in expected:
                raise ValueError('官方核心 SHA 清单无效')
            expected[rel] = digest
        if not expected or files.keys() != expected.keys():
            raise ValueError('官方核心文件集合与 SHA 清单不一致（缺失或多余文件）')
        for rel, digest in expected.items():
            if sha256_file(files[rel]) != digest:
                raise ValueError(f'官方核心文件哈希不匹配: {rel}')
    else:
        # No historical full-file manifest: these are minimum assembly/runtime
        # requirements, not provenance checks for the remaining 780 files.
        assets = core_dir / 'data/flutter_assets'
        for rel in ('THIRD_PARTY_NOTICES.txt', 'lib/glibc/reeden-kindle',
                    'lib/glibc/libc.so.6'):
            path = core_dir / rel
            if not path.is_file() or path.stat().st_size == 0:
                raise ValueError(f'官方组包必需文件缺失或为空: {rel}')
        version = json.loads((assets / 'version.json').read_text(encoding='utf-8'))
        if not isinstance(version, dict) or f'{version.get("version")}+{version.get("build_number")}' != matched_version:
            raise ValueError('官方资源版本与核心库不匹配')
        for rel in ('data/icudtl.dat', 'data/flutter_assets/AssetManifest.bin',
                    'data/flutter_assets/FontManifest.json'):
            if not (core_dir / rel).is_file() or (core_dir / rel).stat().st_size == 0:
                raise ValueError(f'官方资源文件缺失或为空: {rel}')
        fonts = json.loads((assets / 'FontManifest.json').read_text(encoding='utf-8'))
        if not isinstance(fonts, list):
            raise ValueError('官方字体清单必须为数组')
        for font in fonts:
            if not isinstance(font, dict) or not isinstance(font.get('fonts'), list):
                raise ValueError('官方字体清单条目无效')
            for face in font['fonts']:
                if not isinstance(face, dict) or not isinstance(face.get('asset'), str):
                    raise ValueError('官方字体资源路径必须为字符串')
                parts = archive_parts(face['asset'])
                path = assets.joinpath(*parts)
                if not path.is_file() or path.stat().st_size == 0:
                    raise ValueError(f'官方字体资源缺失或为空: {face["asset"]}')
        for path in (core_dir / 'lib').glob('glibc*/data/flutter_assets/version.json'):
            if json.loads(path.read_text(encoding='utf-8')) != version:
                raise ValueError('官方随包运行库资源版本不匹配')
        print('[1/4] 注意：780 仅验证两库白名单、版本与资源结构，尚无官方全文件 SHA 清单。')
    return matched_version


def load_official_input(target: pathlib.Path, temp_root: pathlib.Path) -> tuple[pathlib.Path, str]:
    if not target.exists():
        raise FileNotFoundError(f'未找到官方包: {target}')
    if target.is_file():
        # Each attempt has its own extraction tree; never mix failed candidates.
        extract_dir = pathlib.Path(tempfile.mkdtemp(prefix='official-', dir=temp_root))
        try:
            extract_official_zip(target, extract_dir)
            core_dir = find_core_dir_in(extract_dir)
            if core_dir is None:
                raise ValueError('官方 ZIP 缺少核心目录')
            version = validate_official_core(core_dir)
        except Exception:
            shutil.rmtree(extract_dir)
            raise
    else:
        core_dir = find_core_dir_in(target)
        if core_dir is None:
            raise ValueError('未找到官方核心目录')
        version = validate_official_core(core_dir)
    return core_dir, version


def download_official_core(temp_root: pathlib.Path) -> tuple[pathlib.Path, str]:
    """Commit only a fully verified download; preserve any existing cache."""
    cache_zip = ROOT / 'cache' / 'Reeden-kindle.zip'
    cache_zip.parent.mkdir(parents=True, exist_ok=True)
    for attempt in range(3):
        fd, name = tempfile.mkstemp(prefix='Reeden-kindle-', suffix='.part', dir=cache_zip.parent)
        os.close(fd)
        partial = pathlib.Path(name)
        result = None
        try:
            print(f'[1/4] 正在从 Reeden 官网下载并校验 ({OFFICIAL_LATEST_URL}, {attempt + 1}/3) ...')
            req = urllib.request.Request(OFFICIAL_LATEST_URL, headers={'User-Agent': 'reeden-kobo-installer/0.7.9'})
            with open_url_with_cert_fallback(req, timeout=180) as resp, partial.open('wb') as out:
                shutil.copyfileobj(resp, out)
                expected_size = resp.headers.get('Content-Length')
                if expected_size is not None and out.tell() != int(expected_size):
                    raise OSError('官方下载不完整（Content-Length 不符）')
            result = load_official_input(partial, temp_root)
            os.replace(partial, cache_zip)
            return result
        except (OSError, EOFError, http.client.HTTPException, zipfile.BadZipFile, zlib.error) as exc:
            if result is not None:
                extracted = temp_root / result[0].relative_to(temp_root).parts[0]
                shutil.rmtree(extracted)
            if attempt == 2:
                raise RuntimeError('官方下载失败，原缓存未更改；请重试或用 --official-core 指定受支持的官方包。') from exc
            print(f'[1/4] 下载未完成，稍后重试: {exc}')
            time.sleep(attempt + 1)
        finally:
            partial.unlink(missing_ok=True)
    raise AssertionError('unreachable')


def resolve_official_core(user_arg: str | None, temp_root: pathlib.Path, auto_download: bool = False) -> tuple[pathlib.Path, str]:
    if user_arg and auto_download:
        raise ValueError('--official-core 与 --download-official 不能同时使用')
    if user_arg:
        result = load_official_input(pathlib.Path(user_arg).expanduser().absolute(), temp_root)
    elif auto_download:
        result = download_official_core(temp_root)
    else:
        result = None
        for target in discover_official_inputs(ROOT):
            try:
                result = load_official_input(target, temp_root)
            except (OSError, ValueError, EOFError, zipfile.BadZipFile, zlib.error, KeyError, TypeError) as exc:
                print(f'[1/4] 跳过无效候选包 {target}: {exc}')
                continue
            print(f'[1/4] 自动选择已校验官方包: {target}')
            break
        if result is None and sys.stdin.isatty():
            print('未找到已校验的官方包。支持版本：' + ' / '.join(SUPPORTED_CORES))
            entered = input('拖入官方 ZIP 并回车（直接回车从官网下载并校验）: ').strip()
            result = (load_official_input(clean_dragged_path(entered).absolute(), temp_root)
                      if entered else download_official_core(temp_root))
        if result is None:
            raise FileNotFoundError('未找到有效官方包；请用 --official-core 指定受支持的官方包，或 --download-official 强制下载。')
    print(f'[1/4] 官方 Reeden {result[1]} 核心校验通过（原字节未修改）')
    return result


def open_url_with_cert_fallback(req: urllib.request.Request, timeout: int = 60):
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.URLError as exc:
        if isinstance(exc.reason, ssl.SSLCertVerificationError) and pathlib.Path('/etc/ssl/cert.pem').is_file():
            ctx = ssl.create_default_context(cafile='/etc/ssl/cert.pem')
            return urllib.request.urlopen(req, timeout=timeout, context=ctx)
        raise


def download_verified(url: str, sha256: str, cache_dir: pathlib.Path, alt_dirs: list[pathlib.Path]) -> pathlib.Path:
    filename = pathlib.Path(url).name
    for d in [cache_dir, *alt_dirs]:
        cand = d / filename
        if cand.is_file() and sha256_file(cand) == sha256:
            return cand
    cache_dir.mkdir(parents=True, exist_ok=True)
    dest = cache_dir / filename
    print(f'      下载依赖: {filename} ...')
    req = urllib.request.Request(url, headers={'User-Agent': 'reeden-kobo-installer/0.7.9'})
    with open_url_with_cert_fallback(req, timeout=60) as resp, dest.open('wb') as out:
        shutil.copyfileobj(resp, out)
    if sha256_file(dest) != sha256:
        dest.unlink(missing_ok=True)
        raise RuntimeError(f'下载文件哈希校验失败: {filename}')
    return dest


def extract_deb_pure(deb_path: pathlib.Path, dest_dir: pathlib.Path) -> None:
    data = deb_path.read_bytes()
    if not data.startswith(b'!<arch>\n'):
        raise ValueError(f'非法的 deb/ar 归档文件: {deb_path}')
    pos = 8
    data_payload = None
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[0:16].decode('ascii', errors='replace').strip()
        size_text = hdr[48:58].decode('ascii').strip()
        if hdr[58:60] != b'`\n' or not size_text.isdigit():
            raise ValueError('非法的 deb/ar 成员头或长度')
        size = int(size_text)
        pos += 60
        if size > len(data) - pos:
            raise ValueError('deb/ar 成员数据截断')
        body = data[pos:pos + size]
        pos += size + (size % 2)
        if name.endswith('/'):
            name = name[:-1]
        if name.startswith('data.tar'):
            if data_payload is not None:
                raise ValueError('deb/ar 含多个 data.tar 成员')
            data_payload = body
    if pos != len(data):
        raise ValueError('deb/ar 尾部截断或格式异常')
    if data_payload is None:
        raise ValueError(f'未在 {deb_path} 中找到 data.tar.*')

    symlinks = []
    with tarfile.open(fileobj=io.BytesIO(data_payload), mode='r:*') as tf:
        planned = []
        for member in tf.getmembers():
            parts = archive_parts(member.name, allow_root=member.isdir())
            if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
                raise ValueError('deb 数据归档含特殊文件，已拒绝解包')
            archive_target(dest_dir, parts)
            if member.issym() or member.islnk():
                link = member.linkname
                if not link or link.startswith('/') or '\\' in link or ':' in link or any(ord(c) < 32 for c in link):
                    raise ValueError('不安全的 deb 链接目标')
                base = parts[:-1] if member.issym() else ()
                relative = posixpath.normpath('/'.join((*base, link)))
                source_parts = archive_parts(relative, allow_root=True)
                archive_target(dest_dir, source_parts)
                symlinks.append((parts, source_parts))
            planned.append((member, parts))
        validate_archive_layout([(parts, member.isdir()) for member, parts in planned])
        for member, parts in planned:
            target = archive_target(dest_dir, parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with tf.extractfile(member) as src:
                    target.write_bytes(src.read())

    # Materialize links as regular file copies only inside the extraction root.
    # Directory, missing and cyclic links remain absent (no host links created).
    for _ in range(len(symlinks)):
        remaining = []
        progress = False
        for target_parts, source_parts in symlinks:
            target = archive_target(dest_dir, target_parts)
            src_path = archive_target(dest_dir, source_parts)
            if src_path.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src_path, target)
                progress = True
            else:
                remaining.append((target_parts, source_parts))
        if not remaining or not progress:
            break
        symlinks = remaining


def create_resource_recovery(dest: pathlib.Path) -> pathlib.Path:
    """Keep verified installed resources recoverable without bundling a core in Git.

    The archive is built locally from the already validated official core and the
    completed UI overlay. It is itself included in the final package SHA list.
    Runtime recovery extracts individual missing files and verifies their digest;
    it never unpacks the archive over an existing installation wholesale.
    """
    archive = dest / 'integrity/resources.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    resources = sorted(path for base in ('core/data', 'ui-assets')
                       for path in (dest / base).rglob('*') if path.is_file())
    with tarfile.open(archive, 'w:gz', compresslevel=6) as tf:
        for path in resources:
            if path.is_symlink():
                raise ValueError('Resource recovery does not accept symlinks')
            tf.add(path, arcname=path.relative_to(dest).as_posix(), recursive=False)
    return archive


def assemble_package(
    core_dir: pathlib.Path,
    official_version: str,
    prebuilt_dir: pathlib.Path,
    dest: pathlib.Path,
    temp_root: pathlib.Path,
) -> int:
    cache_dir = ROOT / 'cache'
    alt_dirs = [ROOT.parent / 'deps']
    deb_manifest = json.loads((ROOT / 'vendor/debian-runtime-manifest.json').read_text(encoding='utf-8'))
    font_manifest = json.loads((ROOT / 'vendor/font-manifest.json').read_text(encoding='utf-8'))

    print('[2/4] 正在准备并校验 Debian Bullseye ARMHF 运行库与开源字体 ...')
    deps_root = temp_root / 'debian-runtime'
    deps_root.mkdir(parents=True, exist_ok=True)
    for item in deb_manifest:
        deb_file = download_verified(item['url'], item['sha256'], cache_dir, alt_dirs)
        extract_deb_pure(deb_file, deps_root)
    cjk_font = download_verified(font_manifest['url'], font_manifest['sha256'], cache_dir, alt_dirs)

    print('[3/4] 正在组装完整运行包 (无符号链接，适配 FAT32 分区) ...')
    for d in [
        'bin', 'lib', 'runtime', 'fonts', 'certs', 'checks', 'licenses',
        'docs', 'state', 'logs', 'userdata', 'storage/books', 'storage/documents',
        'storage/screenshots', 'cache',
    ]:
        (dest / d).mkdir(parents=True, exist_ok=True)

    # 1. Official core (byte-for-byte unchanged)
    for src in (core_dir / 'lib').glob('*.so*'):
        copy_regular(src, dest / 'core/lib' / src.name)
    shutil.copytree(core_dir / 'data', dest / 'core/data', dirs_exist_ok=True)
    for src in (core_dir / 'lib/glibc').iterdir():
        if src.is_file():
            copy_regular(src, dest / 'runtime' / src.name)
    shutil.copytree(core_dir / 'licenses', dest / 'licenses/official', dirs_exist_ok=True)
    copy_regular(core_dir / 'THIRD_PARTY_NOTICES.txt', dest / 'licenses/official/THIRD_PARTY_NOTICES.txt')

    # 2. UI assets overlay (CJK + Math fallback font)
    shutil.copytree(core_dir / 'data/flutter_assets', dest / 'ui-assets', dirs_exist_ok=True)
    copy_regular(
        prebuilt_dir / 'fonts/KoboCJKMath-Regular.ttf',
        dest / 'ui-assets/assets/fonts/KoboCJKMath-Regular.ttf',
    )
    font_manifest_path = dest / 'ui-assets/FontManifest.json'
    families = json.loads(font_manifest_path.read_text(encoding='utf-8'))
    for family in families:
        if family.get('family') == 'NotoSansMath':
            family['fonts'] = [{'asset': 'assets/fonts/KoboCJKMath-Regular.ttf'}]
    font_manifest_path.write_text(json.dumps(families, separators=(',', ':')) + '\n', encoding='utf-8', newline='\n')

    # 3. Debian ARMHF runtime & CA certificates
    for tree in [deps_root / 'lib/arm-linux-gnueabihf', deps_root / 'usr/lib/arm-linux-gnueabihf']:
        for src in tree.glob('*.so*'):
            copy_regular(src, dest / 'lib' / src.name)
    for stem in ['gio', 'glib', 'gobject', 'gmodule', 'gthread']:
        copy_regular(dest / f'lib/lib{stem}-2.0.so.0', dest / f'lib/lib{stem}-2.0.so')
    for src in (deps_root / 'usr/share/doc').glob('*/copyright'):
        copy_regular(src, dest / 'licenses/debian' / f'{src.parent.name}-copyright')
    certificates = sorted((deps_root / 'usr/share/ca-certificates/mozilla').glob('*.crt'))
    if len(certificates) < 100:
        raise RuntimeError('缺少 Mozilla 公共根证书集合')
    (dest / 'certs/ca-certificates.crt').write_bytes(b'\n'.join(x.read_bytes() for x in certificates))

    # 4. Prebuilt Kobo adapter binaries & LF-normalized shell scripts
    copy_regular(prebuilt_dir / 'bin/fb-guard', dest / 'bin/fb-guard')
    copy_regular(prebuilt_dir / 'bin/reeden-kindle', dest / 'bin/reeden-kindle')
    copy_regular(prebuilt_dir / 'lib/libreeden_kobo_compat.so', dest / 'lib/libreeden_kobo_compat.so')
    for src in sorted((prebuilt_dir / 'checks').iterdir()):
        if src.is_file():
            copy_regular(src, dest / 'checks' / src.name)
    for name in [
        'run.sh', 'session.sh', 'recover.sh', 'guardian.sh',
        'native-session.sh', 'power.sh', 'wifi.sh', 'precheck.sh', 'reeden-ssh.sh',
        'uninstall.sh', 'install.sh', 'verify.sh', 'integrity.sh',
    ]:
        copy_lf_script(ROOT / 'scripts' / name, dest / name, 0o755)
    for name in ['lipc-get-prop', 'lipc-set-prop']:
        copy_lf_script(ROOT / 'scripts' / name, dest / 'bin' / name, 0o755)

    # 5. Fonts, licenses, docs, NickelMenu config, build-info, SHA256SUMS
    copy_regular(ROOT / 'LICENSE', dest / 'licenses/adapter-MIT-LICENSE')
    copy_regular(cjk_font, dest / 'fonts/NotoSansCJKsc-Regular.otf')
    for src in sorted((ROOT / 'vendor').glob('*LICENSE*')):
        copy_regular(src, dest / 'licenses' / src.name)
    copy_regular(ROOT / 'vendor/debian-runtime-manifest.json', dest / 'licenses/debian-runtime-manifest.json')
    for src in sorted((ROOT / 'docs').rglob('*')):
        if src.is_file():
            copy_regular(src, dest / 'docs' / src.relative_to(ROOT / 'docs'))
    for group in ('bin', 'runtime', 'checks'):
        for src in (dest / group).iterdir():
            if src.is_file():
                src.chmod(0o755)

    copy_lf_script(ROOT / 'scripts/nickelmenu.conf', dest / 'nickelmenu.conf', 0o644)
    info = {
        'port_version': PORT_VERSION,
        'official_version': official_version,
        'model': 'Kobo Clara Colour (393)',
        'firmware': '4.46.23836',
        'first_launch': 'NOT_PERFORMED',
        'official_core_unmodified': True,
        'ui_assets_overlay': True,
        'font_sha256': sha256_file(dest / 'fonts/NotoSansCJKsc-Regular.otf'),
    }
    (dest / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8', newline='\n')

    cleanup_appledouble(dest)
    create_resource_recovery(dest)
    files = sorted(x for x in dest.rglob('*') if x.is_file() and x.name != 'SHA256SUMS')
    sums_text = ''.join(f'{sha256_file(x)}  {x.relative_to(dest).as_posix()}\n' for x in files)
    (dest / 'SHA256SUMS').write_text(sums_text, encoding='utf-8', newline='\n')
    return len(files)


def detect_kobo_usb_mounts() -> list[pathlib.Path]:
    found = []
    sys_name = platform.system()
    candidates = []
    if sys_name == 'Darwin':
        vol = pathlib.Path('/Volumes')
        if vol.is_dir():
            candidates.extend(sorted(vol.iterdir()))
    elif sys_name == 'Windows':
        for letter in 'DEFGHIJKLMNOPQRSTUVWXYZ':
            candidates.append(pathlib.Path(f'{letter}:\\'))
    else:
        for base in (pathlib.Path('/media'), pathlib.Path('/run/media'), pathlib.Path('/mnt')):
            if base.is_dir():
                for child in sorted(base.iterdir()):
                    candidates.append(child)
                    if child.is_dir():
                        try:
                            candidates.extend(sorted(child.iterdir()))
                        except PermissionError:
                            pass
    seen = set()
    for cand in candidates:
        try:
            ver = cand / '.kobo' / 'version'
            if ver.is_file():
                resolved = cand.resolve()
                if resolved not in seen:
                    seen.add(resolved)
                    found.append(resolved)
        except OSError:
            continue
    return found


def parse_kobo_product(mount: pathlib.Path) -> tuple[str, str]:
    ver_file = mount / '.kobo' / 'version'
    if not ver_file.is_file():
        raise FileNotFoundError(f'未在 {mount} 找到 .kobo/version，请确认路径为 Kobo 根目录')
    raw = ver_file.read_text(encoding='utf-8', errors='replace').strip()
    parts = [p.strip() for p in raw.split(',')]
    firmware = parts[2] if len(parts) > 2 else 'unknown'
    last = parts[-1] if parts else ''
    product = last[-3:] if len(last) >= 3 else last
    return product, firmware


KOBO_EXCLUDE_SYNC_FOLDERS_INNER = r'\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+'
KOBO_EXCLUDE_SYNC_FOLDERS = f'({KOBO_EXCLUDE_SYNC_FOLDERS_INNER})'


def _unescape_qsettings_value(raw: str) -> str:
    r"""Unescape Qt QSettings INI backslash sequences (e.g. \\. -> \.)."""
    out: list[str] = []
    i = 0
    n = len(raw)
    while i < n:
        ch = raw[i]
        if ch == '\\' and i + 1 < n:
            out.append(raw[i + 1])
            i += 2
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def _kobo_exclude_pattern_covers_adds(raw_value: str) -> bool:
    """Return True if a QSettings ExcludeSyncFolders value already ignores .adds."""
    val = raw_value.strip()
    if not val:
        return False
    if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
        val = val[1:-1]
    pattern = _unescape_qsettings_value(val)
    try:
        rx = re.compile(pattern)
    except re.error:
        return False
    return bool(rx.search('.adds')) and bool(rx.search('.adds/reeden-kobo'))


def update_kobo_exclude_sync_folders_text(text: str) -> tuple[str, str]:
    """Ensure [FeatureSettings]/ExcludeSyncFolders ignores .adds without clobbering other settings.

    Returns (updated_text, status) where status is one of:
      - 'already_configured': existing rule already excludes .adds (text unchanged)
      - 'added': added ExcludeSyncFolders (and [FeatureSettings] if missing)
      - 'updated': filled empty ExcludeSyncFolders or merged with custom rule
      - 'manual_required': existing rule is quoted/unparseable; text unchanged
    """
    lines = text.splitlines()
    current_section: str | None = None
    feature_sections: list[tuple[int, int]] = []  # (header_idx, end_idx_exclusive)
    feature_start: int | None = None
    exclude_entries: list[tuple[int, str]] = []  # (line_idx, raw_value)

    for idx, raw_line in enumerate(lines):
        stripped = raw_line.strip()
        if stripped.startswith('[') and stripped.endswith(']') and len(stripped) >= 2:
            if feature_start is not None:
                feature_sections.append((feature_start, idx))
                feature_start = None
            current_section = stripped[1:-1].strip()
            if current_section == 'FeatureSettings':
                feature_start = idx
            continue
        if current_section == 'FeatureSettings':
            m = re.match(r'^\s*ExcludeSyncFolders\s*=(.*)$', raw_line)
            if m:
                exclude_entries.append((idx, m.group(1)))

    if feature_start is not None:
        feature_sections.append((feature_start, len(lines)))

    target_line = f'ExcludeSyncFolders={KOBO_EXCLUDE_SYNC_FOLDERS}'

    if exclude_entries:
        last_idx, last_val = exclude_entries[-1]
        trimmed = last_val.strip()
        if not trimmed:
            new_lines = list(lines)
            new_lines[last_idx] = target_line
            return '\n'.join(new_lines) + '\n', 'updated'
        if _kobo_exclude_pattern_covers_adds(trimmed):
            return text, 'already_configured'
        # Preserve custom non-empty regex if unquoted and valid
        if trimmed.startswith('"') or trimmed.endswith('"'):
            return text, 'manual_required'
        try:
            re.compile(_unescape_qsettings_value(trimmed))
        except re.error:
            return text, 'manual_required'
        merged_val = f'(({trimmed})|{KOBO_EXCLUDE_SYNC_FOLDERS_INNER})'
        try:
            merged_rx = re.compile(_unescape_qsettings_value(merged_val))
        except re.error:
            return text, 'manual_required'
        if not (merged_rx.search('.adds') and merged_rx.search('.adds/reeden-kobo')) or merged_rx.search('.kobo'):
            return text, 'manual_required'
        new_lines = list(lines)
        new_lines[last_idx] = f'ExcludeSyncFolders={merged_val}'
        return '\n'.join(new_lines) + '\n', 'updated'

    if feature_sections:
        _, end_idx = feature_sections[-1]
        insert_at = end_idx
        while insert_at > feature_sections[-1][0] + 1 and not lines[insert_at - 1].strip():
            insert_at -= 1
        new_lines = lines[:insert_at] + [target_line] + lines[insert_at:]
        return '\n'.join(new_lines) + '\n', 'added'

    new_lines = list(lines)
    if new_lines and new_lines[-1].strip():
        new_lines.append('')
    new_lines.extend(['[FeatureSettings]', target_line])
    return '\n'.join(new_lines) + '\n', 'added'


def _validate_kobo_conf_path(mount: pathlib.Path) -> pathlib.Path:
    kobo_dir = mount / '.kobo'
    conf_dir = kobo_dir / 'Kobo'
    conf_path = conf_dir / 'Kobo eReader.conf'
    for candidate in (kobo_dir, conf_dir, conf_path):
        if candidate.is_symlink():
            raise RuntimeError(f'检测到不安全的符号链接，已拒绝修改配置: {candidate}')
    if conf_dir.exists() and not conf_dir.is_dir():
        raise RuntimeError(f'配置目录异常，不是有效目录: {conf_dir}')
    if conf_path.exists() and not conf_path.is_file():
        raise RuntimeError(f'配置文件异常，不是普通文件: {conf_path}')
    return conf_path


def ensure_kobo_exclude_sync_folders(mount: pathlib.Path) -> str:
    """Configure Kobo Nickel to skip .adds/ so Reeden assets are not indexed as books."""
    conf_path = _validate_kobo_conf_path(mount)
    if conf_path.exists():
        raw_bytes = conf_path.read_bytes()
        original_text = raw_bytes.decode('utf-8', errors='surrogateescape')
    else:
        original_text = ''

    updated_text, status = update_kobo_exclude_sync_folders_text(original_text)
    if status in ('already_configured', 'manual_required'):
        return status

    conf_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: pathlib.Path | None = None
    try:
        fd, tmp_name = tempfile.mkstemp(prefix='.Kobo-eReader.conf.reeden-', dir=conf_path.parent)
        tmp_path = pathlib.Path(tmp_name)
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(updated_text.encode('utf-8', errors='surrogateescape'))
                f.flush()
                os.fsync(f.fileno())
        except BaseException:
            tmp_path.unlink(missing_ok=True)
            raise
        os.replace(tmp_path, conf_path)
        tmp_path = None
    finally:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)
    cleanup_appledouble(conf_path.parent / '._Kobo eReader.conf')
    return status


def choose_kobo_usb_mount(mounts: list[pathlib.Path]) -> pathlib.Path:
    if not mounts:
        raise RuntimeError('未检测到 Kobo USB 盘，请连接数据线并在 Kobo 屏幕点击「连接」。')
    if len(mounts) != 1:
        raise RuntimeError('检测到多个 Kobo USB 盘，为避免操作错误设备已停止。请只连接一台，或用 --kobo-mount 指定根目录。')
    return mounts[0]


def install_to_usb_mount(pkg_dir: pathlib.Path, mount: pathlib.Path, allow_upgrade: bool) -> None:
    product, firmware = parse_kobo_product(mount)
    if product != SUPPORTED_PRODUCT:
        raise RuntimeError(
            f'机型不匹配：检测到 Kobo 产品代码 {product} (固件 {firmware})，'
            f'当前版本仅支持 Kobo Clara Colour (产品代码 {SUPPORTED_PRODUCT})。未对设备做任何修改。'
        )
    nm_dir = mount / '.adds' / 'nm'
    if not nm_dir.is_dir():
        raise RuntimeError(
            f'未检测到 NickelMenu 目录 ({nm_dir})。\n'
            '请先为 Kobo 安装 NickelMenu (https://pgaskin.net/NickelMenu/)，安装脚本未对设备做任何更改。'
        )
    dest = mount / '.adds' / 'reeden-kobo'
    entry = nm_dir / 'reeden-kobo'
    if entry.exists() and not dest.exists() and not allow_upgrade:
        raise RuntimeError('已有同名 NickelMenu 配置，未覆盖；请检查后使用 --upgrade。')
    if dest.exists():
        if not allow_upgrade:
            raise RuntimeError(
                f'检测到已存在安装目录 {dest}。如需保留书库与阅读进度升级，请加上 --upgrade 参数。'
            )
        if (dest / 'state' / 'active').exists():
            raise RuntimeError('检测到 Reeden 会话锁 state/active 仍存在，请先正常退出 Reeden 再升级。')

    _validate_kobo_conf_path(mount)
    keeps = ('userdata', 'storage', 'diagnostics/ssh')
    needed = sum(p.stat().st_size for p in pkg_dir.rglob('*') if p.is_file())
    for keep in keeps:
        if (dest / keep).is_dir():
            needed += sum(p.stat().st_size for p in (dest / keep).rglob('*') if p.is_file())
    if shutil.disk_usage(mount).free < needed + 16 * 1024 * 1024:
        raise RuntimeError('Kobo 剩余空间不足以安全安装并保留旧版备份，未改动原安装。')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    stage = pathlib.Path(tempfile.mkdtemp(prefix='.reeden-kobo-stage-', dir=mount / '.adds'))
    menu_stage = None
    backup = None
    promoted = False
    try:
        menu_fd, menu_name = tempfile.mkstemp(prefix='.reeden-kobo-menu-', dir=nm_dir)
        os.close(menu_fd)
        menu_stage = pathlib.Path(menu_name)
        print(f'[4/4] 正在复制文件到 Kobo ({mount}) ...')
        copytree_fat32_safe(pkg_dir, stage)
        sums_lines = (stage / 'SHA256SUMS').read_text(encoding='utf-8').splitlines()
        print(f'      正在校验 Kobo 磁盘上的全部 {len(sums_lines)} 个安装文件 SHA-256 ...')
        for line in sums_lines:
            if not line.strip():
                continue
            digest, rel_str = line.split('  ', 1)
            rel = pathlib.PurePosixPath(rel_str)
            if rel.is_absolute() or '..' in rel.parts:
                raise ValueError(f'不安全的安装路径: {rel_str}')
            if sha256_file(stage.joinpath(*rel.parts)) != digest:
                raise RuntimeError(f'USB 写入校验失败: {rel_str}')
        # The old installation remains at its original path throughout copying.
        for keep in keeps:
            if (dest / keep).is_dir():
                copytree_fat32_safe(dest / keep, stage / keep)
        shutil.copyfile(stage / 'nickelmenu.conf', menu_stage)
        cleanup_appledouble(stage)
        if dest.exists():
            backup = mount / '.adds' / f'reeden-kobo-backup-{stamp}-{stage.name.rsplit("-", 1)[-1]}'
            dest.rename(backup)
        stage.rename(dest)
        promoted = True
        # Replace the menu only after promotion, in the same filesystem.
        os.replace(menu_stage, entry)
        # Complete pending removable-filesystem writes before reporting success.
        # Windows does not expose os.sync; safe eject remains required there.
        if hasattr(os, 'sync'):
            os.sync()
    except BaseException:
        try:
            if promoted:
                dest.rename(stage)
            if backup is not None and backup.exists():
                backup.rename(dest)
        except OSError as rollback_error:
            raise RuntimeError(f'自动恢复失败；旧版数据保留在 {backup}，新版位于 {dest if promoted else stage}。请勿删除这些目录。') from rollback_error
        shutil.rmtree(stage, ignore_errors=True)
        raise
    finally:
        if menu_stage is not None:
            try:
                menu_stage.unlink(missing_ok=True)
            except OSError:
                print(f'提示：临时菜单文件未能清理: {menu_stage}')
    try:
        cleanup_appledouble(nm_dir / '._reeden-kobo')
    except OSError:
        print('提示：安装已完成，macOS 元数据文件需在弹出前手动清理。')
    try:
        exclude_status = ensure_kobo_exclude_sync_folders(mount)
        if exclude_status in ('added', 'updated') and hasattr(os, 'sync'):
            os.sync()
    except OSError as exc:
        print(f'⚠️  未能自动更新 .kobo/Kobo/Kobo eReader.conf ({exc})。')
        exclude_status = 'manual_required'
    if backup is not None:
        print(f'✅ USB 一键升级完成！原阅读进度、笔记、冷暖光设置和书库已保留（旧版备份于 {backup.name}）。')
    else:
        print(f'✅ USB 一键安装完成！所有 {len(sums_lines)} 个文件已在 Kobo 磁盘上通过 SHA-256 校验。')
    if exclude_status in ('added', 'updated'):
        print('ℹ️  已自动配置 Kobo 隐藏目录忽略规则 (ExcludeSyncFolders)，防止原生书架扫描 Reeden 图标与资源。')
        if backup is not None:
            print('👉 重要提示：若升级前原生书架已扫入 Reeden 图标，请安全弹出并重启一次 Kobo 让规则生效并清理旧条目；如清理时原系统删除了部分已索引资源，重新运行一次一键安装（保留数据升级）即可恢复完整文件。')
        else:
            print('👉 下一步：安全弹出 Kobo USB 盘；如本机是首次配置忽略规则，建议先重启一次 Kobo 让原系统加载配置，再点击右下角 NickelMenu →「Reeden Kobo (test)」启动。')
    elif exclude_status == 'already_configured':
        print('ℹ️  Kobo 隐藏目录忽略规则 (ExcludeSyncFolders) 已就绪，保持原配置不变。')
        print('👉 下一步：安全弹出 Kobo USB 盘，回到 Kobo 原生主界面，点击右下角 NickelMenu →「Reeden Kobo (test)」即可启动。')
    else:
        print(
            '⚠️  检测到自定义且无法自动合并的 ExcludeSyncFolders 配置，未改动 .kobo/Kobo/Kobo eReader.conf。\n'
            f'    为避免原生书架将 Reeden 图标扫成未读图书，请手动确认 [FeatureSettings] 中包含：\n'
            f'    ExcludeSyncFolders={KOBO_EXCLUDE_SYNC_FOLDERS}'
        )
        print('👉 下一步：安全弹出 Kobo USB 盘，回到 Kobo 原生主界面，点击右下角 NickelMenu →「Reeden Kobo (test)」即可启动。')


def install_via_ssh(
    pkg_dir: pathlib.Path,
    host: str,
    port: str,
    allow_upgrade: bool,
    known_hosts: str | None,
    identity: str | None,
    password_auth: bool,
    temp_root: pathlib.Path,
) -> None:
    ssh_base = ['ssh', '-p', str(port), '-o', 'ConnectTimeout=10']
    if known_hosts:
        ssh_base += [
            '-o', 'StrictHostKeyChecking=yes',
            '-o', f'UserKnownHostsFile={pathlib.Path(known_hosts).expanduser().resolve()}',
        ]
    else:
        ssh_base += ['-o', 'StrictHostKeyChecking=accept-new']
    if identity:
        ssh_base += ['-i', str(pathlib.Path(identity).expanduser().resolve())]
    if password_auth:
        ssh_base += [
            '-o', 'PreferredAuthentications=password',
            '-o', 'PubkeyAuthentication=no',
            '-o', 'NumberOfPasswordPrompts=1',
        ]
    target = host if '@' in host else f'root@{host}'
    ssh_read = ssh_base + ['-n', target]
    ssh_stream = ssh_base + [target]

    # Pre-check remote device state before packing/streaming 78MB archive
    probe_cmd = (
        'if [ ! -d /mnt/onboard/.adds/nm ]; then echo ERR_NO_NM; '
        'elif [ -d /mnt/onboard/.adds/reeden-kobo/state/active ]; then echo ERR_ACTIVE; '
        'elif [ -e /mnt/onboard/.adds/reeden-kobo ] || [ -e /mnt/onboard/.adds/nm/reeden-kobo ]; then echo HAS_EXISTING; '
        'else echo READY_FRESH; fi'
    )
    probe = subprocess.run(ssh_read + [probe_cmd], text=True, capture_output=True, check=True)
    state_lines = [ln.strip() for ln in probe.stdout.splitlines() if ln.strip() in ('ERR_NO_NM', 'ERR_ACTIVE', 'HAS_EXISTING', 'READY_FRESH')]
    remote_state = state_lines[-1] if state_lines else 'UNKNOWN'
    if remote_state == 'ERR_NO_NM':
        raise RuntimeError('Kobo 上未检测到 NickelMenu (/mnt/onboard/.adds/nm)，请先安装 NickelMenu。')
    if remote_state == 'ERR_ACTIVE':
        raise RuntimeError('检测到 Kobo 上 Reeden 正在运行（state/active 存在）。请先在 Kobo 上同时按住左上角+右上角 2 秒退出 Reeden，再执行升级。')
    if remote_state == 'HAS_EXISTING' and not allow_upgrade:
        raise RuntimeError('检测到 Kobo 上已安装旧版 reeden-kobo。如需保留书库与阅读进度升级，请加上 --upgrade 参数。')

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    remote_stage = f'/mnt/onboard/.adds/.reeden-kobo-stage-{stamp}'
    archive = temp_root / f'transfer-{stamp}.tar.gz'
    print(f'[4/4] 正在打包并经由 SSH ({target}:{port}) 推送到 Kobo ...')
    with tarfile.open(archive, 'w:gz', compresslevel=6) as tf:
        for path in sorted(pkg_dir.iterdir()):
            tf.add(path, arcname=path.name, recursive=True)

    subprocess.run(ssh_read + [f'mkdir {remote_stage}'], check=True)
    with archive.open('rb') as f:
        subprocess.run(ssh_stream + [f'tar -xzf - -C {remote_stage}'], stdin=f, check=True)
    install_cmd = f'sh {remote_stage}/install.sh' + (' --upgrade' if allow_upgrade else '')
    result = subprocess.run(ssh_read + [install_cmd], text=True, capture_output=True)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    print('✅ SSH 一键安装完成（未自动启动应用）。请回到 Kobo 原生主界面，从 NickelMenu 点击「Reeden Kobo (test)」启动。')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--official-core',
        help='官方 Reeden 安装包路径 (支持 1.43.2+791 或 1.43.1+780；支持直接传 Reeden-kindle.zip 或解压后的 reeden/reeden 目录；省略时自动搜索)',
    )
    parser.add_argument(
        '--download-official',
        action='store_true',
        help='强制从官网下载并校验官方 Kindle 包（不使用本地候选，与 --official-core 互斥）',
    )
    parser.add_argument(
        '--output',
        help='在电脑本地输出完整 package 目录的路径（如传此参数且未指定 --usb / --ssh，则仅在本地生成安装包）',
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        '--usb',
        action='store_true',
        help='自动查找通过 USB 挂载的 KoboClaraColour (KOBOeReader) 并直接一键安装',
    )
    mode.add_argument(
        '--kobo-mount',
        help='手动指定 Kobo USB 挂载根目录（例如 /Volumes/KOBOeReader 或 E:\\）并一键安装',
    )
    mode.add_argument('--ssh', metavar='HOST', help='通过 Wi-Fi SSH 一键安装到指定 Kobo IP（例如 192.168.1.100）')
    parser.add_argument('--port', default='2222', help='SSH 端口（KOReader SSH 默认为 2222，Reeden 独立 SSH 为 2223）')
    parser.add_argument('--known-hosts', help='可选 SSH known_hosts 文件路径（启用严格主机密钥校验）')
    parser.add_argument('--identity', help='可选 SSH 私钥文件路径')
    parser.add_argument('--password-auth', action='store_true', help='使用 SSH 密码认证')
    parser.add_argument(
        '--upgrade',
        action='store_true',
        help='若设备已安装旧版 reeden-kobo，自动备份旧版并保留 userdata/ 与 storage/ 完成升级',
    )
    args = parser.parse_args()

    prebuilt_dir = verify_prebuilt(ROOT)
    with tempfile.TemporaryDirectory(prefix='reeden-kobo-oneclick-') as tmp:
        temp_root = pathlib.Path(tmp)
        core_dir, official_version = resolve_official_core(
            args.official_core,
            temp_root,
            auto_download=args.download_official,
        )

        # Determine output directory if requested
        if args.output:
            out_dir = pathlib.Path(args.output).expanduser().resolve()
            if out_dir.exists():
                parser.error(f'输出目录已存在，为保护现有文件已拒绝覆盖: {out_dir}')
            pkg_dir = out_dir
        else:
            pkg_dir = temp_root / 'package'

        file_count = assemble_package(core_dir, official_version, prebuilt_dir, pkg_dir, temp_root)
        print(f'      已组出 {file_count} 个已校验文件（加上 SHA256SUMS 共 {file_count + 1} 个文件）。')

        # Explicit CLI mode selection
        if args.kobo_mount:
            mount = pathlib.Path(args.kobo_mount).expanduser().resolve()
            install_to_usb_mount(pkg_dir, mount, allow_upgrade=args.upgrade)
            return 0
        if args.usb:
            mounts = detect_kobo_usb_mounts()
            if not mounts:
                raise RuntimeError('未检测到通过 USB 挂载的 Kobo 设备，请确认 Kobo 已连接电脑并处于 USB 传书模式。')
            install_to_usb_mount(pkg_dir, choose_kobo_usb_mount(mounts), allow_upgrade=args.upgrade)
            return 0
        if args.ssh:
            install_via_ssh(
                pkg_dir,
                host=args.ssh,
                port=args.port,
                allow_upgrade=args.upgrade,
                known_hosts=args.known_hosts,
                identity=args.identity,
                password_auth=args.password_auth,
                temp_root=temp_root,
            )
            return 0
        if args.output:
            print(f'[4/4] 已在本地生成验证通过的完整安装包: {pkg_dir}（未连接或改动设备）。')
            return 0

        # Interactive one-click flow when run with no mode arguments
        mounts = detect_kobo_usb_mounts()
        if mounts:
            mount = choose_kobo_usb_mount(mounts)
            has_existing = (mount / '.adds' / 'reeden-kobo').exists()
            mode_label = '保留书库与进度升级' if has_existing else '全新安装'
            if sys.stdin.isatty():
                ans = input(f'\n检测到已通过 USB 挂载的 Kobo ({mount})，按回车立即【{mode_label}】(输入 n 取消): ').strip().lower()
                if ans in ('n', 'no', 'q'):
                    print('已取消设备安装。')
                    return 0
            install_to_usb_mount(pkg_dir, mount, allow_upgrade=True if has_existing else args.upgrade)
            return 0

        if sys.stdin.isatty():
            print('\n当前未检测到 USB 挂载的 Kobo，请选择安装方式：')
            print('  [1] 我现在插上 Kobo USB 数据线，连接后按回车一键安装（推荐，免 SSH）')
            print('  [2] 通过 Wi-Fi SSH 无线一键安装（需在 Kobo 开启 KOReader SSH 或 Reeden SSH）')
            print('  [3] 仅在当前目录生成 package/ 安装包文件夹')
            choice = input('请输入序号 [1/2/3，默认 1]: ').strip() or '1'
            if choice == '1':
                input('请用 USB 线将 Kobo 连接到电脑并点击 Kobo 屏幕上的「连接」，电脑出现 KOBOeReader 盘后按回车...')
                mounts = detect_kobo_usb_mounts()
                if not mounts:
                    raise RuntimeError('仍未检测到 KOBOeReader USB 盘，请确认连接后重试。')
                mount = choose_kobo_usb_mount(mounts)
                has_existing = (mount / '.adds' / 'reeden-kobo').exists()
                install_to_usb_mount(pkg_dir, mount, allow_upgrade=True if has_existing else args.upgrade)
                return 0
            elif choice == '2':
                host = input('请输入 Kobo 的局域网 IP 地址 (如 192.168.1.100): ').strip()
                port = input('请输入 SSH 端口 [KOReader 默认 2222 / Reeden SSH 填 2223]: ').strip() or '2222'
                install_via_ssh(
                    pkg_dir,
                    host=host,
                    port=port,
                    allow_upgrade=True,
                    known_hosts=args.known_hosts,
                    identity=args.identity,
                    password_auth=args.password_auth,
                    temp_root=temp_root,
                )
                return 0

        fallback_out = ROOT / 'package'
        if fallback_out.exists():
            stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
            fallback_out = ROOT / f'package-{stamp}'
        shutil.copytree(pkg_dir, fallback_out)
        print(f'[4/4] 已在本地生成验证通过的完整安装包: {fallback_out}')
        return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(f'\n❌ 错误: {exc}', file=sys.stderr)
        sys.exit(1)
