/* The PNG encoder under memory pressure. This file includes src/image.c with a variable
 * encoder budget, and the Makefile builds it with assertions off and AddressSanitizer, the
 * configuration in which stb_image_write used to write past a buffer it failed to grow. */
#include <stddef.h>
static size_t limit;
#define IMAGE_ENCODE_LIMIT limit
#include "../src/image.c"
#include <stdio.h>

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "image-test: %s\n", what);
        exit(1);
    }
}

/* Noise doesn't compress, so the writer's output buffer keeps growing. */
static unsigned char *noise(int w, int h)
{
    unsigned char *rgba = malloc((size_t)w * h * 4);
    check(rgba != NULL, "out of memory");
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rgba[i] = (unsigned char)(i % 4 == 3 ? 255 : x);
    }
    return rgba;
}

static void round_trip(const unsigned char *png, size_t n, const unsigned char *rgba, int w, int h)
{
    struct image image = {0};
    check(image_decode(&image, png, n), "encoded PNG doesn't decode");
    check(image.width == w && image.height == h, "decoded size differs");
    check(!memcmp(image.rgba, rgba, (size_t)w * h * 4), "decoded pixels differ");
    image_free(&image);
}

int main(void)
{
    /* Every budget from nothing up to enough: each attempt either fails cleanly, leaving the
     * pool empty, or produces the same image. */
    int w = 256, h = 192;
    unsigned char *rgba = noise(w, h);
    bool encoded = false;
    unsigned failures = 0;
    for (limit = 0; !encoded; limit += 40 * 1024) {
        check(limit < 64u * 1024 * 1024, "never fit the budget");
        size_t n = 0;
        unsigned char *png = image_png(rgba, w, h, &n);
        check(!writer_blocks && !writer_used, "encoder memory left behind");
        if (png) {
            round_trip(png, n, rgba, w, h);
            free(png);
            encoded = true;
        } else
            ++failures;
    }
    check(failures > 8, "budget too large to exercise the failure path");
    free(rgba);

    /* The largest image mib accepts, uncompressible, fits the real budget. */
    limit = IMAGE_ENCODE_BUDGET;
    w = 2048, h = 4096;
    rgba = noise(w, h);
    size_t n = 0;
    unsigned char *png = image_png(rgba, w, h, &n);
    check(png != NULL, "8 megapixel noise didn't encode within the budget");
    check(!writer_blocks && !writer_used, "encoder memory left behind");
    int iw, ih;
    check(image_info(png, n, &iw, &ih) && iw == w && ih == h, "8 megapixel PNG unreadable");
    free(png);
    free(rgba);
    printf("PNG encoder: %u out-of-budget attempts failed cleanly; 8 MP noise fits\n", failures);
    return 0;
}
