#ifndef IMAGE_H
#define IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct image {
    unsigned char *rgba;
    int width, height;
};
bool image_info(const unsigned char *data, size_t length, int *width, int *height);
bool image_decode(struct image *image, const unsigned char *data, size_t length);
unsigned char *image_png(const unsigned char *rgba, int width, int height, size_t *length);
void image_free(struct image *image);
#endif
