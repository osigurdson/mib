#!/usr/bin/env python3
"""Write version.h with the version and commit mib is built from.

The version is `git describe --tags --always --dirty` (v1.2.0 on a release tag,
v1.2.0-3-gabc1234 after it, with -dirty for uncommitted changes) and the commit is the full
hash, so `mib --version` names exactly the source a binary came from. MIB_VERSION and
MIB_COMMIT override them, for builds from a source archive without git. The file is only
rewritten when it changes, so builds stay incremental.
"""
import os
import subprocess
import sys
from pathlib import Path


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=Path(__file__).resolve().parent.parent,
                              capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


version = os.environ.get("MIB_VERSION") or git("describe", "--tags", "--always", "--dirty") or "unknown"
commit = os.environ.get("MIB_COMMIT") or git("rev-parse", "HEAD") or "unknown"
text = f'#define MIB_VERSION "{version}"\n#define MIB_COMMIT "{commit}"\n'
out = Path(sys.argv[1])
if not out.exists() or out.read_text() != text:
    out.write_text(text)
