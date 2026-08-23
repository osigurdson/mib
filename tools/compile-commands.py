#!/usr/bin/env python3
"""Write compile_commands.json so clangd (and editors using it) can check every C file.

Linux files get the flags passed after `--` (the Makefile's). Windows files get the
mingw-w64 headers and defines that Zig's cross compiler uses, asked from
tools/zig-windows-cc.sh, so they check as Windows code on a Linux machine.
"""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WINDOWS_TARGET = "x86_64-w64-windows-gnu"


def is_windows(path):
    return path.startswith("src/platform/windows/") or path.startswith("tests/windows-")


def windows_flags():
    """The system include directories and defines `zig cc` passes to clang."""
    probe = ROOT / ".build" / "compile-commands-probe.c"
    probe.parent.mkdir(parents=True, exist_ok=True)
    probe.write_text("int probe;\n")
    try:
        result = subprocess.run(
            [str(ROOT / "tools" / "zig-windows-cc.sh"), "-###", "-c", str(probe), "-o", "/dev/null"],
            capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"compile-commands: no Zig, Windows files get no system headers ({error})",
              file=sys.stderr)
        return []
    finally:
        probe.unlink(missing_ok=True)
    words = [word.strip('"') for line in result.stderr.splitlines() for word in line.split()]
    flags = ["-nostdinc"]
    for option, value in zip(words, words[1:]):
        if option in ("-isystem", "-D"):
            flags += [option, value]
    return flags


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default=str(ROOT / "compile_commands.json"))
    parser.add_argument("flags", nargs="*", help="Linux compile flags, after --")
    args = parser.parse_args()

    files = subprocess.run(["git", "ls-files", "*.c", ":!third_party"], cwd=ROOT,
                           capture_output=True, text=True, check=True).stdout.split()
    windows = None
    entries = []
    for path in files:
        if is_windows(path):
            if windows is None:
                windows = windows_flags()
            command = ["clang", f"--target={WINDOWS_TARGET}", "-std=c11", "-Isrc", *windows]
        else:
            command = ["cc", *args.flags]
        entries.append({"directory": str(ROOT), "file": path,
                        "arguments": [*command, "-c", path]})
    pathlib.Path(args.output).write_text(json.dumps(entries, indent=1) + "\n")


if __name__ == "__main__":
    main()
