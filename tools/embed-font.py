"""Represent the bundled TVF1 binary as read-only C data."""
from pathlib import Path
import sys
blob = Path(sys.argv[1]).read_bytes()
lines = ["/* Generated from the bundled TVF1 subset. */", "static const unsigned char font_data[] = {"]
for i in range(0, len(blob), 16):
    lines.append("    " + ",".join(f"0x{byte:02x}" for byte in blob[i:i+16]) + ",")
lines += ["};", ""]
Path(sys.argv[2]).write_text("\n".join(lines))
