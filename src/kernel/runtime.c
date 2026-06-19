#include "uefi.h"

void *memcpy(void *destination, const void *source, UINTN count)
{
    UINT8 *output = (UINT8 *)destination;
    const UINT8 *input = (const UINT8 *)source;
    UINTN index;

    for (index = 0; index < count; index++) {
        output[index] = input[index];
    }

    return destination;
}

void *memset(void *destination, int value, UINTN count)
{
    UINT8 *output = (UINT8 *)destination;
    UINTN index;

    for (index = 0; index < count; index++) {
        output[index] = (UINT8)value;
    }

    return destination;
}

