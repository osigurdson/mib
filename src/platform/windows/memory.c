#include "win32.h"
#include "platform.h"

void *platform_allocate_zeroed(size_t bytes)
{
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void platform_release(void *memory, size_t bytes)
{
    (void)bytes;
    if (memory)
        VirtualFree(memory, 0, MEM_RELEASE);
}

void platform_trim_heap(void)
{
    /* Font pages use VirtualFree directly; there is no CRT heap trim here. */
}
