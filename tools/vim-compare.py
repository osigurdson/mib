#!/usr/bin/env python3
"""Type Vim key sequences into the real Jot binary and into Vim, then compare the saved files.

Each case in tests/vim-compare.toml starts Jot on a fresh copy of a seed file inside a private,
headless Mutter session, types the keys, then saves with <Esc><Esc>:wq<CR>. The same keys run in
Vim (`vim -u NONE -es` with options matching Jot's conventions). Cases marked `status = "gap"`
are known differences; the run fails when a passing case regresses or a gap starts matching.

Test-only dependencies: Mutter, dbus-run-session, Python PyGObject (Gio) and Vim.
Nothing is sent to the user's desktop session.
"""
import argparse
import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib

ROOT = Path(__file__).resolve().parent.parent
# Vim settings that match Jot's fixed behaviour: 4-space indents, kept indentation,
# smart-case search, single-space joins and 79-column gq.
VIM_SETUP = ['set nocompatible', 'set expandtab shiftwidth=4 tabstop=4 softtabstop=0 autoindent',
             'set ignorecase smartcase nojoinspaces textwidth=79 nrformats=bin,hex',
             'call cursor(1, 1)']
SPECIAL = {'Esc': 0xff1b, 'CR': 0xff0d, 'BS': 0xff08, 'Tab': 0xff09, 'Del': 0xffff,
           'Left': 0xff51, 'Up': 0xff52, 'Right': 0xff53, 'Down': 0xff54,
           'Home': 0xff50, 'End': 0xff57, 'lt': ord('<')}
TOKEN = re.compile(r'<(C-[a-z\[]|' + '|'.join(SPECIAL) + r')>|(.)', re.S)


def tokens(keys):
    """Split Vim key notation into ('ctrl', 'x'), ('special', 'Esc') or ('text', 'a')."""
    for match in TOKEN.finditer(keys):
        name, char = match.groups()
        if name and name.startswith('C-'):
            yield 'ctrl', name[2]
        elif name:
            yield 'special', name
        else:
            yield 'text', char


def vim_keys(keys):
    """Turn key notation into a double-quoted Vim string for :execute "normal! ..."."""
    out = []
    for kind, value in tokens(keys):
        if kind == 'ctrl':
            out.append('\\<C-%s>' % value)
        elif kind == 'special':
            out.append('<' if value == 'lt' else '\\<%s>' % value)
        else:
            out.append({'\\': '\\\\', '"': '\\"'}.get(value, value))
    return ''.join(out)


