#include "radar01_utils.h"

void debug_hex_dump(char *str, uint8_t *pSrcBufVA, int SrcBufLen)
{
    if (RADAR01_HEX_DEBUG_ENABLE == 0)
        return;
    uint8_t *pt;
    int x;
    pt = pSrcBufVA;
    debug_print("%s: %p, len = %d\n", str, pSrcBufVA, SrcBufLen);
    for (x = 0; x < SrcBufLen; x++) {
        if (x % 16 == 0) {
            debug_print("0x%04x : ", x);
        }
        debug_print("%02x ", ((uint8_t) pt[x]));
        if (x % 16 == 15) {
            debug_print("\n");
        }
    }
    debug_print("\n");
}

static struct timespec g_timestamp;
void clock_get_hw_time(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

double get_curr_time(void)
{
    clock_get_hw_time(&g_timestamp);
    return g_timestamp.tv_sec + (double) g_timestamp.tv_nsec / (double) BILLION;
}

void *radar01_alloc_mem(size_t size)
{
    return calloc(1, size);
}

void radar01_free_mem(void **ptr)
{
    if (*ptr) {
        free(*ptr);
        *ptr = NULL;
    }
    return;
}