"""Record a README demo from the real editor.

    python3 tools/demo.py DEMO_FRAMES_BINARY NAME OUTPUT.svg [--frames DIR] [--mp4 OUTPUT.mp4]

NAME is a key of DEMOS: editing (docs/demo.svg) or ocr (docs/demo-ocr.svg). Fills a
throwaway notes folder, plays the demo's script through tools/demo-frames.c (the real editor
and renderer, keys in and pixels out), then packs the frames into one animated SVG. The SVG shows
the first frame and lays each later frame's changed regions over it as PNG patches, timed with
CSS, so typing costs a few small patches rather than a whole frame per key. Uses only the
Python standard library, plus Tesseract for the ocr demo. --frames keeps the PNG of every
frame in DIR for checking. --mp4 also encodes the same frames and timings as an H.264 video
with ffmpeg, for places that don't play animated SVGs.
"""
import os, shutil, struct, subprocess, sys, tempfile, zlib
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
# The ocr demo: a release checklist with a pasted screenshot of a monitoring dashboard, which
# Tesseract has already read. Searching for invoice finds it twice in the notes, then in the
# dashboard's graph legend.
OCR_PAGES = {
    1: '# Release 2.4\n\n- [x] Bump the version and changelog\n'
       '- [ ] Fix the flaky `invoice` test\n- [ ] Ask Sam about the invoice latency\n'
       '\n![](assets/dashboard.png)\n',
    2: '# Standup\n\n- API: rate limits ship Thursday\n',
}
OCR_SCRIPT = '''
type gg
begin
hold 1200
speed 110
type /invoice
hold 300
key enter
hold 1300
speed 1300
type n
type n
hold 1700
'''
# The dashboard's labels. Each is drawn by mib (headings come out larger), then recoloured and
# placed by dashboard().
LABELS = ['Checkout service / Overview', 'Last 1 hour', 'Error rate', 'Requests / s',
          'p99 latency', '# 4.8%', '# 1,284', '# 2.9 s', 'p99 latency by endpoint',
          'POST /invoice', 'GET /cart', '3s', '2s', '1s', '0s', '13:30', '13:45', '14:00',
          '14:15']
