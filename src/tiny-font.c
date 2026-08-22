#include "tiny-font.h"
#include <stddef.h>
#include <string.h>
#include "platform.h"
#include "font-data.h"

/* Deliberately fixed bounds: no per-glyph malloc, growing arrays, or font VM. */
#define CACHE_SLOTS 64
#define CACHE_BYTES (48 * 1024)
#define MAX_EDGES 2048
#define MAX_DIMENSION 256
#define SAMPLES 8
#define RECORD_BYTES 20
#define HEADER_BYTES 32

struct edge {
    float x0, y0, x1, y1;
};
struct crossing {
    float x;
    int direction;
};
struct workspace {
    struct edge edges[MAX_EDGES];
    struct crossing crossings[MAX_EDGES];
    unsigned count;
};
struct entry {
    uint32_t codepoint, pixels64;
    struct tiny_glyph glyph;
};
struct tiny_font {
    unsigned pixels64, count, used;
    struct workspace *workspace;
    struct entry entries[CACHE_SLOTS];
    uint8_t cache[CACHE_BYTES];
};

static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static int s16(const unsigned char *p)
{
    unsigned n = u16(p);
    return n < 32768 ? (int)n : (int)n - 65536;
}
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int floor_int(float value)
{
    int n = (int)value;
    return n - (value < n);
}
static int ceil_int(float value)
{
    int n = (int)value;
    return n + (value > n);
}
static float absolute(float value) { return value < 0 ? -value : value; }

/* TVF1 is generated data, but check every table/command before trusting it. */
static bool valid_font(const unsigned char *data, size_t size)
{
    if (size < HEADER_BYTES || memcmp(data, "TVF1", 4) || !u16(data + 4) || !u16(data + 6) ||
        u16(data + 14) || s16(data + 8) <= s16(data + 10) || !u16(data + 16) ||
        u16(data + 16) > 512 || u16(data + 18) != RECORD_BYTES || u32(data + 20) != HEADER_BYTES ||
        u32(data + 28) != size)
        return false;
    unsigned count = u16(data + 16);
    size_t commands = HEADER_BYTES + (size_t)count * RECORD_BYTES;
    if (commands > size || u32(data + 24) != commands)
        return false;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *record = data + HEADER_BYTES + i * RECORD_BYTES;
        uint32_t cp = u32(record), offset = u32(record + 4);
        unsigned n = u16(record + 8);
        if (cp <= previous || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || n > 1024 ||
            u16(record + 10) || (!i && offset) || offset > size - commands ||
            s16(record + 12) > s16(record + 16) || s16(record + 14) > s16(record + 18))
            return false;
        previous = cp;
        size_t at = commands + offset;
        bool open = false;
        for (unsigned j = 0; j < n; ++j) {
            if (at >= size)
                return false;
            unsigned opcode = data[at++], bytes = opcode == 3 ? 8 : opcode == 4 ? 0 : 4;
            if (opcode < 1 || opcode > 4 || bytes > size - at)
                return false;
            if (opcode == 1) {
                if (open)
                    return false;
                open = true;
            } else {
                if (!open)
                    return false;
                if (opcode == 4)
                    open = false;
            }
            for (unsigned k = 0; k < bytes; k += 4) {
                int x = s16(data + at + k), y = s16(data + at + k + 2);
                if (x < s16(record + 12) || x > s16(record + 16) || y < s16(record + 14) ||
                    y > s16(record + 18))
                    return false;
            }
            at += bytes;
        }
        if (open || (i + 1 < count && at != commands + u32(record + RECORD_BYTES + 4)) ||
            (i + 1 == count && at != size))
            return false;
    }
    return true;
}

struct tiny_font *tiny_font_open(void)
{
    if (!valid_font(font_data, sizeof(font_data)))
        return NULL;
    return platform_allocate_zeroed(sizeof(struct tiny_font));
}

void tiny_font_end_frame(struct tiny_font *font)
{
    if (font && font->workspace) {
        platform_release(font->workspace, sizeof(*font->workspace));
        font->workspace = NULL;
    }
}