def run_vim(seed, keys, folder):
    path = folder / 'vim.txt'
    path.write_text(seed)
    command = ['vim', '-N', '-u', 'NONE', '-U', 'NONE', '-i', 'NONE', '-n', '-es']
    for line in VIM_SETUP + ['exe "normal! %s"' % vim_keys(keys), 'wq!']:
        command += ['-c', line]
    subprocess.run(command + [str(path)], timeout=10, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return path.read_text()


def load_cases(path, only):
    data = tomllib.loads(path.read_text())
    seeds = data['seeds']
    cases = []
    for case in data['case']:
        unknown = set(case) - {'name', 'keys', 'vim_keys', 'seed', 'status', 'note'}
        if unknown or case.get('status', 'pass') not in ('pass', 'gap'):
            raise SystemExit(f'{path}: bad case {case!r}')
        if case.get('seed', 'default') not in seeds:
            raise SystemExit(f'{path}: case {case["name"]!r} uses unknown seed')
        if only and not any(re.search(pattern, case['name']) for pattern in only):
            continue
        cases.append(case)
    names = [case['name'] for case in data['case']]
    duplicates = {name for name in names if names.count(name) > 1}
    if duplicates:
        raise SystemExit(f'{path}: duplicate case names {sorted(duplicates)}')
    return seeds, cases


def diff(vim, jot):
    lines = difflib.unified_diff(vim.splitlines(keepends=True), jot.splitlines(keepends=True),
                                 'vim', 'jot', n=1)
    return ''.join('      ' + repr(line)[1:-1].replace('\\n', '') + '\n' for line in lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', type=Path, default=ROOT / '.build/linux/jot')
    parser.add_argument('--cases', type=Path, default=ROOT / 'tests/vim-compare.toml')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/linux/vim-compare')
    parser.add_argument('--only', action='append', metavar='REGEX', help='Run cases whose name matches (repeatable)')
    parser.add_argument('--show', action='store_true', help='Print the diff for known gaps too')
    parser.add_argument('--vim', action='store_true', help="Only print Vim's result for each case (no Jot)")
    parser.add_argument('--isolated', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    seeds, cases = load_cases(args.cases, args.only)
    if not cases:
        parser.error('no cases selected')
    if not shutil.which('vim'):
        parser.error('Test-only dependency missing: vim')
    args.output.mkdir(parents=True, exist_ok=True)
    if args.vim:
        for case in cases:
            folder = Path(tempfile.mkdtemp(prefix='vim-', dir=args.output))
            print(f'## {case["name"]}  {case["keys"]}')
            print(run_vim(seeds[case.get('seed', 'default')], case.get('vim_keys', case['keys']), folder), end='')
            shutil.rmtree(folder)
        return 0
    for tool in ('mutter', 'dbus-run-session'):
        if not shutil.which(tool):
            parser.error(f'Test-only dependency missing: {tool}')
    if not args.isolated:
        forwarded = sys.argv[1:] + ['--isolated']
        return subprocess.call(['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve())] + forwarded)
    try:
        from gi.repository import Gio, GLib
    except ImportError:
        parser.error('Test-only dependency missing: Python PyGObject (gi.repository.Gio)')
    if not args.binary.exists():
        parser.error(f'{args.binary} does not exist; run make first')

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    run_dir = Path(tempfile.mkdtemp(prefix='run-', dir=args.output))
    # Wayland socket paths are limited to 108 bytes, so the runtime directory stays short.
    runtime = Path(tempfile.mkdtemp(prefix='jot-vc-'))
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(run_dir / 'config'),
               XDG_DATA_HOME=str(run_dir / 'data'), XDG_CACHE_HOME=str(run_dir / 'cache'),
               XDG_STATE_HOME=str(run_dir / 'state'), GSETTINGS_BACKEND='memory',
               GIO_USE_VFS='local', TINYVI_OCR='0', WAYLAND_DISPLAY='wayland-0')
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
        raise TimeoutError(description)

    def has_remote_desktop():
        return bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                             'NameHasOwner', GLib.Variant('(s)', ('org.gnome.Mutter.RemoteDesktop',)),
                             None, Gio.DBusCallFlags.NONE, 1000, None).unpack()[0]

    if has_remote_desktop():
        raise RuntimeError('Refusing to attach to an existing compositor')
    compositor_log = (run_dir / 'mutter.log').open('w')
    compositor = subprocess.Popen(['mutter', '--headless', '--wayland', '--no-x11', '--wayland-display',
                                   'wayland-0', '--virtual-monitor', '1280x800'],
                                  env=env, stdout=compositor_log, stderr=compositor_log)
    failures = []
    try:
        def ready():
            if compositor.poll() is not None:
                raise RuntimeError('Mutter exited; see ' + str(run_dir / 'mutter.log'))
            return has_remote_desktop() and (runtime / 'wayland-0').exists()
        wait_for(ready, 'private Wayland compositor', 15)

        def remote(path, interface, method, params=None):
            return bus.call_sync('org.gnome.Mutter.RemoteDesktop', path, interface, method, params,
                                 None, Gio.DBusCallFlags.NONE, 3000, None)
        session = remote('/org/gnome/Mutter/RemoteDesktop', 'org.gnome.Mutter.RemoteDesktop',
                         'CreateSession').unpack()[0]

        def call(method, params=None):
            return remote(session, 'org.gnome.Mutter.RemoteDesktop.Session', method, params)

        def keysym(symbol, pressed):
            call('NotifyKeyboardKeysym', GLib.Variant('(ub)', (symbol, pressed)))

        def tap(symbol):
            keysym(symbol, True)
            keysym(symbol, False)
            pump(.004)

        def send(keys):
            for kind, value in tokens(keys):
                if kind == 'ctrl':
                    keysym(0xffe3, True)
                    tap(ord(value))
                    keysym(0xffe3, False)
                    pump(.02)
                elif kind == 'special':
                    tap(SPECIAL[value])
                else:
                    cp = ord(value)
                    tap(cp if cp < 256 else cp | 0x01000000)
            pump(.04)

        call('Start')
        tap(0xffe1)  # Create the virtual keyboard before Jot binds wl_seat.
        counts = {'PASS': 0, 'GAP': 0, 'FAIL': 0, 'FIXED': 0, 'ERROR': 0}
        for case in cases:
            seed = seeds[case.get('seed', 'default')]
            folder = Path(tempfile.mkdtemp(prefix='case-', dir=run_dir))
            note = folder / 'jot.txt'
            note.write_text(seed)
            log_path = folder / 'client.log'
            error = None
            with log_path.open('w') as log:
                app = subprocess.Popen([str(args.binary.resolve()), '--file', str(note)],
                                       env=dict(env, WAYLAND_DEBUG='client'), stdout=log, stderr=log)
                try:
                    def focused():
                        if app.poll() is not None:
                            raise RuntimeError(f'Jot exited early; see {log_path}')
                        return re.search(r'wl_keyboard[^\n]*\.enter\(', log_path.read_text(errors='replace'))
                    wait_for(focused, 'Jot keyboard focus')
                    pump(.05)
                    send(case['keys'])
                    send('<Esc><Esc>:wq<CR>')
                    wait_for(lambda: app.poll() is not None, 'Jot to save and quit', 4)
                    if app.returncode:
                        error = f'Jot exited {app.returncode}; see {log_path}'
                except Exception as exception:
                    error = str(exception)
                finally:
                    if app.poll() is None:
                        app.kill()
                        app.wait()
            jot = note.read_text()
            vim = run_vim(seed, case.get('vim_keys', case['keys']), folder)
            gap = case.get('status') == 'gap'
            if error:
                result = 'ERROR'
            elif jot == vim:
                result = 'FIXED' if gap else 'PASS'
            else:
                result = 'GAP' if gap else 'FAIL'
            counts[result] += 1
            label = f'{result:5} {case["name"]}  {case["keys"]}'
            if result in ('FAIL', 'ERROR', 'FIXED'):
                failures.append(case['name'])
            print(label, flush=True)
            if error:
                print('      ' + error)
            if result == 'FAIL' or (result == 'GAP' and args.show):
                print(diff(vim, jot), end='')
            if result == 'FIXED':
                print('      Now matches Vim: remove `status = "gap"` from this case.')
            if not error and result in ('PASS', 'FIXED'):
                shutil.rmtree(folder)
        print(', '.join(f'{n} {k.lower()}' for k, n in counts.items() if n))
    finally:
        compositor.terminate()
        try:
            compositor.wait(5)
        except subprocess.TimeoutExpired:
            compositor.kill()
        compositor_log.close()
        shutil.rmtree(runtime, ignore_errors=True)
    if failures:
        print(f'Failed: {", ".join(failures)}  (outputs kept in {run_dir})')
        return 1
    shutil.rmtree(run_dir, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