# Reading view: no gutter, and heading markers hidden.
LABEL_SCRIPT = '''
type :set view=reading
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
        (notes / str(page) / 'page.md').write_text(text)
    for page, files in (assets or {}).items():
        for file in files:
            (notes / str(page) / 'assets' / file.name).write_bytes(file.read_bytes())
    frames.mkdir()
    env = dict(os.environ, MIB_DIR=str(notes), XDG_CONFIG_HOME=str(work / 'config'),
               XDG_STATE_HOME=str(work / 'state'), MIB_OCR='0')
    result = subprocess.run([binary, str(frames), str(SCALE120)], env=env, check=True,
                            input=script, text=True, capture_output=True)
    timeline = []
    for line in result.stdout.split('\n'):
        if line.startswith('hold '):
            timeline[-1][1] += int(line.split()[1])
        elif line == 'begin':
            timeline = []
        elif line:
            number, ms = line.split()
            timeline.append([int(number), int(ms)])
    return frames, timeline


def label_sprites(binary, work):
    """Each of LABELS as drawn by mib: {label: (width, height, coverage, baseline)}, where
    coverage is one byte per pixel from 0 (background) to 255 (full ink), ready to draw in any
    colour, and baseline is how far below the top the text sits.
    Labels are drawn a few at a time so each batch fits in the window."""
    sprites = {}
    for i in range(0, len(LABELS), 6):
        sprites.update(label_batch(binary, work / str(i), LABELS[i:i + 6]))
    return sprites


def label_batch(binary, work, labels):
    frames, timeline = record(binary, work, {1: '\n\n'.join(labels) + '\n'}, LABEL_SCRIPT)
    width, height, rgb = read_ppm(frames / f'{timeline[-1][0]:04d}.ppm')
    stride = width * 3
    pixel = lambda x, y: rgb[y * stride + x * 3:y * stride + x * 3 + 3]
    background = pixel(width - 3, height // 2)
    # Text rows: the window's right edge shows the page background and the row has ink.
    inked = [y for y in range(height) if pixel(width - 3, y) == background
             and rgb[y * stride:(y + 1) * stride] != background * width]
    lines, start = [], inked[0]
    for a, b in zip(inked, inked[1:] + [None]):
        if b is None or b - a > 1:
            lines.append((start, a))
            start = b
    lum = lambda p: 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]
    paper = lum(background)
    sprites = {}
    # Reading view draws a rule under level-1 headings; those thin groups aren't labels.
    lines = [line for line in lines if line[1] - line[0] > 6]
    assert len(lines) >= len(labels), f'{len(lines)} of {len(labels)} labels fit in the window'
    for label, (top, bottom) in zip(labels, lines):
        xs = [x for x in range(width) for y in range(top, bottom + 1)
              if pixel(x, y) != background]
        left, right = min(xs), max(xs) + 1
        dark = [lum(pixel(x, y)) for y in range(top, bottom + 1) for x in range(left, right)]
        ink = min(dark)
        coverage = bytes(max(0, min(255, round(255 * (paper - v) / (paper - ink))))
                         for v in dark)
        # The baseline: the last row with much ink, so descenders and slashes don't count.
        w = right - left
        counts = [sum(1 for c in coverage[r * w:(r + 1) * w] if c > 128)
                  for r in range(bottom - top + 1)]
        base = max(r for r, n in enumerate(counts) if n >= max(counts) / 4) + 1
        sprites[label] = (w, bottom - top + 1, coverage, base)
    return sprites


def dashboard(binary, work):
    """A screenshot of a dark monitoring dashboard as a PNG: title bar, sidebar, three stat
    panels and a latency graph whose legend reads POST /invoice and GET /cart."""
    import math
    sprites = label_sprites(binary, work)
    W, H = 1500, 540
    px = bytearray(bytes((17, 18, 23)) * (W * H))

    def blend(x, y, color, a):
        if 0 <= x < W and 0 <= y < H and a > 0:
            o = (y * W + x) * 3
            for i in range(3):
                px[o + i] = round(px[o + i] + (color[i] - px[o + i]) * a)

    def rect(x, y, w, h, color, radius=0):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                dx = max(x + radius - xx - 0.5, xx + 0.5 - (x + w - radius), 0)
                dy = max(y + radius - yy - 0.5, yy + 0.5 - (y + h - radius), 0)
                cover = min(1.0, max(0.0, radius + 0.5 - math.hypot(dx, dy))) if radius else 1
                blend(xx, yy, color, cover)

    def text(label, x, base, color, height=None):
        """Draw a label starting at x on the baseline base, or centred in the band of height
        starting at base when height is given. Returns its width."""
        w, h, coverage, baseline = sprites[label]
        if height is not None:
            base += (height + baseline) // 2
        y = base - baseline
        for row in range(h):
            for col in range(w):
                blend(x + col, y + row, color, coverage[row * w + col] / 255)
        return w

    def panel(x, y, w, h, title):
        rect(x, y, w, h, (44, 48, 54), 8)
        rect(x + 2, y + 2, w - 4, h - 4, (24, 27, 31), 7)
        text(title, x + 24, y + 50, (204, 204, 220))

    muted, bright = (142, 142, 160), (204, 204, 220)
    # Title bar, with GNOME-style minimise, maximise and close buttons on the right, and
    # sidebar.
    rect(0, 0, W, 72, (24, 27, 31))
    glyph = (200, 200, 210)
    for i in range(3):
        cx, cy = W - 46 - 44 * (2 - i), 36
        rect(cx - 14, cy - 14, 28, 28, (52, 56, 64), 14)
        if i == 0:
            rect(cx - 5, cy + 3, 10, 2, glyph)
        elif i == 1:
            rect(cx - 5, cy - 5, 10, 10, glyph)
            rect(cx - 3, cy - 3, 6, 6, (52, 56, 64))
        else:
            for d in range(-50, 51):
                t = d / 10
                for sx in (1, -1):
                    x, y = cx + t, cy + sx * t
                    for yy in range(int(y) - 1, int(y) + 3):
                        blend(round(x), yy, glyph, min(1.0, max(0.0, 1.4 - abs(yy - y))))
    text('Checkout service / Overview', 96, 0, bright, 72)
    label_w = sprites['Last 1 hour'][0]
    pill_x = W - 46 - 88 - 14 - 32 - label_w - 40
    rect(pill_x, 14, label_w + 40, 44, (44, 48, 54), 6)
    text('Last 1 hour', pill_x + 20, 14, bright, 44)
    rect(0, 72, 72, H - 72, (24, 27, 31))
    for i in range(4):
        rect(22, 104 + 64 * i, 28, 28, (70, 75, 85), 6)
    # Stat panels.
    stats = [('Error rate', '# 4.8%', (242, 73, 92)), ('Requests / s', '# 1,284', (115, 191, 105)),
             ('p99 latency', '# 2.9 s', (255, 152, 48))]
    left, gap, top = 96, 20, 96
    stat_w = (W - left - 24 - 2 * gap) // 3
    for i, (title, value, color) in enumerate(stats):
        x = left + i * (stat_w + gap)
        panel(x, top, stat_w, 140, title)
        text(value, x + 24, top + 118, color)
    # The latency graph.
    gy, gh, gw = top + 140 + gap, H - (top + 140 + gap) - 24, W - left - 24
    panel(left, gy, gw, gh, 'p99 latency by endpoint')
    plot_l, plot_r = left + 90, left + gw - 30
    plot_t, plot_b = gy + 80, gy + gh - 58
    for i, label in enumerate(['3s', '2s', '1s', '0s']):
        y = plot_t + (plot_b - plot_t) * i // 3
        rect(plot_l, y, plot_r - plot_l, 2, (44, 48, 54))
        text(label, left + 28, y + sprites[label][3] // 2, muted)
    for i, label in enumerate(['13:30', '13:45', '14:00', '14:15']):
        x = plot_l + (plot_r - plot_l) * (2 * i + 1) // 8 - sprites[label][0] // 2
        text(label, x, plot_b + 44, muted)
    seconds_y = lambda v: plot_b - (plot_b - plot_t) * v / 3
    orange, blue = (255, 152, 48), (87, 148, 242)
    for color, curve in [
            (blue, lambda t: 0.45 + 0.06 * math.sin(t * 31) + 0.04 * math.sin(t * 83 + 2)),
            (orange, lambda t: 0.8 + 0.08 * math.sin(t * 23 + 1)
             + 1.9 * math.exp(-((t - 0.62) / 0.06) ** 2))]:
        for x in range(plot_l, plot_r):
            t = (x - plot_l) / (plot_r - plot_l)
            y = seconds_y(curve(t))
            for yy in range(int(y) - 3, int(y) + 4):
                blend(x, yy, color, min(1.0, max(0.0, 2.5 - abs(yy - y))))
    # Legend, beside the panel title.
    x = left + gw - 24 - sum(sprites[label][0] + 88 for label in ['POST /invoice', 'GET /cart'])
    for label, color in [('POST /invoice', orange), ('GET /cart', blue)]:
        rect(x, gy + 38, 28, 6, color, 3)
        x += 40 + text(label, x + 40, gy + 50, bright) + 48
    return png(W, H, bytes(px))


def main():
    binary, name, output = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    option = lambda flag: Path(sys.argv[sys.argv.index(flag) + 1]) if flag in sys.argv else None
    keep, video = option('--frames'), option('--mp4')
    # Raw frames are about 5 MB each, so the work folder goes as soon as the SVG is written.
    work = Path(tempfile.mkdtemp(prefix='mib-demo-'))
    try:
        build(binary, name, output, keep, video, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def build(binary, name, output, keep, video, work):
    if name == 'editing':
        title = 'mib: editing Markdown, pasting a screenshot and switching pages'
        (work / 'chart.png').write_bytes(chart())
        frames, timeline = record(binary, work / 'demo', PAGES,
                                  SCRIPT.format(chart=work / 'chart.png'))
    elif name == 'ocr':
        title = 'mib: searching for text inside a pasted screenshot'
        image = work / 'dashboard.png'
        image.write_bytes(dashboard(binary, work / 'labels'))
        # Read it the way mib does, writing dashboard.png.txt and .tsv beside it.
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
    # What a viewer sees: the first frame with every patch shown so far laid over it. Each
    # frame is checked against it, so a wrong patch fails the recording.
    shown = bytearray(previous)
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
            for x, y, w, h in boxes:
                for row in range(y, y + h):
                    start = (row * width + x) * 3
                    shown[start:start + w * 3] = current[start:start + w * 3]
            if shown != current:
                sys.exit(f'Frame {number} does not match its patches')
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
    if video:
        mp4(frames, timeline, video, title)


def mp4(frames, timeline, output, title):
    """Encode the frames, each shown for its time in timeline, as an H.264 MP4 at 30 frames a
    second: yuv420p and faststart so browsers, phones and chat apps play it inline. Frames go to
    ffmpeg raw, each repeated to the video frame its time ends on, so rounding doesn't add up."""
    fps = 30
    width, height, _ = read_ppm(frames / f'{timeline[0][0]:04d}.ppm')
    encoder = subprocess.Popen(
        ['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
         '-s', f'{width}x{height}', '-r', str(fps), '-i', '-',
         '-vf', 'scale=out_color_matrix=bt709,format=yuv420p', '-c:v', 'libx264',
         '-preset', 'veryslow', '-crf', '20', '-tune', 'animation', '-colorspace', 'bt709',
         '-color_primaries', 'bt709', '-color_trc', 'bt709', '-movflags', '+faststart',
         '-metadata', f'title={title}', str(output)], stdin=subprocess.PIPE)
    written, at = 0, 0
    for number, ms in timeline:
        at += ms
        rgb = read_ppm(frames / f'{number:04d}.ppm')[2]
        for _ in range(round(at * fps / 1000) - written):
            encoder.stdin.write(rgb)
            written += 1
    encoder.stdin.close()
    if encoder.wait():
        sys.exit(f'ffmpeg failed writing {output}')
    print(f'{output}: {output.stat().st_size // 1024} KiB')


main()
