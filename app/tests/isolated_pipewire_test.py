#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run signal tests on a private PipeWire server and policy-only WirePlumber."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

if any(shutil.which(cmd) is None for cmd in ('pipewire', 'wireplumber', 'dbus-run-session')):
    print('SKIP: requires PipeWire, WirePlumber 0.5+, and dbus-run-session')
    sys.exit(77)
if os.environ.get('WAVELINE_TEST_SESSION') != '1':
    env = dict(os.environ, WAVELINE_TEST_SESSION='1')
    sys.exit(subprocess.call(['dbus-run-session', '--', sys.executable, __file__, *sys.argv[1:]], env=env))

with tempfile.TemporaryDirectory(prefix='waveline-audio-test-') as tmp:
    root = Path(tmp)
    env = dict(os.environ, PIPEWIRE_RUNTIME_DIR=tmp, XDG_RUNTIME_DIR=tmp,
               XDG_CONFIG_HOME=tmp + '/config', XDG_STATE_HOME=tmp + '/state',
               XDG_CACHE_HOME=tmp + '/cache', PIPEWIRE_REMOTE='pipewire-0',
               WAVELINE_CHANNELS='system')
    # No ALSA/USB/Bluetooth modules. Clients inherit no user PipeWire drop-ins.
    env.pop('PIPEWIRE_CONFIG_DIR', None)
    env.pop('PIPEWIRE_CONFIG_NAME', None)
    config = root / 'server.conf'
    config.write_text('''
context.properties = {
 core.daemon = true
 core.name = pipewire-0
 default.clock.rate = 48000
 default.clock.quantum = 512
}
context.spa-libs = {
 audio.convert.* = audioconvert/libspa-audioconvert
 support.* = support/libspa-support
}
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-spa-node-factory }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-client-device }
 { name = libpipewire-module-access args = { access.force = unrestricted } }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-link-factory }
 { name = libpipewire-module-session-manager }
]
context.objects = [
 { factory = spa-node-factory args = {
     factory.name = support.node.driver
     node.name = Dummy-Driver
     node.group = pipewire.dummy
     priority.driver = 20000
 } }
]
''')
    processes = []
    with (root / 'server.log').open('w+') as log:
        try:
            processes.append(subprocess.Popen(['pipewire', '-c', str(config)], env=env, stdout=log, stderr=log))
            for _ in range(100):
                if (root / 'pipewire-0').exists():
                    break
                if processes[0].poll() is not None:
                    raise RuntimeError('private PipeWire server failed')
                time.sleep(0.05)
            else:
                raise RuntimeError('private PipeWire socket did not appear')
            # The policy profile does not load hardware monitors.
            processes.append(subprocess.Popen(['wireplumber', '--profile', 'policy'], env=env, stdout=log, stderr=log))
            time.sleep(1)
            result = subprocess.run(sys.argv[1:], env=env, timeout=60)
            if result.returncode:
                raise RuntimeError(f'signal test failed: {result.returncode}')
        except Exception as exc:
            print(exc, file=sys.stderr)
            log.flush()
            print((root / 'server.log').read_text(), file=sys.stderr)
            sys.exit(1)
        finally:
            for process in reversed(processes):
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
