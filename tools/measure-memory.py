"""Measure the idle native client with a real empty file in a Wayland session."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--samples", type=int, default=3)
parser.add_argument("--delay", type=float, default=0.8)
parser.add_argument("--max-pss", type=int, default=1024, help="exclusive PSS limit in KiB")
args = parser.parse_args()
if args.samples < 1 or args.delay <= 0:
    parser.error("samples and delay must be positive")
root = Path(__file__).resolve().parent.parent
failed = False
with tempfile.TemporaryDirectory(prefix="tiny-vi-memory-") as directory:
    empty = Path(directory) / "empty.txt"
    empty.touch()
    for sample in range(args.samples):
        process = subprocess.Popen([str(root / "tiny-vi"), str(empty)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(args.delay)
            if process.poll() is not None:
                raise SystemExit(process.stderr.read().decode())
            fields = {}
            for line in Path(f"/proc/{process.pid}/smaps_rollup").read_text().splitlines()[1:]:
                key, value = line.split(":", 1)
                if value.strip().endswith("kB"):
                    fields[key] = int(value.split()[0])
            print(f"run {sample + 1}: PSS {fields['Pss']} KiB; RSS {fields['Rss']} KiB")
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
print(f"All idle samples below {args.max_pss} KiB PSS (not a startup peak or compositor-memory limit).")
