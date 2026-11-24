"""Represent docs/help.md as read-only C data for the :help document."""
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_bytes()
if b"\0" in text:
    sys.exit(f"{sys.argv[1]}: contains a NUL byte")
lines = ["/* Generated from docs/help.md by tools/embed-help.py. */",
         "static const char help_text[] = {"]
data = list(text) + [0]
for i in range(0, len(data), 16):
    lines.append("    " + ",".join(f"0x{byte:02x}" for byte in data[i:i+16]) + ",")
lines += ["};", ""]
Path(sys.argv[2]).write_text("\n".join(lines))