void tiny_font_close(struct tiny_font *font)
{
    if (!font)
        return;
    tiny_font_end_frame(font);
    platform_release(font, sizeof(*font));
}

bool tiny_font_set_size(struct tiny_font *font, unsigned pixels64)
{
    if (!font || pixels64 < 64 || pixels64 > 128 * 64)
        return false;
    font->pixels64 = pixels64;
    return true;
}

struct tiny_font_metrics tiny_font_metrics_at(unsigned pixels64)
{
    float scale = (float)pixels64 / (64 * u16(font_data + 4));
    struct tiny_font_metrics metrics = {
        .advance = (int)(u16(font_data + 6) * scale + 0.5f),
        .ascender = ceil_int(s16(font_data + 8) * scale),
        .descender = floor_int(s16(font_data + 10) * scale),
        .line_height =
            ceil_int((s16(font_data + 8) - s16(font_data + 10) + s16(font_data + 12)) * scale),
    };
    if (metrics.advance < 1)
        metrics.advance = 1;
    if (metrics.line_height < 1)
        metrics.line_height = 1;
    return metrics;
}

struct tiny_font_metrics tiny_font_metrics(const struct tiny_font *font)
{
    return tiny_font_metrics_at(font->pixels64);
}

static const unsigned char *lookup(uint32_t cp)
{
    unsigned low = 0, high = u16(font_data + 16);
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        const unsigned char *record = font_data + HEADER_BYTES + middle * RECORD_BYTES;
        uint32_t value = u32(record);
        if (cp == value)
            return record;
        if (cp < value)
            high = middle;
        else
            low = middle + 1;
    }
    return NULL;
}

static bool edge(struct workspace *work, float x0, float y0, float x1, float y1)
{
    if (y0 == y1)
        return true;
    if (work->count == MAX_EDGES)
        return false;
    work->edges[work->count++] = (struct edge){x0, y0, x1, y1};
    return true;
}

static bool quadratic(struct workspace *work, float x0, float y0, float cx, float cy, float x1,
                      float y1, unsigned depth)
{
    /* Midpoint deviation <= 1/8 physical pixel, with an explicit depth bound. */
    if (depth == 10 || (absolute(x0 - 2 * cx + x1) <= 0.5f && absolute(y0 - 2 * cy + y1) <= 0.5f))
        return edge(work, x0, y0, x1, y1);
    float ax = (x0 + cx) / 2, ay = (y0 + cy) / 2;
    float bx = (cx + x1) / 2, by = (cy + y1) / 2;
    float mx = (ax + bx) / 2, my = (ay + by) / 2;
    return quadratic(work, x0, y0, ax, ay, mx, my, depth + 1) &&
           quadratic(work, mx, my, bx, by, x1, y1, depth + 1);
}

static bool flatten(struct workspace *work, const unsigned char *record, float scale, int left,
                    int top)
{
    const unsigned char *p = font_data + u32(font_data + 24) + u32(record + 4);
    float x = 0, y = 0, start_x = 0, start_y = 0;
    work->count = 0;
    for (unsigned i = 0; i < u16(record + 8); ++i) {
        unsigned opcode = *p++;
        if (opcode == 4) {
            if (!edge(work, x, y, start_x, start_y))
                return false;
            x = start_x;
            y = start_y;
            continue;
        }
        float px = s16(p) * scale - left, py = top - s16(p + 2) * scale;
        p += 4;
        if (opcode == 1) {
            x = start_x = px;
            y = start_y = py;
        } else if (opcode == 2) {
            if (!edge(work, x, y, px, py))
                return false;
            x = px;
            y = py;
        } else {
            float end_x = s16(p) * scale - left, end_y = top - s16(p + 2) * scale;
            p += 4;
            if (!quadratic(work, x, y, px, py, end_x, end_y, 0))
                return false;
            x = end_x;
            y = end_y;
        }
    }
    return true;
}

