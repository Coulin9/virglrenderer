#!/usr/bin/env python3
"""macOS control-plane regression; creates and removes one temporary LaunchAgent.

python3 tests/test_work_context.py [--output /path/to/evidence]
Requires clang, an interactive gui launchd domain, and Rosetta. Does not launch VM.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import select
import signal
import subprocess
import tempfile
import uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='appledeck-work-context-') as scratch:
    output = args.output.resolve() if args.output else Path(scratch)
    output.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for arch in ('arm64', 'x86_64'):
        binary = output / ('test-work-context-' + arch)
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang++', '-arch', arch, '-std=c++17', '-DENABLE_NEPTUNE',
                        '-fblocks', '-fno-objc-arc', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(source / 'src'),
                        str(source / 'tests/test_work_context.mm'),
                        str(source / 'src/neptune/npt_work_context.mm'),
                        '-framework', 'Foundation', '-o', str(binary)], check=True)
        binaries[arch] = binary
    service = 'org.appledeck.test-work-context.' + uuid.uuid4().hex
    plist = Path(scratch) / (service + '.plist')
    plist.write_bytes(plistlib.dumps({
        'Label': service,
        'ProgramArguments': [str(binaries['arm64']), '--service', service],
        'MachServices': {service: True},
        'StandardErrorPath': str(output / 'broker.log'),
    }))
    domain = 'gui/' + str(os.getuid())
    subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True)
    try:
        with (output / 'results.log').open('w') as log:
            for binary in binaries.values():
                subprocess.run([str(binary), '--probe', service], stdout=log,
                               stderr=subprocess.STDOUT, timeout=20, check=True)
            for mode in ('success', 'unsupported', 'wrong-nonce', 'wrong-pid', 'aggregate',
                         'inactive-before-device'):
                subprocess.run([str(binaries['arm64']), service, str(binaries['x86_64']), mode],
                               stdout=log, stderr=subprocess.STDOUT, timeout=20, check=True)
            # Hold only this uniquely named test broker so the device event is
            # guaranteed to precede authentication. Always resume it, including
            # assertion failures and timeouts in either test process.
            details = subprocess.check_output(['launchctl', 'print', domain + '/' + service], text=True)
            match = re.search(r'^\s*pid = (\d+)\s*$', details, re.MULTILINE)
            assert match, 'test broker PID missing'
            broker_pid = int(match[1])
            for mode in ('device-first', 'device-first-inactive'):
                process = None
                try:
                    os.kill(broker_pid, signal.SIGSTOP)
                    process = subprocess.Popen([str(binaries['arm64']), service,
                                                str(binaries['x86_64']), mode],
                                               stdout=subprocess.PIPE, stderr=log, text=True)
                    readable, _, _ = select.select([process.stdout], [], [], 3)
                    assert readable, 'device-first acknowledgement timed out'
                    assert process.stdout.readline() == 'DEVICE_READY\n'
                finally:
                    os.kill(broker_pid, signal.SIGCONT)
                    if process:
                        try:
                            assert process.wait(timeout=20) == 0
                        finally:
                            if process.poll() is None:
                                process.kill()
                                process.wait()
                            process.stdout.close()
            subprocess.run([str(binaries['arm64']), service + '.missing',
                            str(binaries['x86_64']), 'unavailable'],
                           stdout=log, stderr=subprocess.STDOUT, timeout=20, check=True)
            for binary in binaries.values():
                subprocess.run([str(binary), '--startup-races', service + '.missing'],
                               stdout=log, stderr=subprocess.STDOUT, timeout=15, check=True)
        results = (output / 'results.log').read_text()
        expected = re.findall(r'EXPECT role-sets pid=(\d+) count=(\d+)', results)
        assert len(expected) == 5, expected
        for pid, count in expected:
            setters = re.findall(r'work-context device-role: pid=' + pid + r' .*', results)
            assert len(setters) == int(count), (pid, count, setters)
            assert all('set=0 get=0 role=1 count=1 default=0' in line for line in setters), setters
        print(results)
    finally:
        subprocess.run(['launchctl', 'bootout', domain + '/' + service], check=True)
