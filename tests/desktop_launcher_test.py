#!/usr/bin/env python3
"""Exercise desktop entry scripts without contacting or modifying a device."""
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='reeden-launcher-') as tmp:
    root = Path(tmp) / 'Reeden 用户 tools'
    root.mkdir()
    for name in ['install.py', 'uninstall.py']:
        shutil.copyfile(SOURCE / name, root / name)
    for action in ('安装', '更新', '卸载'):
        for suffix in ('Windows.bat', 'macOS.command', 'Linux.sh'):
            name = f'一键{action}-{suffix}'
            shutil.copyfile(SOURCE / name, root / name)
        if os.name == 'nt':
            commands = [['cmd', '/d', '/c', str(root / f'一键{action}-Windows.bat')]]
        else:
            commands = [['sh', str(root / f'一键{action}-macOS.command')],
                        ['sh', str(root / f'一键{action}-Linux.sh')]]
        for command in commands:
            for option, expected in [('--help', 0), ('--not-a-real-option', 2)]:
                result = subprocess.run(command + [option], input='\n', capture_output=True,
                                        text=True, encoding='utf-8', errors='replace', timeout=20)
                assert result.returncode == expected, (command, option, result.returncode, result.stdout, result.stderr)
print('PASS: desktop install/update/uninstall launchers preserve success and argument-error exit status')
