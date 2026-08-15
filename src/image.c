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
#define STBI_MALLOC codec_alloc
#define STBI_REALLOC codec_realloc
#define STBI_FREE codec_free
#define STBIW_MALLOC codec_alloc
#define STBIW_REALLOC codec_realloc
#define STBIW_FREE codec_free
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
    int bytes;
    unsigned char *png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &bytes);
    if (!png)
        return NULL;
    unsigned char *copy = malloc((size_t)bytes);
    if (copy) {
        memcpy(copy, png, (size_t)bytes);
        *length = (size_t)bytes;
    }
    codec_free(png);
    return copy;
}
void image_free(struct image *image)
{
    stbi_image_free(image->rgba);
    *image = (struct image){0};
}
