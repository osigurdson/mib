#!/usr/bin/env python3
"""Drive the actual Jot Wayland executable in a private, headless Mutter session.

Test-only dependencies: Mutter, dbus-run-session, Python PyGObject (Gio).
No input or clipboard requests are sent to the user's desktop/session bus.
"""
import argparse
import os
from pathlib import Path
import re
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def png_fixture(width=96, height=64, seed=0):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data)))
    rows = b''.join(b'\0' + bytes([y * 255 // height, (100 + seed) % 256, (180 + seed * 13) % 256]) * width for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('.build/linux/jot'))
    parser.add_argument('--output', type=Path, default=Path('.build/linux/e2e'))
    parser.add_argument('--quick', action='store_true', help='Skip the 10,000-line/50-image stress workflow')
    parser.add_argument('--isolated', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    for tool in ('mutter', 'dbus-run-session'):
        if not shutil.which(tool):
            parser.error(f'Test-only dependency missing: {tool}')
    if not args.isolated:
        return subprocess.call(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()),
                                '--isolated', '--binary', str(args.binary.resolve()),
                                '--output', str(args.output.resolve())] + (['--quick'] if args.quick else []))
    try:
        from gi.repository import Gio, GLib
    except ImportError:
        parser.error('Test-only dependency missing: Python PyGObject (gi.repository.Gio)')
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    owner = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                          'NameHasOwner', GLib.Variant('(s)', ('org.gnome.Mutter.RemoteDesktop',)),
                          None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
    if owner:
        raise RuntimeError('Refusing to attach to an existing compositor; use the isolated launcher')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='run-', dir=args.output))
    runtime = root / 'runtime'
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root/'config'),
               XDG_DATA_HOME=str(root/'data'), XDG_CACHE_HOME=str(root/'cache'),
               XDG_STATE_HOME=str(root/'state'), GSETTINGS_BACKEND='memory',
               GIO_USE_VFS='local', JOT_OCR='0', WAYLAND_DISPLAY='wayland-0')
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DEBUG', None)
    context = GLib.MainContext.default()

    def pump(duration=.015):
        until = time.monotonic() + duration
        while time.monotonic() < until:
            while context.pending():
                context.iteration(False)
            time.sleep(.002)

    def wait_for(predicate, description, timeout=6):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return
            pump(.01)
        raise AssertionError('Timed out: ' + description)

    def dbus(path, interface, method, params=None):
        return bus.call_sync('org.gnome.Mutter.RemoteDesktop', path, interface, method, params,
                             None, Gio.DBusCallFlags.NONE, 3000, None)

    compositor_log = (root/'mutter.log').open('w')
    compositor = subprocess.Popen(['mutter', '--headless', '--wayland', '--no-x11',
                                   '--wayland-display', 'wayland-0', '--virtual-monitor', '1280x800'],
                                  env=env, stdout=compositor_log, stderr=compositor_log)
    app = None
    log = None
    failures = []
    session = None
    try:
        def ready():
            if compositor.poll() is not None:
                raise AssertionError('Mutter exited; see ' + str(root/'mutter.log'))
            named = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                                  'org.freedesktop.DBus', 'NameHasOwner',
                                  GLib.Variant('(s)', ('org.gnome.Mutter.RemoteDesktop',)),
                                  None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]
            return named and (runtime/'wayland-0').exists()
        wait_for(ready, 'private Wayland compositor')
        session = dbus('/org/gnome/Mutter/RemoteDesktop', 'org.gnome.Mutter.RemoteDesktop',
                       'CreateSession').unpack()[0]
        iface = 'org.gnome.Mutter.RemoteDesktop.Session'

        def call(method, params=None):
            return dbus(session, iface, method, params)

        def key(symbol):
            call('NotifyKeyboardKeysym', GLib.Variant('(ub)', (symbol, True)))
            call('NotifyKeyboardKeysym', GLib.Variant('(ub)', (symbol, False)))
            pump(.004)

        def text(value):
            for char in value:
                cp = ord(char)
                key(cp if cp < 256 else cp | 0x01000000)

        def ctrl(char):
            call('NotifyKeyboardKeysym', GLib.Variant('(ub)', (0xffe3, True)))
            key(ord(char))
            call('NotifyKeyboardKeysym', GLib.Variant('(ub)', (0xffe3, False)))
            pump(.03)

        def command(value):
            key(0xff1b)
            text(':' + value)
            key(0xff0d)
            pump(.03)

        call('Start')
        key(0xffe1)  # Create the virtual keyboard before Jot binds wl_seat.
        clipboard = {'image/png': png_fixture()}
        transfers = []
        transfer_errors = []

        def transfer(connection, sender, path, interface, signal, parameters):
            mime, serial = parameters.unpack()
            try:
                reply, fds = connection.call_with_unix_fd_list_sync(
                    'org.gnome.Mutter.RemoteDesktop', session, iface, 'SelectionWrite',
                    GLib.Variant('(u)', (serial,)), GLib.VariantType.new('(h)'),
                    Gio.DBusCallFlags.NONE, 3000, None, None)
                fd = fds.get(reply.unpack()[0])
                try:
                    data = clipboard[mime]
                    offset = 0
                    while offset < len(data):
                        offset += os.write(fd, data[offset:])
                finally:
                    os.close(fd)
                call('SelectionWriteDone', GLib.Variant('(ub)', (serial, True)))
                transfers.append(mime)
            except Exception as error:
                transfer_errors.append(str(error))

        bus.signal_subscribe('org.gnome.Mutter.RemoteDesktop', iface, 'SelectionTransfer', session,
                             None, Gio.DBusSignalFlags.NONE, transfer)
        call('EnableClipboard', GLib.Variant('(a{sv})', ({},)))

        def paste(mime='image/png', data=None):
            if data is not None:
                clipboard[mime] = data
            call('SetSelection', GLib.Variant('(a{sv})',
                 ({'mime-types': GLib.Variant('as', [mime])},)))
            pump(.03)
            count = len(transfers)
            ctrl('v')
            wait_for(lambda: len(transfers) > count or transfer_errors, 'clipboard transfer')
            assert not transfer_errors, transfer_errors
            pump(.07)

        def launch(name, seed=None, assets=None):
            nonlocal app, log
            folder = root/name
            folder.mkdir()
            note = folder/'note.txt'
            if seed is not None:
                note.write_text(seed)
            if assets:
                (folder/'assets').mkdir()
                for name, data in assets.items():
                    (folder/'assets'/name).write_bytes(data)
            log_path = folder/'client.log'
            if log:
                log.close()
            log = log_path.open('w')
            app_env = dict(env, WAYLAND_DEBUG='client')
            app = subprocess.Popen([str(args.binary.resolve()), str(note)],
                                   env=app_env, stdout=log, stderr=log)
            def focused():
                assert app.poll() is None, 'Jot exited early: ' + str(log_path)
                return re.search(r'wl_keyboard[^\n]*\.enter\(', log_path.read_text(errors='replace')) is not None
            wait_for(focused, 'Jot keyboard focus')
            pump(.05)
            return folder, note

        def finish(note):
            command('wq')
            wait_for(lambda: app.poll() is not None, 'Jot save and quit')
            assert app.returncode == 0, f'Jot exited {app.returncode}; see {log.name}'
            return note.read_text()

        def expect(actual, expected):
            if actual != expected:
                offset = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]),
                              min(len(actual), len(expected)))
                start, stop = max(0, offset-40), offset+80
                raise AssertionError(f'Mismatch at {offset}: expected {expected[start:stop]!r}, '
                                     f'got {actual[start:stop]!r}; lengths {len(expected)}/{len(actual)}')

        def refs(folder, note, count):
            names = re.findall(r'^!\[\]\((assets/[^)]+)\)$', note, re.M)
            assert len(names) == count, f'Expected {count} standalone images; got {len(names)} in {note[:300]!r}'
            for name in names:
                assert (folder/name).read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
            return names

        def editing():
            _, note = launch('text')
            text('iabc'); key(0xff50); key(0xff57); text('!')
            expect(finish(note), 'abc!')

        def navigation():
            _, note = launch('search-undo', 'target one\ntarget two\ntarget three\n')
            text('gg/target'); key(0xff0d); text('nNiX'); key(0xff1b)
            text('u'); ctrl('r')
            expect(finish(note), 'target one\nXtarget two\ntarget three\n')

        def text_paste():
            _, note = launch('text-paste')
            text('i'); paste('text/plain;charset=utf-8', 'caf\u00e9\r\nsecond'.encode())
            key(0xff57); text('!')
            expect(finish(note), 'caf\u00e9\nsecond!')

        def image_typing():
            folder, note = launch('image-typing')
            text('i'); paste(); text('after'); key(0xff50); key(0xff52); text('d')
            key(0xff1b); text('u'); ctrl('r')
            value = finish(note)
            refs(folder, value, 1)
            assert value.startswith('d\n') and value.endswith('\nafter'), value
            # Verify the saved image survives a fresh executable process.
            _, copy = launch('image-reopen', value, {name.name: name.read_bytes() for name in (folder/'assets').glob('*.png')})
            expect(finish(copy), value)

        def backspace():
            folder, note = launch('image-backspace')
            text('i'); paste(); text('tail'); key(0xff50); key(0xff08)
            expect(finish(note), 'tail')
            assert not list((folder/'assets').glob('*.png'))

        def delete_boundary():
            folder, note = launch('image-delete-boundary')
            text('iabove'); key(0xff0d); paste(); key(0xff1b); text('ggA'); key(0xffff)
            expect(finish(note), 'above\n')
            assert not list((folder/'assets').glob('*.png'))

        def image_repaste():
            folder, note = launch('image-repaste')
            text('i'); paste(); key(0xff52); key(0xff53); paste()
            refs(folder, finish(note), 2)

        def image_x():
            folder, note = launch('image-x')
            text('i'); paste(); key(0xff1b); text('ggx')
            expect(finish(note), '')
            assert not list((folder/'assets').glob('*.png'))

        def model_editing():
            seed = 'alpha bravo\nshort\ncafé words\n'
            _, note = launch('mixed-edits-model', seed)
            expected, cursor = seed, 0
            rng = random.Random(0x4a6f74)
            text('i')
            for step in range(500):
                action = rng.randrange(14)
                start = expected.rfind('\n', 0, cursor) + 1
                stop = expected.find('\n', cursor)
                if stop < 0:
                    stop = len(expected)
                if action < 4:
                    value = rng.choice(['a', 'xy', ' ', '_', 'Z'])
                    text(value)
                    expected = expected[:cursor] + value + expected[cursor:]
                    cursor += len(value)
                elif action == 4:
                    key(0xff08)
                    if cursor:
                        expected = expected[:cursor-1] + expected[cursor:]
                        cursor -= 1
                elif action == 5:
                    key(0xffff)
                    expected = expected[:cursor] + expected[cursor+1:]
                elif action == 6:
                    key(0xff51); cursor = max(start, cursor-1)
                elif action == 7:
                    key(0xff53); cursor = min(stop, cursor+1)
                elif action == 8:
                    key(0xff50); cursor = start
                elif action == 9:
                    key(0xff57); cursor = stop
                elif action in (10, 11):
                    column = cursor-start
                    if action == 10:
                        key(0xff52)
                        if start:
                            previous = expected.rfind('\n', 0, start-1)+1
                            cursor = previous + min(column, start-1-previous)
                    else:
                        key(0xff54)
                        if stop < len(expected):
                            following = expected.find('\n', stop+1)
                            if following < 0:
                                following = len(expected)
                            cursor = stop+1 + min(column, following-stop-1)
                elif action == 12:
                    key(0xff0d)
                    prefix = expected[start:cursor]
                    indent = prefix[:len(prefix)-len(prefix.lstrip(' \t'))]
                    value = '\n' + indent
                    expected = expected[:cursor] + value + expected[cursor:]
                    cursor += len(value)
                elif rng.randrange(2):
                    ctrl('u')
                    expected = expected[:start] + expected[cursor:]
                    cursor = start
                else:
                    ctrl('w')
                    previous = cursor
                    while previous and expected[previous-1] in ' \t':
                        previous -= 1
                    while previous and expected[previous-1] not in ' \t\n\r':
                        previous -= 1
                    expected = expected[:previous] + expected[cursor:]
                    cursor = previous
                if step % 25 == 24:
                    ctrl('s')
                    pump(.025)
                    actual = note.read_text()
                    assert actual == expected, f'Model mismatch after action {step+1}: expected {expected!r}, got {actual!r}'
            key(0xff1b); text('u'); command('w')
            expect(note.read_text(), seed)
            ctrl('r')
            expect(finish(note), expected)

        def stress():
            assets = {f'fixture-{i:03d}.png': png_fixture(512, 256, i) for i in range(50)}
            lines = []
            for i in range(10000):
                lines.append(f'line{i:05d} text, words, UTF-8 café and an ordinary Markdown paragraph.\n')
                if i % 200 == 100:
                    lines.append(f'![](assets/fixture-{i // 200:03d}.png)\n')
            seed = ''.join(lines)
            folder, note = launch('stress-10000-lines-50-images', seed, assets)
            text('ggISTART '); key(0xff1b)
            text('GAEND'); key(0xff1b)
            text('/line05000'); key(0xff0d); text('iCHECK '); key(0xff1b)
            expected = 'START ' + seed.replace('line05000', 'CHECK line05000', 1) + 'END'
            # Visit and render every image through the real search/input path.
            for i in range(50):
                text(f'/fixture-{i:03d}.png'); key(0xff0d); pump(.04)
            # Repeat insert/undo/redo transactions at the end of the large note.
            for _ in range(250):
                text('GAx'); key(0xff1b); text('u'); ctrl('r')
            expected += 'x' * 250
            expect(note.read_text(), seed)  # Editing never autosaves.
            command('w')
            wait_for(lambda: note.read_text() == expected, 'explicit stress save')
            command('e!')
            pump(.2)
            fields = {}
            for line in Path(f'/proc/{app.pid}/smaps_rollup').read_text().splitlines():
                if ':' in line:
                    key_name, value = line.split(':', 1)
                    if value.strip().endswith('kB'):
                        fields[key_name] = int(value.split()[0])
            def ticks():
                stat = Path(f'/proc/{app.pid}/stat').read_text().split()
                return int(stat[13]) + int(stat[14])
            before = ticks(); pump(.4); idle_ticks = ticks() - before
            print(f"  Stress: {len(seed.encode())} text bytes, 50 images, 250 edit/undo/redo cycles; "
                  f"idle PSS {fields['Pss']} KiB; idle CPU ticks {idle_ticks}", flush=True)
            assert idle_ticks <= 1, 'Unexpected redraw/CPU work while idle'
            value = finish(note)
            expect(value, expected)
            refs(folder, value, 50)

        cases = [('insert End', editing), ('search, n/N, undo/redo', navigation),
                 ('real text clipboard', text_paste), ('image typing/navigation/undo', image_typing),
                 ('Backspace at image boundary', backspace), ('Delete at image boundary', delete_boundary),
                 ('image paste on image', image_repaste), ('normal x on image', image_x)]
        cases.append(('500 mixed edits vs reference model', model_editing))
        if not args.quick:
            cases.append(('10,000 lines / 50 images', stress))
        for name, test in cases:
            try:
                test()
                print('PASS:', name, flush=True)
            except Exception as error:
                failures.append((name, str(error)))
                print('FAIL:', name, '-', error, flush=True)
            finally:
                if app and app.poll() is None:
                    app.terminate()
                    try:
                        app.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        app.kill(); app.wait()
                if log:
                    log.close()
                app = None
        print('Artifacts:', root, flush=True)
        if failures:
            print(f'{len(failures)}/{len(cases)} real-app workflows failed', flush=True)
            return 1
        print(f'{len(cases)} real-app workflows passed', flush=True)
        return 0
    finally:
        if app and app.poll() is None:
            app.terminate(); app.wait(timeout=3)
        if session:
            try:
                dbus(session, 'org.gnome.Mutter.RemoteDesktop.Session', 'Stop')
            except GLib.Error:
                pass
        compositor.terminate()
        try:
            compositor.wait(timeout=3)
        except subprocess.TimeoutExpired:
            compositor.kill(); compositor.wait()
        compositor_log.close()
        shutil.rmtree(runtime, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
