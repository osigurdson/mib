"""Record the README demo, docs/demo.svg, from the real editor.

    python3 tools/demo.py DEMO_FRAMES_BINARY OUTPUT.svg [--frames DIR]

Fills a throwaway notes folder, plays SCRIPT through tools/demo-frames.c (the real editor and
renderer, keys in and pixels out), then packs the frames into one animated SVG. The SVG shows
the first frame and lays each later frame's changed regions over it as PNG patches, timed with
CSS, so typing costs a few small patches rather than a whole frame per key. Uses only the
Python standard library. --frames keeps the PNG of every frame in DIR for checking.
"""
import os, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

SCALE120 = 240  # 2x pixels, so the demo stays sharp on high-DPI screens
PAGES = {
    1: '# Jot: memory vs Electron\n\n- Jot idles at 618.5 kB in System Monitor\n'
       '- A typical Electron-based app: `356 MB`\n',
    2: '# Ideas\n\n- macOS port: Cocoa window, `curl | bash` install\n'
       '- Relative line numbers (done!)\n- Signed Windows builds?\n',
    3: '# Reading list\n\n- A Philosophy of Software Design\n',
    4: '# Shopping\n\n- Coffee beans\n',
}
SCRIPT = '''
hold 1400
type ko
speed 60
type - [ ] Measure startup on **Hyprland** too
key esc
hold 900
paste {chart}
hold 2200
page 0
hold 1300
speed 450
type j
key enter
hold 2600
'''


def png(width, height, rgb):
    """Encode packed RGB bytes as a PNG."""
    stride = width * 3
    raw = b''.join(b'\0' + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2,
                                                               0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def chart():
    """Memory to scale, as circle areas: an Electron-based app (356 MB) and Jot (618.5 kB),
    the numbers in the demo note, measured in System Monitor."""
    import math
    w, h = 900, 380
    background = (16, 24, 38)
    px = bytearray(bytes(background) * (w * h))
    big = 160
    small = big * math.sqrt(0.6185 / 356)

    def disc(cx, cy, r, inner, outer):
        """An antialiased disc, lighter toward its upper left."""
        for y in range(int(cy - r - 2), int(cy + r + 3)):
            for x in range(int(cx - r - 2), int(cx + r + 3)):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                cover = min(1.0, max(0.0, r + 0.5 - d))
                if cover <= 0:
                    continue
                t = min(1.0, math.hypot(x - (cx - r / 3), y - (cy - r / 3)) / (1.6 * r))
                color = [inner[i] + (outer[i] - inner[i]) * t for i in range(3)]
                o = (y * w + x) * 3
                for i in range(3):
                    px[o + i] = round(px[o + i] + (color[i] - px[o + i]) * cover)

    disc(300, 190, big, (170, 225, 250), (70, 160, 215))
    disc(700, 190, max(small, 1.5), (255, 214, 140), (240, 170, 70))
    return png(w, h, bytes(px))


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=4)
    width, height = int(parts[1]), int(parts[2])
    return width, height, parts[4][:width * height * 3]


def patches(width, height, before, after):
    """The changed regions between two frames, as (x, y, w, h) boxes: changed rows grouped
    into bands, each cut to the columns that changed."""
    stride = width * 3
    rows = [y for y in range(height)
            if before[y * stride:(y + 1) * stride] != after[y * stride:(y + 1) * stride]]
    bands, start, last = [], None, None
    for y in rows:
        if start is None or y - last > 8:
            if start is not None:
                bands.append((start, last))
            start = y
        last = y
    if start is not None:
        bands.append((start, last))
    boxes = []
    for top, bottom in bands:
        changed = 0
        for y in range(top, bottom + 1):
            row = slice(y * stride, (y + 1) * stride)
            changed |= int.from_bytes(before[row], 'little') ^ int.from_bytes(after[row], 'little')
        # Bit positions to bytes, then bytes to pixels: the lowest set bit is the leftmost.
        first = ((changed & -changed).bit_length() - 1) // 8 // 3
        end = (changed.bit_length() - 1) // 8 // 3 + 1
        boxes.append((first, top, end - first, bottom - top + 1))
    return boxes


