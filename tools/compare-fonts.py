"""Optional visual QA: custom rasterizer versus FreeType using the same font.
Requires Pillow, a C compiler, pkg-config, and FreeType development files.
FreeType is used only by the standalone reference tool, never by mib.
"""
from pathlib import Path
import shlex
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent.parent
subprocess.run(["make", "font-preview"], cwd=root, check=True)
with tempfile.TemporaryDirectory(prefix="tiny-font-comparison-") as temp:
    temp = Path(temp)
    cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "freetype2"], text=True))
    libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "freetype2"], text=True))
    subprocess.run(["cc", "-Os", "-std=c11", "-DFONT_REFERENCE_FREETYPE", *cflags, "-Isrc",
                    "tools/font-preview.c", "src/editor.c", "src/platform/linux/files.c", "-o", str(temp / "reference"), *libs],
                   cwd=root, check=True)
    subprocess.run([str(temp / "reference"), str(temp / "reference.ppm")], cwd=root, check=True)
    subprocess.run([str(root / "font-preview"), str(temp / "custom.ppm")], cwd=root, check=True)
    reference = Image.open(temp / "reference.ppm").copy()
    custom = Image.open(temp / "custom.ppm").copy()

# Pair each physical-size sample vertically, without resizing any glyph pixels.
bands = [(0,80),(80,160),(160,240),(240,320),(320,430),(430,540),(540,680),(680,820)]
labels = ["12 px (12 at 1x)", "14 px (14 at 1x)", "16 px (16 at 1x)", "18 px (18 at 1x)",
          "21 px (14 at 1.5x)", "22.5 px (18 at 1.25x)", "27 px (18 at 1.5x)", "36 px (18 at 2x)"]
height = sum(2*(end-start) + 36 for start,end in bands)
comparison = Image.new("RGB", (reference.width + 170,height), "#18202b")
draw = ImageDraw.Draw(comparison)
label_font = ImageFont.load_default(size=16)
y = 0
for (start,end), label in zip(bands,labels):
    h = end-start
    draw.text((12,y+3), label, fill="#8fbcbb",font=label_font)
    draw.text((12,y+38), "FreeType", fill="#e5edf5",font=label_font)
    draw.text((12,y+38+h), "Custom 8x8", fill="#e5edf5",font=label_font)
    comparison.paste(reference.crop((0,start,reference.width,end)),(170,y+26))
    comparison.paste(custom.crop((0,start,custom.width,end)),(170,y+26+h))
    y += 2*h+36
out = root / ".build/linux/font-comparison"
out.mkdir(parents=True, exist_ok=True)
comparison.save(out / "font-comparison.png")
custom.save(out / "font-samples.png")
print(f"Wrote {out}/font-comparison.png and font-samples.png (native pixel sizes)")
