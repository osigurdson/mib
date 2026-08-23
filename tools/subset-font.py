"""Extract one static, monospace TTF/OTF into TVF1 (no runtime font libraries)."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.recordingPen import DecomposingRecordingPen

# This list is the supported character contract, not all of the Nerd Font PUA.
CODEPOINTS = sorted(set(range(0x20, 0x7F)) | {
    0xA0, 0xB0, 0xB7, 0xD7,
    0x2010, 0x2013, 0x2014, *range(0x2018, 0x2020), 0x2022, 0x2026, 0x2032, 0x2033,
    *range(0x2190, 0x2194), 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
    0xE0B0, 0xE0B2, 0xF00C, 0xF040, 0xF071, 0xF07B, 0xF0C7, 0xF111, 0xF15B, 0xFFFD,
})

class OutlinePen(BasePen):
    """BasePen resolves components and implied TrueType on-curve points."""
    def __init__(self, glyphs):
        super().__init__(glyphs)
        self.data = bytearray()
        self.count = 0
        self.points = []

    def emit(self, opcode, *points):
        self.data.append(opcode)
        self.count += 1
        for x, y in points:
            x, y = round(x), round(y)
            if not (-32768 <= x <= 32767 and -32768 <= y <= 32767):
                raise ValueError("Outline coordinate exceeds int16")
            self.data.extend(struct.pack("<hh", x, y))
            self.points.append((x, y))

    def _moveTo(self, p): self.emit(1, p)
    def _lineTo(self, p): self.emit(2, p)
    def _qCurveToOne(self, control, end): self.emit(3, control, end)
    def _curveToOne(self, *points): raise ValueError("Cubic conversion was not applied")
    def _closePath(self): self.emit(4)
    def _endPath(self): raise ValueError("Open contours are not supported")

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
font = TTFont(args.source, recalcBBoxes=False, recalcTimestamp=False)
if "fvar" in font:
    raise SystemExit("Variable fonts are unsupported; supply a static instance")
glyphs = font.getGlyphSet()
cmap = font.getBestCmap()
units = font["head"].unitsPerEm
advance = font["hmtx"][cmap[ord("M")]][0]
records, commands = bytearray(), bytearray()
manifest = []
for cp in CODEPOINTS:
    if cp not in cmap:
        raise SystemExit(f"Required codepoint U+{cp:04X} is missing")
    name = cmap[cp]
    if font["hmtx"][name][0] != advance:
        raise SystemExit(f"U+{cp:04X} is not a single monospace cell")
    pen = OutlinePen(glyphs)
    # CFF cubic outlines become quadratics once, with <= 0.5 design-unit error.
    # TrueType quadratics pass through without re-approximation.
    recording = DecomposingRecordingPen(glyphs)
    glyphs[name].draw(recording)
    recording.replay(Cu2QuPen(pen, max_err=0.5, all_quadratic=True))
    if pen.count > 1024:
        raise SystemExit(f"U+{cp:04X} exceeds the 1024-command runtime bound")
    xs = [p[0] for p in pen.points] or [0]
    ys = [p[1] for p in pen.points] or [0]
    bounds = (min(xs), min(ys), max(xs), max(ys))
    records.extend(struct.pack("<IIHHhhhh", cp, len(commands), pen.count, 0, *bounds))
    commands.extend(pen.data)
    manifest.append({"codepoint": f"U+{cp:04X}", "source_glyph": name, "commands": pen.count})
header = struct.pack("<4sHHhhhhHHIII", b"TVF1", units, advance,
                     font["hhea"].ascent, font["hhea"].descent, font["hhea"].lineGap, 0,
                     len(CODEPOINTS), 20, 32, 32 + len(records), 32 + len(records) + len(commands))
args.output.write_bytes(header + records + commands)
args.output.with_suffix(".json").write_text(json.dumps({
    "family": "Jot Mono Subset",  # Renamed derivative; retain the upstream OFL.
    "source_family": font["name"].getDebugName(1),
    "source_version": font["name"].getDebugName(5),
    "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
    "units_per_em": units, "advance": advance,
    "glyphs": manifest,
}, indent=2) + "\n")
print(f"{args.output}: {len(CODEPOINTS)} glyphs, {len(header + records + commands)} bytes")