def crop(width, rgb, box):
    x, y, w, h = box
    stride = width * 3
    return b''.join(rgb[(y + i) * stride + x * 3:(y + i) * stride + (x + w) * 3] for i in range(h))


def main():
    binary, output = sys.argv[1], Path(sys.argv[2])
    keep = Path(sys.argv[sys.argv.index('--frames') + 1]) if '--frames' in sys.argv else None
    work = Path(tempfile.mkdtemp(prefix='jot-demo-'))
    notes, frames = work / 'notes', work / 'frames'
    for page, text in PAGES.items():
        (notes / str(page)).mkdir(parents=True)
        (notes / str(page) / 'jot.md').write_text(text)
    frames.mkdir()
    (work / 'chart.png').write_bytes(chart())
    env = dict(os.environ, JOT_DIR=str(notes), XDG_CONFIG_HOME=str(work / 'config'),
               XDG_STATE_HOME=str(work / 'state'), JOT_OCR='0')
    result = subprocess.run([binary, str(frames), str(SCALE120)], env=env, check=True,
                            input=SCRIPT.format(chart=work / 'chart.png'), text=True,
                            capture_output=True)
    timeline = []  # [frame number, milliseconds]
    for line in result.stdout.split('\n'):
        if line.startswith('hold '):
            timeline[-1][1] += int(line.split()[1])
        elif line:
            number, ms = line.split()
            timeline.append([int(number), int(ms)])
    width, height, previous = read_ppm(frames / '0000.ppm')
    scale = SCALE120 / 120
    logical_w, logical_h = round(width / scale), round(height / scale)
    total = sum(ms for _, ms in timeline)
    images, styles, at = [], [], timeline[0][1]

    def image(box, rgb, name=None):
        x, y, w, h = box
        uri = 'data:image/png;base64,' + __import__('base64').b64encode(
            png(w, h, crop(width, rgb, box))).decode()
        attributes = f' class="{name}"' if name else ''
        return (f'<image{attributes} x="{x / scale:g}" y="{y / scale:g}" width="{w / scale:g}" '
                f'height="{h / scale:g}" href="{uri}"/>')

    images.append(image((0, 0, width, height), previous))
    if keep:
        keep.mkdir(parents=True, exist_ok=True)
        (keep / '0000.png').write_bytes(png(width, height, previous))
    for number, ms in timeline[1:]:
        _, _, current = read_ppm(frames / f'{number:04d}.ppm')
        boxes = patches(width, height, previous, current)
        if boxes:
            name = f'f{len(styles)}'
            percent = 100 * at / total
            styles.append(f'@keyframes {name}{{0%,{percent:.3f}%{{opacity:0}}'
                          f'{percent + 0.001:.3f}%,100%{{opacity:1}}}}'
                          f'.{name}{{animation:{name} {total}ms infinite}}')
            images.extend(image(box, current, name) for box in boxes)
            if keep:
                (keep / f'{number:04d}.png').write_bytes(png(width, height, current))
        previous = current
        at += ms
    radius = 10
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {logical_w} {logical_h}" '
           f'width="{logical_w}" height="{logical_h}">'
           f'<title>Jot: editing Markdown, pasting an image and switching pages</title>'
           f'<style>{"".join(styles)}</style>'
           f'<clipPath id="window"><rect width="{logical_w}" height="{logical_h}" '
           f'rx="{radius}"/></clipPath><g clip-path="url(#window)">'
           + ''.join(images) + '</g></svg>\n')
    output.write_text(svg)
    print(f'{output}: {len(svg) // 1024} KiB, {len(timeline)} frames, {total / 1000:.1f} s')


main()
