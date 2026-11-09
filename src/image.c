#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Codec allocations carry their sizes so malformed compressed input cannot
   grow beyond a 64 MiB transient budget. The UI is single-threaded. */
union allocation {
    max_align_t align;
    size_t size;
};
static size_t codec_used;
static void *codec_alloc(size_t size)
{
    if (size > 64 * 1024 * 1024 - codec_used || size > SIZE_MAX - sizeof(union allocation))
        return NULL;
    union allocation *p = malloc(sizeof(*p) + size);
    if (!p)
        return NULL;
    p->size = size;
    codec_used += size;
    return p + 1;
}
static void codec_free(void *ptr)
{
    if (!ptr)
        return;
    union allocation *p = (union allocation *)ptr - 1;
    codec_used -= p->size;
    free(p);
}
static void *codec_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return codec_alloc(size);
    union allocation *old = (union allocation *)ptr - 1;
    size_t before = old->size;
    if (size > 64 * 1024 * 1024 - (codec_used - before) || size > SIZE_MAX - sizeof(*old))
        return NULL;
    union allocation *p = realloc(old, sizeof(*p) + size);
    if (!p)
        return NULL;
    p->size = size;
    codec_used = codec_used - before + size;
    return p + 1;
}
/* The PNG writer asserts that its buffers grew, but with assertions off it carries on and
 * writes past the old buffer. Its allocations come from this pool instead, which never returns
 * NULL: running out, or any writer assertion failing, jumps back to image_png, and image_png
 * frees whatever the writer still holds. An 8 megapixel image that doesn't compress at all
 * peaks at about 83 MiB, so within the budget the jump is only taken when memory runs out.
 * tests/image-test.c lowers IMAGE_ENCODE_LIMIT to exercise it. */
#define IMAGE_ENCODE_BUDGET (128u * 1024 * 1024)
#ifndef IMAGE_ENCODE_LIMIT
#define IMAGE_ENCODE_LIMIT IMAGE_ENCODE_BUDGET
#endif
union block {
    struct {
        union block *prev, *next;
        size_t size;
    } b;
    max_align_t align;
};
static union block *writer_blocks;
static size_t writer_used;
static jmp_buf *writer_escape;
static void writer_fail(void)
{
    if (writer_escape)
        longjmp(*writer_escape, 1);
    abort();
}
static void writer_link(union block *p, size_t size)
{
    p->b.size = size;
    p->b.prev = NULL;
    p->b.next = writer_blocks;
    if (writer_blocks)
        writer_blocks->b.prev = p;
    writer_blocks = p;
    writer_used += size;
}
static void writer_unlink(union block *p)
{
    if (p->b.prev)
        p->b.prev->b.next = p->b.next;
    else
        writer_blocks = p->b.next;
    if (p->b.next)
        p->b.next->b.prev = p->b.prev;
    writer_used -= p->b.size;
}
static void *writer_alloc(size_t size)
{
    union block *p = NULL;
    if (size <= IMAGE_ENCODE_LIMIT - writer_used && size <= SIZE_MAX - sizeof(*p))
        p = malloc(sizeof(*p) + size);
    if (!p)
        writer_fail();
    writer_link(p, size);
    return p + 1;
}
static void writer_free(void *ptr)
{
    if (!ptr)
        return;
    union block *p = (union block *)ptr - 1;
    writer_unlink(p);
    free(p);
}
static void *writer_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return writer_alloc(size);
    union block *old = (union block *)ptr - 1, *p = NULL;
    size_t before = old->b.size;
    writer_unlink(old);
    if (size <= IMAGE_ENCODE_LIMIT - writer_used && size <= SIZE_MAX - sizeof(*p))
        p = realloc(old, sizeof(*p) + size);
    if (!p) {
        writer_link(old, before);
        writer_fail();
    }
    writer_link(p, size);
    return p + 1;
}
#define STBI_MALLOC codec_alloc
#define STBI_REALLOC codec_realloc
#define STBI_FREE codec_free
#define STBIW_MALLOC writer_alloc
#define STBIW_REALLOC writer_realloc
#define STBIW_FREE writer_free
#define STBIW_ASSERT(x) ((x) ? (void)0 : writer_fail())
/* Independent codecs, only touched after a page references/pastes an image. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"
#include "image.h"
#include <limits.h>
bool image_info(const unsigned char *data, size_t length, int *w, int *h)
{
    int channels;
    return data && length <= INT_MAX && stbi_info_from_memory(data, (int)length, w, h, &channels) &&
           *w > 0 && *h > 0 && (uint64_t)*w * (unsigned)*h <= 8 * 1024 * 1024;
}
bool image_decode(struct image *image, const unsigned char *data, size_t length)
{
    int w, h, channels;
    if (!image_info(data, length, &w, &h))
        return false;
    unsigned char *rgba = stbi_load_from_memory(data, (int)length, &w, &h, &channels, 4);
    if (!rgba)
        return false;
    *image = (struct image){rgba, w, h};
    return true;
}
unsigned char *image_png(const unsigned char *rgba, int w, int h, size_t *length)
{
    if (!rgba || w < 1 || h < 1 || (uint64_t)w * (unsigned)h > 8 * 1024 * 1024)
        return NULL;
    jmp_buf escape;
    unsigned char *volatile copy = NULL;
    writer_escape = &escape;
    if (!setjmp(escape)) {
        int bytes;
        unsigned char *png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &bytes);
        if (png && bytes > 0 && (copy = malloc((size_t)bytes))) {
            memcpy(copy, png, (size_t)bytes);
            *length = (size_t)bytes;
        }
    }
    writer_escape = NULL;
    /* The encoded PNG, and on a jump everything else the writer had. */
    while (writer_blocks)
        writer_free(writer_blocks + 1);
    return copy;
}
void image_free(struct image *image)
{
    stbi_image_free(image->rgba);
    *image = (struct image){0};
}