static void span(uint8_t *row, int width, float x0, float x1)
{
    int begin = ceil_int(x0 * SAMPLES - 0.5f), end = ceil_int(x1 * SAMPLES - 0.5f);
    if (begin < 0)
        begin = 0;
    if (end > width * SAMPLES)
        end = width * SAMPLES;
    while (begin < end) {
        int pixel = begin / SAMPLES;
        int next = (pixel + 1) * SAMPLES;
        if (next > end)
            next = end;
        row[pixel] += (uint8_t)(next - begin);
        begin = next;
    }
}

static void rasterize(struct workspace *work, uint8_t *alpha, int width, int height)
{
    memset(alpha, 0, (size_t)width * height);
    for (int row = 0; row < height; ++row) {
        for (int sample = 0; sample < SAMPLES; ++sample) {
            float y = row + (sample + 0.5f) / SAMPLES;
            unsigned count = 0;
            for (unsigned i = 0; i < work->count; ++i) {
                struct edge e = work->edges[i];
                /* Half-open intervals avoid counting contour vertices twice. */
                if ((e.y0 <= y && y < e.y1) || (e.y1 <= y && y < e.y0)) {
                    work->crossings[count++] = (struct crossing){
                        e.x0 + (y - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0),
                        e.y1 > e.y0 ? 1 : -1,
                    };
                }
            }
            /* In-place sort: small crossing lists, no libc sorting allocations. */
            for (unsigned i = 1; i < count; ++i) {
                struct crossing value = work->crossings[i];
                unsigned at = i;
                while (at && work->crossings[at - 1].x > value.x) {
                    work->crossings[at] = work->crossings[at - 1];
                    --at;
                }
                work->crossings[at] = value;
            }
            int winding = 0;
            float start = 0;
            for (unsigned i = 0; i < count; ++i) {
                if (!winding)
                    start = work->crossings[i].x;
                winding += work->crossings[i].direction;
                if (!winding)
                    span(alpha + (size_t)row * width, width, start, work->crossings[i].x);
            }
        }
    }
    for (int i = 0; i < width * height; ++i)
        alpha[i] = (uint8_t)((alpha[i] * 255u + SAMPLES * SAMPLES / 2) / (SAMPLES * SAMPLES));
}

const struct tiny_glyph *tiny_font_glyph(struct tiny_font *font, uint32_t cp)
{
    if (!font || !font->pixels64)
        return NULL;
    const unsigned char *record = lookup(cp);
    if (!record)
        record = lookup(0xfffd);
    if (!record)
        return NULL;
    cp = u32(record);
    for (unsigned i = 0; i < font->count; ++i)
        if (font->entries[i].codepoint == cp && font->entries[i].pixels64 == font->pixels64)
            return &font->entries[i].glyph;
    float scale = (float)font->pixels64 / (64 * u16(font_data + 4));
    int left = floor_int(s16(record + 12) * scale), top = ceil_int(s16(record + 18) * scale);
    int width = ceil_int(s16(record + 16) * scale) - left;
    int height = top - floor_int(s16(record + 14) * scale);
    if (width < 0 || height < 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
        return NULL;
    unsigned bytes = (unsigned)(width * height);
    if (bytes > CACHE_BYTES)
        return NULL;
    if (font->count == CACHE_SLOTS || bytes > CACHE_BYTES - font->used)
        font->count = font->used = 0; /* Simple bounded arena: flush, never grow. */
    uint8_t *alpha = font->cache + font->used;
    if (bytes) {
        if (!font->workspace) {
            void *p = platform_allocate_zeroed(sizeof(*font->workspace));
            if (!p)
                return NULL;
            font->workspace = p;
        }
        if (!flatten(font->workspace, record, scale, left, top))
            return NULL;
        rasterize(font->workspace, alpha, width, height);
    }
    struct entry *entry = &font->entries[font->count++];
    *entry = (struct entry){cp, font->pixels64, {width, height, left, top, alpha}};
    font->used += bytes;
    return &entry->glyph;
}
