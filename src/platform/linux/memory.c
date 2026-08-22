#define _GNU_SOURCE
#include "platform.h"
#include <sys/mman.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

void *platform_allocate_zeroed(size_t bytes)
{
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}

void platform_release(void *memory, size_t bytes)
{
    if (memory)
        munmap(memory, bytes);
}

void platform_trim_heap(void)
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}
