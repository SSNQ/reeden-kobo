#!/usr/bin/env python3
"""One-click update and official download retries, using synthetic files only."""
from contextlib import ExitStack, redirect_stdout, redirect_stderr
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class UpdateTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='reeden-update-')
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name)
        self.mount = self.base / 'Kobo USB'
        (self.mount / '.kobo').mkdir(parents=True)
        (self.mount / '.adds/nm').mkdir(parents=True)
        self.version = self.mount / '.kobo/version'
        self.version.write_text('PRIVATE-SERIAL,4.9.77,4.46.23836,000-393\n', encoding='utf-8')
        self.installed = self.mount / '.adds/reeden-kobo'
        for name, data in {'userdata/progress': b'page-42', 'storage/book.epub': b'book',
                           'userdata/kobo-light.conf': b'warmth=60', 'diagnostics/ssh/key': b'test-key',
                           'payload': b'old-version'}.items():
            file = self.installed / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        self.after_build = lambda: None

    def assemble(self, core, version, prebuilt, dest, temp):
        dest.mkdir()
        (dest / 'payload').write_bytes(b'new-version')
        (dest / 'nickelmenu.conf').write_bytes(b'new-menu\n')
        (dest / 'SHA256SUMS').write_text(''.join(
            f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
            for p in sorted(dest.iterdir())), encoding='utf-8')
        self.after_build()
        return 2

    def run_main(self, args, scans=None, answers=None):
        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(installer.sys, 'argv', ['install.py', *args]))
            stack.enter_context(mock.patch.object(installer, 'detect_kobo_usb_mounts', return_value=[self.mount], side_effect=scans))
            self.verify = stack.enter_context(mock.patch.object(installer, 'verify_prebuilt', return_value=self.base))
            self.resolve = stack.enter_context(mock.patch.object(installer, 'resolve_official_core', return_value=(self.base, 'test')))
            stack.enter_context(mock.patch.object(installer, 'assemble_package', side_effect=self.assemble))
            self.write = stack.enter_context(mock.patch.object(installer, 'install_to_usb_mount', wraps=installer.install_to_usb_mount))
            stack.enter_context(mock.patch('builtins.input', side_effect=answers if answers is not None
                                          else AssertionError('unexpected prompt')))
            self.output = io.StringIO()
            stack.enter_context(redirect_stdout(self.output))
            stack.enter_context(redirect_stderr(self.output))
            return installer.main()

    def test_update_automates_flags_and_preserves_data_and_backup(self):
        self.assertEqual(self.run_main(['--update']), 0)
        self.assertFalse(self.resolve.call_args.kwargs['auto_download'])
        self.assertTrue(self.resolve.call_args.kwargs['download_missing'])
        self.assertTrue(self.write.call_args.kwargs['allow_upgrade'])
        self.assertEqual((self.installed / 'payload').read_bytes(), b'new-version')
        backups = list((self.mount / '.adds').glob('reeden-kobo-backup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'payload').read_bytes(), b'old-version')
        for name in ['userdata/progress', 'storage/book.epub', 'userdata/kobo-light.conf', 'diagnostics/ssh/key']:
            self.assertEqual((self.installed / name).read_bytes(), (backups[0] / name).read_bytes())
        self.assertNotIn('PRIVATE-SERIAL', self.output.getvalue())

    def test_interactive_update_waits_for_device(self):
        with mock.patch.object(installer.sys.stdin, 'isatty', return_value=True), mock.patch.object(installer.time, 'sleep') as sleep:
            self.assertEqual(self.run_main(['--update'], [[], [self.mount], [self.mount]]), 0)
            sleep.assert_called_once_with(2)

    def test_cancel_never_writes_present_or_late_device(self):
        with mock.patch.object(installer.sys.stdin, 'isatty', return_value=True):
            for answer in ('取消', 'no', 'typo'):
                self.assertEqual(self.run_main([], answers=[answer]), 0)
                self.write.assert_not_called()
            self.assertEqual(self.run_main([], scans=[[], []], answers=['1', '取消']), 0)
            self.write.assert_not_called()

    def test_custom_menu_is_preserved_and_stock_menu_is_updated(self):
        entry = self.mount / '.adds/nm/reeden-kobo'
        (self.installed / 'nickelmenu.conf').write_bytes(b'old stock menu')
        entry.write_bytes(b'my commands')
        self.assertEqual(self.run_main(['--update']), 0)
        self.assertEqual(entry.read_bytes(), b'my commands')
        self.assertEqual((self.installed / 'nickelmenu.conf').read_bytes(), b'new-menu\n')
        (self.installed / 'nickelmenu.conf').write_bytes(b'old stock menu')
        entry.write_bytes(b'old stock menu')
        self.assertEqual(self.run_main(['--update']), 0)
        self.assertEqual(entry.read_bytes(), b'new-menu\n')

    def test_unreadable_menu_is_kept_without_rolling_back_the_install(self):
        entry = self.mount / '.adds/nm/reeden-kobo'
        entry.write_bytes(b'my commands')
        (self.installed / 'nickelmenu.conf').write_bytes(b'old stock menu')
        real_sha256 = installer.sha256_file

        def unreadable_menu(path, *args, **kwargs):
            if Path(path) == entry:
                raise PermissionError(13, 'permission denied')
            return real_sha256(path, *args, **kwargs)

        with mock.patch.object(installer, 'sha256_file', side_effect=unreadable_menu):
            self.assertEqual(self.run_main(['--update']), 0)
        # A menu that cannot be read is kept, and the verified install still lands.
        self.assertEqual(entry.read_bytes(), b'my commands')
        self.assertEqual((self.installed / 'payload').read_bytes(), b'new-version')
        self.assertIn('无法读取现有菜单', self.output.getvalue())

    def test_update_timeout_and_instant_check(self):
        for args, waits in [(['--update'], 30), (['--check'], 0)]:
            with mock.patch.object(installer.sys.stdin, 'isatty', return_value=True), mock.patch.object(installer.time, 'sleep') as sleep:
                with self.assertRaises(RuntimeError):
                    self.run_main(args, [[]] * (waits + 1))
                self.assertEqual(sleep.call_count, waits)
                self.resolve.assert_not_called()
                self.write.assert_not_called()

    def test_check_explains_next_step(self):
        for expected in ['一键更新', '一键安装']:
            self.assertEqual(self.run_main(['--check']), 0)
            self.assertIn(expected, self.output.getvalue())
            self.write.assert_not_called()
            if self.installed.exists():
                self.installed.rename(self.base / 'previous')

    def test_update_supports_explicit_mount_and_official_core(self):
        self.assertEqual(self.run_main(['--update', '--kobo-mount', str(self.mount), '--official-core', 'chosen.zip']), 0)
        self.assertEqual(self.resolve.call_args.args[0], 'chosen.zip')

    def test_update_rejects_fresh_device_before_package_preparation(self):
        # Keep all fixture files as a backup; only remove the installation name.
        self.installed.rename(self.mount / '.adds/old-backup')
        with self.assertRaisesRegex(RuntimeError, '一键安装'):
            self.run_main(['--update'])
        self.verify.assert_not_called()
        self.write.assert_not_called()

    def test_update_rejects_incompatible_modes(self):
        for args in [['--check'], ['--ssh', '192.0.2.1'], ['--output', 'unused']]:
            with self.subTest(args=args):
                with self.assertRaises(SystemExit) as result:
                    self.run_main(['--update', *args])
                self.assertEqual(result.exception.code, 2)
                self.verify.assert_not_called()

    def test_replaced_device_on_same_mount_is_not_updated(self):
        def replace():
            self.version.write_text('OTHER-DEVICE,4.9.77,4.46.23836,000-393\n', encoding='utf-8')
        self.after_build = replace
        with self.assertRaisesRegex(RuntimeError, '更新目标已变化'):
            self.run_main(['--update'])
        self.write.assert_not_called()
        self.assertEqual((self.installed / 'payload').read_bytes(), b'old-version')

    def test_missing_installation_after_build_is_not_reinstalled(self):
        self.after_build = lambda: self.installed.rename(self.mount / '.adds/moved-by-test')
        with self.assertRaisesRegex(RuntimeError, '更新目标已变化'):
            self.run_main(['--update'])
        self.write.assert_not_called()

    def test_active_session_after_build_is_rejected(self):
        self.after_build = lambda: (self.installed / 'state/active').mkdir(parents=True)
        with self.assertRaisesRegex(RuntimeError, 'state/active'):
            self.run_main(['--update'])
        self.assertEqual((self.installed / 'payload').read_bytes(), b'old-version')


class UpdateCoreSelectionTests(unittest.TestCase):
    def test_download_only_when_no_verified_local_copy(self):
        for found in (True, False):
            with self.subTest(local=found), ExitStack() as stack:
                core = (Path('fixture'), '1.43.2+791')
                stack.enter_context(mock.patch.object(installer, 'discover_official_inputs',
                                                      return_value=[Path('local.zip')] if found else []))
                stack.enter_context(mock.patch.object(installer, 'load_official_input', return_value=core))
                download = stack.enter_context(mock.patch.object(installer, 'download_official_core', return_value=core))
                stack.enter_context(mock.patch('builtins.input', side_effect=AssertionError('unexpected prompt')))
                self.assertEqual(installer.resolve_official_core(None, Path('.'), download_missing=True), core)
                self.assertEqual(download.call_count, int(not found))

    def test_explicit_bad_core_never_falls_back(self):
        with mock.patch.object(installer, 'load_official_input', side_effect=ValueError('bad core')), \
             mock.patch.object(installer, 'download_official_core') as download:
            with self.assertRaisesRegex(ValueError, 'bad core'):
                installer.resolve_official_core('chosen.zip', Path('.'), download_missing=True)
            download.assert_not_called()


if __name__ == '__main__':
    unittest.main()
