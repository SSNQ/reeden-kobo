#!/usr/bin/env python3
"""Official input selection, immutable cache and resource compatibility regressions.

All default fixtures are synthetic; no official core is distributed here.
"""
import hashlib
import http.client
import http.server
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock
import urllib.error
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import install


class Response(io.BytesIO):
    def __init__(self, data, size=None):
        super().__init__(data)
        self.headers = {'Content-Length': str(len(data) if size is None else size)}


class OfficialCoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='reeden-official-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.base / 'repo'
        (self.root / 'vendor').mkdir(parents=True)
        self.extract = self.base / 'extract'
        self.extract.mkdir()
        self.core = self.base / 'core'
        files = {
            'lib/libapp.so': b'synthetic-app',
            'lib/libflutter_engine.so': b'synthetic-engine',
            'lib/glibc/reeden-kindle': b'synthetic-loader',
            'lib/glibc/libc.so.6': b'synthetic-libc',
            'THIRD_PARTY_NOTICES.txt': b'synthetic-notices',
            'lib/glibc/ld-linux.so': b'synthetic-runtime',
            'licenses/NOTICE': b'synthetic-license',
            'data/icudtl.dat': b'synthetic-icu',
            'data/flutter_assets/AssetManifest.bin': b'synthetic-assets',
            'data/flutter_assets/FontManifest.json': b'[{"family":"Test","fonts":[{"asset":"assets/test.ttf"}]}]',
            'data/flutter_assets/assets/test.ttf': b'synthetic-font',
            'data/flutter_assets/version.json': b'{"version":"1.43.2","build_number":"791"}',
        }
        for rel, data in files.items():
            p = self.core / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        self.hashes = {n: install.sha256_file(self.core / 'lib' / n)
                       for n in ('libapp.so', 'libflutter_engine.so')}
        self.manifest = self.root / 'vendor/official-core-1.43.2+791.sha256'
        self.manifest.write_text(''.join(f'{hashlib.sha256(data).hexdigest()}  {rel}\n'
                                         for rel, data in sorted(files.items())), encoding='utf-8')
        self.valid_zip = self.base / 'valid.zip'
        self.make_zip(self.valid_zip)
        self.cache = self.root / 'cache/Reeden-kindle.zip'
        self.cache.parent.mkdir()
        self.cache.write_bytes(b'previous-cache-must-survive')
        for patch in (mock.patch.object(install, 'ROOT', self.root),
                      mock.patch.object(install, 'SUPPORTED_CORES', {'1.43.2+791': self.hashes}),
                      mock.patch.object(install.time, 'sleep'),
                      mock.patch.object(install.sys.stdin, 'isatty', return_value=False)):
            patch.start()
            self.addCleanup(patch.stop)

    def make_zip(self, path, prefix='reeden/reeden/'):
        with zipfile.ZipFile(path, 'w') as z:
            for p in self.core.rglob('*'):
                if p.is_file():
                    z.write(p, prefix + p.relative_to(self.core).as_posix())

    def assert_old_cache(self):
        self.assertEqual(self.cache.read_bytes(), b'previous-cache-must-survive')
        self.assertEqual(list(self.cache.parent.glob('*.part')), [])

    def test_full_manifest_accepts_pristine_core_and_rejects_all_resource_changes(self):
        self.assertEqual(install.validate_official_core(self.core), '1.43.2+791')
        for rel in ('data/icudtl.dat', 'lib/glibc/ld-linux.so', 'licenses/NOTICE'):
            p = self.core / rel
            data = p.read_bytes()
            p.write_bytes(b'changed')
            with self.subTest(rel=rel), self.assertRaisesRegex(ValueError, '哈希'):
                install.validate_official_core(self.core)
            p.write_bytes(data)
        extra = self.core / 'data/unexpected'
        extra.write_bytes(b'extra')
        with self.assertRaisesRegex(ValueError, '集合'):
            install.validate_official_core(self.core)
        extra.unlink()
        (self.core / 'licenses/NOTICE').unlink()
        with self.assertRaisesRegex(ValueError, '集合'):
            install.validate_official_core(self.core)

    def test_missing_manifest_never_weakens_validation(self):
        self.manifest.unlink()
        with self.assertRaises(FileNotFoundError):
            install.validate_official_core(self.core)

    def test_finder_metadata_recommends_original_zip_without_weakening_manifest(self):
        (self.core / 'data/.DS_Store').write_bytes(b'metadata')
        with self.assertRaisesRegex(ValueError, 'Reeden-kindle.zip'):
            install.validate_official_core(self.core)
        _, version = install.load_official_input(self.valid_zip, self.extract)
        self.assertEqual(version, '1.43.2+791')

    def test_invalid_candidates_do_not_hide_valid_later_zip_or_mix_extractions(self):
        corrupt = self.base / 'corrupt.zip'
        corrupt.write_bytes(b'PK truncated')
        unknown = self.base / 'unknown.zip'
        (self.core / 'data/extra').write_bytes(b'extra')
        self.make_zip(unknown)
        with mock.patch.object(install, 'discover_official_inputs', return_value=[corrupt, unknown, self.valid_zip]):
            core, version = install.resolve_official_core(None, self.extract)
        self.assertEqual(version, '1.43.2+791')
        self.assertFalse((core / 'data/extra').exists())
        self.assertEqual(len(list(self.extract.iterdir())), 1)

    def test_real_http_truncation_then_success_never_exposes_half_cache(self):
        payload = self.valid_zip.read_bytes()
        observed_cache = []
        cache = self.cache

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                observed_cache.append(cache.read_bytes())
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload[:100] if len(observed_cache) == 1 else payload)
                self.close_connection = True

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with mock.patch.object(install, 'OFFICIAL_LATEST_URL',
                                   f'http://127.0.0.1:{server.server_port}/official.zip'):
                install.download_official_core(self.extract)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
        self.assertEqual(observed_cache, [b'previous-cache-must-survive'] * 2)
        self.assertEqual(self.cache.read_bytes(), payload)
        self.assertEqual(list(self.cache.parent.glob('*.part')), [])

    def test_corrupt_deflate_stream_does_not_hide_valid_candidate(self):
        broken = self.base / 'broken-deflate.zip'
        with zipfile.ZipFile(broken, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr('reeden/reeden/lib/libapp.so', b'synthetic-app' * 100)
        with zipfile.ZipFile(broken) as z:
            info = z.infolist()[0]
            offset = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
        raw = bytearray(broken.read_bytes())
        raw[offset:offset + info.compress_size] = b'\xff' * info.compress_size
        broken.write_bytes(raw)
        with mock.patch.object(install, 'discover_official_inputs', return_value=[broken, self.valid_zip]):
            core, version = install.resolve_official_core(None, self.extract)
        self.assertEqual(version, '1.43.2+791')
        self.assertTrue(core.is_dir())
        self.assertEqual(len(list(self.extract.iterdir())), 1)

    def test_explicit_bad_input_does_not_silently_fall_back(self):
        with mock.patch.object(install, 'discover_official_inputs') as discover:
            with self.assertRaises(zipfile.BadZipFile):
                install.resolve_official_core(str(self.cache), self.extract)
            discover.assert_not_called()

    def test_force_download_ignores_discovery_and_replaces_cache_after_validation(self):
        payload = self.valid_zip.read_bytes()
        with mock.patch.object(install, 'discover_official_inputs') as discover, \
                mock.patch.object(install, 'open_url_with_cert_fallback', return_value=Response(payload)) as network:
            core, version = install.resolve_official_core(None, self.extract, auto_download=True)
            discover.assert_not_called()
            self.assertEqual(network.call_count, 1)
        self.assertEqual(version, '1.43.2+791')
        self.assertTrue(core.is_dir())
        self.assertEqual(self.cache.read_bytes(), payload)
        self.assertEqual(list(self.cache.parent.glob('*.part')), [])

    def test_truncated_response_retries_but_preserves_old_cache(self):
        with mock.patch.object(install, 'open_url_with_cert_fallback',
                               side_effect=lambda *a, **kw: Response(b'PK half', size=200)) as network:
            with self.assertRaisesRegex(RuntimeError, '原缓存未更改'):
                install.resolve_official_core(None, self.extract, auto_download=True)
        self.assertEqual(network.call_count, 3)
        self.assert_old_cache()
        self.assertEqual(list(self.extract.iterdir()), [])

    def test_transient_network_failure_then_success(self):
        with mock.patch.object(install, 'open_url_with_cert_fallback', side_effect=[
                http.client.IncompleteRead(b'half', 40), Response(self.valid_zip.read_bytes())]) as network:
            install.download_official_core(self.extract)
        self.assertEqual(network.call_count, 2)
        self.assertEqual(self.cache.read_bytes(), self.valid_zip.read_bytes())

    def test_unsupported_latest_fails_closed_without_replacing_or_using_old_cache(self):
        (self.core / 'lib/libapp.so').write_bytes(b'new-unsupported-core')
        unknown = self.base / 'new.zip'
        self.make_zip(unknown)
        with mock.patch.object(install, 'open_url_with_cert_fallback', return_value=Response(unknown.read_bytes())) as network:
            with self.assertRaisesRegex(ValueError, '白名单'):
                install.resolve_official_core(None, self.extract, auto_download=True)
        self.assertEqual(network.call_count, 1)
        self.assert_old_cache()
        self.assertEqual(list(self.extract.iterdir()), [])

    def test_failed_cache_commit_cleans_only_its_extraction(self):
        # A ZIP with the core at its root exercises the cleanup boundary.
        self.make_zip(self.valid_zip, prefix='')
        sentinel = self.extract / 'unrelated'
        sentinel.write_bytes(b'keep')
        with mock.patch.object(install, 'open_url_with_cert_fallback',
                               side_effect=lambda *a, **kw: Response(self.valid_zip.read_bytes())), \
                mock.patch.object(install.os, 'replace', side_effect=OSError('read-only cache')):
            with self.assertRaises(RuntimeError):
                install.download_official_core(self.extract)
        self.assertEqual(sentinel.read_bytes(), b'keep')
        self.assertEqual(list(self.extract.iterdir()), [sentinel])
        self.assert_old_cache()

    def test_discovery_is_deduplicated_and_excludes_half_downloads(self):
        (self.root / 'Reeden-kindle.zip').write_bytes(b'invalid')
        (self.root / 'Reeden-kindle (1).zip').write_bytes(b'valid')
        (self.root / 'Reeden-kindle-123.part').write_bytes(b'half')
        home = self.base / 'home'
        home.mkdir()
        with mock.patch.object(install.pathlib.Path, 'home', return_value=home), \
                mock.patch.object(install.pathlib.Path, 'cwd', return_value=self.root), \
                mock.patch.dict(os.environ, {'REEDEN_OFFICIAL_CORE': ''}):
            paths = install.discover_official_inputs(self.root)
        self.assertEqual(len(paths), 3)
        self.assertEqual(len(set(paths)), 3)
        self.assertTrue(all(p.suffix == '.zip' for p in paths))

    def test_780_retains_library_allowlist_and_checks_version_and_fonts(self):
        version = self.core / 'data/flutter_assets/version.json'
        version.write_text('{"version":"1.43.1","build_number":"780"}', encoding='utf-8')
        with mock.patch.object(install, 'SUPPORTED_CORES', {'1.43.1+780': self.hashes}):
            self.assertEqual(install.validate_official_core(self.core), '1.43.1+780')
            for invalid in ('[]', 'null', '{"version":"1.43.2","build_number":"791"}'):
                version.write_text(invalid, encoding='utf-8')
                with self.subTest(metadata=invalid), self.assertRaisesRegex(ValueError, '版本'):
                    install.validate_official_core(self.core)
            version.write_text('{"version":"1.43.1","build_number":"780"}', encoding='utf-8')
            (self.core / 'data/flutter_assets/assets/test.ttf').unlink()
            with self.assertRaisesRegex(ValueError, '字体'):
                install.validate_official_core(self.core)

    def use_780(self):
        (self.core / 'data/flutter_assets/version.json').write_text(
            '{"version":"1.43.1","build_number":"780"}', encoding='utf-8')
        patch = mock.patch.object(install, 'SUPPORTED_CORES', {'1.43.1+780': self.hashes})
        patch.start()
        self.addCleanup(patch.stop)

    def assert_candidate_skipped(self):
        with mock.patch.object(install, 'discover_official_inputs',
                               return_value=[self.core, self.valid_zip]):
            core, version = install.resolve_official_core(None, self.extract)
        self.assertEqual(version, '1.43.1+780')
        self.assertNotEqual(core, self.core)
        self.assertEqual(len(list(self.extract.iterdir())), 1)

    def test_780_missing_or_empty_assembly_files_do_not_hide_valid_candidate(self):
        self.use_780()
        self.make_zip(self.valid_zip)
        for rel in ('THIRD_PARTY_NOTICES.txt', 'lib/glibc/reeden-kindle',
                    'lib/glibc/libc.so.6'):
            path = self.core / rel
            original = path.read_bytes()
            for missing in (True, False):
                with self.subTest(path=rel, missing=missing):
                    if missing:
                        path.unlink()
                    else:
                        path.write_bytes(b'')
                    with self.assertRaisesRegex(ValueError, '必需文件'):
                        install.validate_official_core(self.core)
                    self.assert_candidate_skipped()
                    for extracted in self.extract.iterdir():
                        install.shutil.rmtree(extracted)
                    path.write_bytes(original)

    def test_780_malformed_font_shapes_do_not_abort_candidate_search(self):
        self.use_780()
        self.make_zip(self.valid_zip)
        path = self.core / 'data/flutter_assets/FontManifest.json'
        for malformed in (None, {}, 7, [None], [{}], [{'fonts': None}],
                          [{'fonts': [None]}], [{'fonts': [{'asset': None}]}],
                          [{'fonts': [{'asset': 7}]}]):
            with self.subTest(manifest=malformed):
                path.write_text(json.dumps(malformed), encoding='utf-8')
                with self.assertRaisesRegex(ValueError, '字体'):
                    install.validate_official_core(self.core)
                self.assert_candidate_skipped()
                for extracted in self.extract.iterdir():
                    install.shutil.rmtree(extracted)

    def test_conflicting_explicit_options_fail(self):
        with self.assertRaisesRegex(ValueError, '不能同时'):
            install.resolve_official_core(str(self.valid_zip), self.extract, auto_download=True)


if __name__ == '__main__':
    unittest.main()
