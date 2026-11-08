"""Record a README demo from the real editor.

    python3 tools/demo.py DEMO_FRAMES_BINARY NAME OUTPUT.svg [--frames DIR]

NAME is a key of DEMOS: editing (docs/demo.svg) or ocr (docs/demo-ocr.svg). Fills a
throwaway notes folder, plays the demo's script through tools/demo-frames.c (the real editor
and renderer, keys in and pixels out), then packs the frames into one animated SVG. The SVG shows
the first frame and lays each later frame's changed regions over it as PNG patches, timed with
CSS, so typing costs a few small patches rather than a whole frame per key. Uses only the
Python standard library, plus Tesseract for the ocr demo. --frames keeps the PNG of every
frame in DIR for checking.
"""
import os, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

SCALE120 = 240  # 2x pixels, so the demo stays sharp on high-DPI screens
# The editing demo: type a Markdown task, paste a screenshot, switch pages.
PAGES = {
    1: '# Checkout latency\n\n- p99 jumped after the 14:00 deploy\n'
       '- Suspect the new `pricing` call\n',
    2: '# Standup\n\n- API: rate limits ship Thursday\n- Web: dark mode is in review\n'
       '- Me: chasing checkout latency\n',
    3: '# Ideas\n\n- Cache the tax tables\n',
    4: '# Commands\n\n- `kubectl rollout undo deploy/checkout`\n',
}
SCRIPT = '''
hold 1400
type ko
speed 60
type - [ ] Compare with **yesterday's** graph
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
# The ocr demo: a page with a pasted log screenshot, which Tesseract has already read, and a
# search that finds words inside it.
LOG = '''$ kubectl logs deploy/checkout --since=10m
14:02:11  GET   /cart      200     48 ms
14:02:12  POST  /pricing   504   3012 ms  upstream timeout
14:02:13  GET   /cart      200     51 ms
14:02:15  POST  /pricing   504   3008 ms  upstream timeout
'''
OCR_PAGES = {
    1: '# Checkout latency\n\n- p99 jumped after the 14:00 deploy\n- Logs from the checkout pod:\n'
       '\n![](assets/log.png)\n',
    2: '# Standup\n\n- API: rate limits ship Thursday\n',
}
OCR_SCRIPT = '''
hold 1400
speed 110
type /timeout
hold 500
key enter
hold 3200
'''
# Draws the log as a light terminal-like screenshot, using Jot's own renderer.
LOG_SCRIPT = '''
type :set view=source
key enter
type :set nonu
key enter
type :set bg=light
key enter
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
    """A latency graph with a spike, standing in for a dashboard screenshot."""
    import math
    w, h, left, right, top, bottom = 900, 340, 40, 870, 40, 300
    background, grid = (27, 31, 39), (48, 54, 66)
    line, fill = (97, 175, 239), (38, 62, 86)
    spike, spike_fill = (236, 120, 90), (86, 50, 46)
    px = bytearray(bytes(background) * (w * h))

    def put(x, y, color):
        if 0 <= x < w and 0 <= y < h:
            px[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(color)

    for y in range(top, bottom + 1, 52):
        for x in range(left, right):
            put(x, y, grid)
    for x in range(left, right):
        t = (x - left) / (right - left)
        y = 250 - 10 * math.sin(t * 23) - 6 * math.sin(t * 61 + 1)
        hot = math.exp(-((t - 0.68) / 0.045) ** 2)
        y -= 170 * hot
        stroke, under = (spike, spike_fill) if hot > 0.08 else (line, fill)
        for yy in range(int(y) + 3, bottom):
            put(x, yy, under)
        for yy in range(int(y) - 1, int(y) + 3):
            put(x, yy, stroke)
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


def record(binary, work, pages, script, assets=None):
    """Play script over a fresh notes folder holding pages, and assets (files copied into a
    page's assets/ folder), in its own config and state folders. Returns the frames folder and
    [frame number, milliseconds] for each frame."""
    work.mkdir(parents=True)
    notes, frames = work / 'notes', work / 'frames'
    for page, text in pages.items():
        (notes / str(page) / 'assets').mkdir(parents=True)
        (notes / str(page) / 'jot.md').write_text(text)
    for page, files in (assets or {}).items():
        for file in files:
            (notes / str(page) / 'assets' / file.name).write_bytes(file.read_bytes())
    frames.mkdir()
    env = dict(os.environ, JOT_DIR=str(notes), XDG_CONFIG_HOME=str(work / 'config'),
               XDG_STATE_HOME=str(work / 'state'), JOT_OCR='0')
    result = subprocess.run([binary, str(frames), str(SCALE120)], env=env, check=True,
                            input=script, text=True, capture_output=True)
    timeline = []
    for line in result.stdout.split('\n'):
        if line.startswith('hold '):
            timeline[-1][1] += int(line.split()[1])
        elif line:
            number, ms = line.split()
            timeline.append([int(number), int(ms)])
    return frames, timeline


def log_screenshot(binary, work):
    """LOG drawn by Jot in a light source view, cropped to its five lines of text, as a PNG."""
    frames, timeline = record(binary, work, {1: LOG}, LOG_SCRIPT)
    width, height, rgb = read_ppm(frames / f'{timeline[-1][0]:04d}.ppm')
    stride = width * 3
    pixel = lambda x, y: rgb[y * stride + x * 3:y * stride + x * 3 + 3]
    background = pixel(width // 2, height // 2)
    # Text rows: the window's right edge shows the page background (not the title or status bar)
    # and the row has ink. The cursor sits on the line after the log, so keep five lines.
    inked = [y for y in range(height) if pixel(width - 3, y) == background
             and rgb[y * stride:(y + 1) * stride] != background * width]
    lines, start = [], inked[0]
    for a, b in zip(inked, inked[1:] + [None]):
        if b is None or b - a > 1:
            lines.append((start, a))
            start = b
    top, bottom = lines[0][0], lines[LOG.count('\n') - 1][1]
    columns = [x for x in range(width) for y in range(top, bottom + 1, 2)
               if pixel(x, y) != background]
    # The same margin on every side, small enough to stay below the title bar.
    page_top = next(y for y in range(height) if pixel(width - 3, y) == background)
    margin = min(28, top - page_top)
    box = (min(columns) - margin, top - margin, max(columns) - min(columns) + 2 * margin,
           bottom - top + 2 * margin)
    return png(box[2], box[3], crop(width, rgb, box))


def main():
    binary, name, output = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    keep = Path(sys.argv[sys.argv.index('--frames') + 1]) if '--frames' in sys.argv else None
    work = Path(tempfile.mkdtemp(prefix='jot-demo-'))
    if name == 'editing':
        title = 'Jot: editing Markdown, pasting a screenshot and switching pages'
        (work / 'chart.png').write_bytes(chart())
        frames, timeline = record(binary, work / 'demo', PAGES,
                                  SCRIPT.format(chart=work / 'chart.png'))
    elif name == 'ocr':
        title = 'Jot: searching for text inside a pasted screenshot'
        image = work / 'log.png'
        image.write_bytes(log_screenshot(binary, work / 'log'))
        # Read it the way Jot does, writing log.png.txt and log.png.tsv beside it.
        subprocess.run(['tesseract', str(image), str(image), 'txt', 'tsv'], check=True,
                       capture_output=True)
        frames, timeline = record(binary, work / 'demo', OCR_PAGES, OCR_SCRIPT,
                                  assets={1: [image, Path(f'{image}.txt'), Path(f'{image}.tsv')]})
    else:
        sys.exit(f'Unknown demo {name}; use editing or ocr')
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
           f'<title>{title}</title>'
           f'<style>{"".join(styles)}</style>'
           f'<clipPath id="window"><rect width="{logical_w}" height="{logical_h}" '
           f'rx="{radius}"/></clipPath><g clip-path="url(#window)">'
           + ''.join(images) + '</g></svg>\n')
    output.write_text(svg)
    print(f'{output}: {len(svg) // 1024} KiB, {len(timeline)} frames, {total / 1000:.1f} s')


main()
