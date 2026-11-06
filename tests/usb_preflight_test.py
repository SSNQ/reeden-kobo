#!/usr/bin/env python3
"""USB preflight must fail before downloads and never write in --check mode."""
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('installer', ROOT / 'install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class BuildReached(Exception):
    pass


class UsbPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='reeden-preflight-')
        self.addCleanup(self.temporary.cleanup)
        self.mount = Path(self.temporary.name) / 'Kobo USB'
        (self.mount / '.kobo').mkdir(parents=True)
        (self.mount / '.adds/nm').mkdir(parents=True)
        self.version = self.mount / '.kobo/version'
        self.version.write_text('PRIVATE-SERIAL,4.9.77,4.46.23836,000-393\n', encoding='utf-8')

    def run_main(self, args, mounts=None):
        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(installer.sys, 'argv', ['install.py', *args]))
            self.discovery = stack.enter_context(mock.patch.object(
                installer, 'detect_kobo_usb_mounts', return_value=[self.mount] if mounts is None else mounts))
            self.verify = stack.enter_context(mock.patch.object(
                installer, 'verify_prebuilt', side_effect=BuildReached('package preparation started')))
            for name in ('resolve_official_core', 'assemble_package', 'install_via_ssh'):
                stack.enter_context(mock.patch.object(installer, name, side_effect=AssertionError(name)))
            self.output = io.StringIO()
            stack.enter_context(redirect_stdout(self.output))
            stack.enter_context(redirect_stderr(self.output))
            return installer.main()

    def snapshot(self):
        return {p.relative_to(self.mount).as_posix(): p.read_bytes() if p.is_file() else None
                for p in self.mount.rglob('*')}

    def test_check_does_not_prepare_packages_or_modify_device(self):
        before = self.snapshot()
        self.assertEqual(self.run_main(['--check']), 0)
        self.verify.assert_not_called()
        self.assertEqual(self.snapshot(), before)
        self.assertIn('393', self.output.getvalue())
        self.assertIn('4.46.23836', self.output.getvalue())
        self.assertNotIn('PRIVATE-SERIAL', self.output.getvalue())

    def test_check_accepts_an_existing_install_without_upgrade_flag(self):
        (self.mount / '.adds/reeden-kobo/userdata').mkdir(parents=True)
        progress = self.mount / '.adds/reeden-kobo/userdata/progress'
        progress.write_bytes(b'reading-progress')
        before = self.snapshot()
        self.assertEqual(self.run_main(['--check', '--kobo-mount', str(self.mount)]), 0)
        self.discovery.assert_not_called()
        self.assertEqual(self.snapshot(), before)

    def test_missing_or_multiple_devices_fail_before_build(self):
        for args in (['--usb'], ['--check']):
            for mounts in ([], [self.mount, self.mount / 'another']):
                with self.subTest(args=args, mounts=mounts):
                    with self.assertRaises(RuntimeError):
                        self.run_main(args, mounts)
                    self.verify.assert_not_called()

    def test_wrong_model_fails_before_build_in_every_usb_entry(self):
        self.version.write_text('PRIVATE-SERIAL,4.9.77,4.46.23836,000-999\n', encoding='utf-8')
        before = self.snapshot()
        for args in ([], ['--usb'], ['--kobo-mount', str(self.mount)], ['--check']):
            with self.subTest(args=args):
                with self.assertRaisesRegex(RuntimeError, '999'):
                    self.run_main(args)
                self.verify.assert_not_called()
                self.assertEqual(self.snapshot(), before)

    def test_missing_nickelmenu_fails_before_build(self):
        (self.mount / '.adds/nm').rmdir()
        with self.assertRaisesRegex(RuntimeError, 'NickelMenu'):
            self.run_main(['--usb'])
        self.verify.assert_not_called()

    def test_active_session_fails_even_for_check_or_automatic_upgrade(self):
        (self.mount / '.adds/reeden-kobo/state/active').mkdir(parents=True)
        for args in ([], ['--check'], ['--usb', '--upgrade']):
            with self.subTest(args=args):
                with self.assertRaisesRegex(RuntimeError, 'state/active'):
                    self.run_main(args)
                self.verify.assert_not_called()

    def test_explicit_upgrade_requires_flag_but_interactive_flow_keeps_existing_behavior(self):
        (self.mount / '.adds/reeden-kobo').mkdir()
        with self.assertRaisesRegex(RuntimeError, '--upgrade'):
            self.run_main(['--usb'])
        self.verify.assert_not_called()
        for args in ([], ['--usb', '--upgrade']):
            with self.subTest(args=args):
                with self.assertRaises(BuildReached):
                    self.run_main(args)

    def test_fresh_usb_install_still_reaches_build(self):
        with self.assertRaises(BuildReached):
            self.run_main(['--usb'])

    def test_default_flow_can_still_prepare_before_connecting_a_device(self):
        with self.assertRaises(BuildReached):
            self.run_main([], mounts=[])

    def test_existing_menu_without_install_requires_explicit_upgrade(self):
        (self.mount / '.adds/nm/reeden-kobo').write_bytes(b'custom-menu')
        with self.assertRaisesRegex(RuntimeError, 'NickelMenu'):
            self.run_main(['--usb'])
        self.verify.assert_not_called()

    def test_check_cli_exit_status_and_output(self):
        for product, expected in [('393', 0), ('999', 1)]:
            with self.subTest(product=product):
                self.version.write_text(f'PRIVATE-SERIAL,4.9.77,4.46.23836,000-{product}\n', encoding='utf-8')
                before = self.snapshot()
                result = subprocess.run(
                    [sys.executable, '-X', 'utf8', str(ROOT / 'install.py'), '--check',
                     '--kobo-mount', str(self.mount)],
                    stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding='utf-8', timeout=15,
                )
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertNotIn('Traceback', result.stderr)
                self.assertNotIn('PRIVATE-SERIAL', result.stdout + result.stderr)
                self.assertEqual(self.snapshot(), before)

    def test_check_rejects_non_usb_or_package_options(self):
        for args in (['--ssh', '192.0.2.1'], ['--output', 'unused'],
                     ['--official-core', 'unused.zip'], ['--download-official']):
            with self.subTest(args=args):
                with self.assertRaises(SystemExit) as result:
                    self.run_main(['--check', *args])
                self.assertEqual(result.exception.code, 2)
                self.verify.assert_not_called()

    def test_output_and_ssh_modes_do_not_probe_usb(self):
        for args in (['--output', 'unused'], ['--ssh', '192.0.2.1']):
            with self.subTest(args=args):
                with self.assertRaises(BuildReached):
                    self.run_main(args)
                self.discovery.assert_not_called()

    def test_write_path_rechecks_device_after_preflight(self):
        self.assertEqual(self.run_main(['--check']), 0)
        self.version.write_text('PRIVATE-SERIAL,4.9.77,4.46.23836,000-999\n', encoding='utf-8')
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, '999'):
            installer.install_to_usb_mount(Path('unused-package'), self.mount, False)
        self.assertEqual(self.snapshot(), before)


if __name__ == '__main__':
    unittest.main()
