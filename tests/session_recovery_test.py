#!/usr/bin/env python3
"""Exercise the real kernel lock and recovery script without touching Kobo hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith('linux'), 'Kobo lock helper uses Linux headers')
class RecoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='reeden-lock-helper-')
        cls.addClassCleanup(cls.build.cleanup)
        cls.helper = Path(cls.build.name) / 'fb-guard'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(ROOT / 'src/fb_guard.c'),
                        '-o', str(cls.helper)], check=True)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='reeden-recovery-')
        self.addCleanup(temp.cleanup)
        self.app = Path(temp.name)
        self.state = self.app / 'state'
        for name in ('state/active', 'logs', 'bin'):
            (self.app / name).mkdir(parents=True)
        shutil.copy2(self.helper, self.app / 'bin/fb-guard')
        self.script('recover.sh', (ROOT / 'scripts/recover.sh').read_text())
        self.script('session.sh', (ROOT / 'scripts/session.sh').read_text() +
                    '\npidof() { [ -e "$APP/test-nickel-running" ]; }\n')
        self.script('power.sh', '#!/bin/sh\necho "$1" >> power-calls\n[ ! -f fail-radio ]\n')
        self.script('native-session.sh', '#!/bin/sh\necho "$1" >> native-calls\n[ ! -f fail-ui ]\n')
        (self.state / 'owner').write_text('123 456\n')

    def script(self, name, body):
        path = self.app / name
        path.write_text(body)
        path.chmod(0o755)

    def recover(self):
        return subprocess.run(['sh', './recover.sh'], cwd=self.app, timeout=5).returncode

    def test_interrupted_recovery_and_exclusion(self):
        (self.state / 'recovering').mkdir()
        original = (self.app / 'recover.sh').read_text()
        self.script('recover.sh', '#!/bin/sh\n: > entered\nread -r line\n')
        holder = subprocess.Popen([str(self.helper), '--recover-session', str(self.app), '123 456'],
                                  cwd=self.app, stdin=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not (self.app / 'entered').exists():
                self.assertIsNone(holder.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.01)
            other = subprocess.run([str(self.helper), '--recover-session', str(self.app), '123 456'],
                                   timeout=3, stdin=subprocess.DEVNULL)
            self.assertEqual(other.returncode, 75)
        finally:
            holder.kill()
            holder.wait(timeout=3)
            holder.stdin.close()
        self.script('recover.sh', original)
        self.assertEqual(self.recover(), 0)
        self.assertFalse((self.state / 'recovering').exists())
        self.assertFalse((self.state / 'active').exists())

    def test_stale_generation_and_unsafe_lock(self):
        subprocess.run([str(self.helper), '--recover-session', str(self.app), 'different session'],
                       check=True, timeout=3)
        self.assertTrue((self.state / 'active').exists())
        lock = self.state / 'recovery.lock'
        lock.unlink()
        (self.app / 'untouched').write_text('safe')
        os.symlink(self.app / 'untouched', lock)
        self.assertNotEqual(self.recover(), 0)
        self.assertEqual((self.app / 'untouched').read_text(), 'safe')
        self.assertTrue((self.state / 'active').exists())

    def test_radio_failure_does_not_block_home_or_reset_live_nickel(self):
        (self.state / 'native-restart').touch()
        (self.app / 'fail-radio').touch()
        pending = self.state / 'native-wifi-pending'
        self.assertNotEqual(self.recover(), 0)
        self.assertFalse((self.state / 'active').exists())
        self.assertTrue(pending.exists())
        self.assertIn('pending', (self.app / 'logs/launcher.log').read_text())
        self.assertNotIn('[recovery] completed', (self.app / 'logs/launcher.log').read_text())
        self.assertEqual((self.app / 'native-calls').read_text().splitlines(), ['start'])
        (self.app / 'fail-radio').unlink()
        (self.app / 'test-nickel-running').touch()
        (self.state / 'active').mkdir()
        (self.state / 'native-restart').touch()
        self.assertNotEqual(self.recover(), 0)
        self.assertEqual((self.app / 'power-calls').read_text().splitlines(), ['native'])
        (self.app / 'test-nickel-running').unlink()
        (self.state / 'active').mkdir()
        (self.state / 'native-restart').touch()
        self.assertEqual(self.recover(), 0)
        self.assertFalse(pending.exists())
        self.assertEqual((self.app / 'power-calls').read_text().splitlines(), ['native', 'native'])

    def test_native_start_failure_remains_retryable(self):
        (self.state / 'native-restart').touch()
        (self.app / 'fail-ui').touch()
        self.assertNotEqual(self.recover(), 0)
        self.assertTrue((self.state / 'active').exists())
        self.assertTrue((self.state / 'native-restart').exists())
        (self.app / 'fail-ui').unlink()
        self.assertEqual(self.recover(), 0)
        self.assertFalse((self.state / 'active').exists())


if __name__ == '__main__':
    unittest.main()
