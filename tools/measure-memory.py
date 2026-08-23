"""Measure real Wayland idle memory; fixtures and clipboard are isolated."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import zlib

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, default=root / ".build/linux/jot")
parser.add_argument("--kind", choices=("file", "page", "image"), default="file")
parser.add_argument("--samples", type=int, default=3)
parser.add_argument("--delay", type=float, default=0.8)
parser.add_argument("--max-pss", type=int, default=1024, help="exclusive PSS limit in KiB")
args = parser.parse_args()
if args.samples < 1 or args.delay <= 0:
    parser.error("samples and delay must be positive")

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

def screenshot():
    # Standard-library PNG fixture: 2880x1800, a common HiDPI screenshot size.
    width, height = 2880, 1800
    compressor = zlib.compressobj()
    row = b"\0" + b"\x33\x66\xaa" * width
    encoded = b"".join(compressor.compress(row) for _ in range(height)) + compressor.flush()
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", encoded) + chunk(b"IEND", b""))

failed = False
with tempfile.TemporaryDirectory(prefix="jot-memory-") as directory:
    fixture = Path(directory)
    if args.kind == "file":
        empty = fixture / "empty.txt"
        empty.touch()
        command = [str(args.binary.resolve()), str(empty)]
    else:
        page = fixture / "notes/1"
        (page / "assets").mkdir(parents=True)
        (page / "jot.md").write_text("![](assets/screenshot.png)\n" if args.kind == "image" else "")
        if args.kind == "image":
            (page / "assets/screenshot.png").write_bytes(screenshot())
        command = [str(args.binary.resolve())]
    # The notes folder, settings and remembered page all stay in the fixture.
    environment = dict(os.environ, JOT_OCR="0", JOT_DIR=str(fixture / "notes"),
                       XDG_CONFIG_HOME=str(fixture / "config"), XDG_STATE_HOME=str(fixture / "state"))
    for sample in range(args.samples):
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=environment)
        try:
            time.sleep(args.delay)
            if process.poll() is not None:
                raise SystemExit(process.stderr.read().decode())
            fields = {}
            for name in ("smaps_rollup", "status"):
                for line in Path(f"/proc/{process.pid}/{name}").read_text().splitlines():
                    if ":" not in line:
                        continue
                    key, value = line.split(":", 1)
                    if value.strip().endswith("kB"):
                        fields[key] = int(value.split()[0])
            private = fields["Private_Clean"] + fields["Private_Dirty"]
            print(f"{args.kind} run {sample + 1}: PSS {fields['Pss']} KiB; RSS {fields['Rss']} KiB; "
                  f"private {private} KiB; process RSS high-water {fields['VmHWM']} KiB")
            failed |= fields["Pss"] >= args.max_pss
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stderr.close()
if failed:
    raise SystemExit(f"PSS exceeded the < {args.max_pss} KiB idle target")
print(f"All idle samples below {args.max_pss} KiB PSS; excludes compositor and optional OCR process memory.")
