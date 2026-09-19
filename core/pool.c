/*
 * pool.c — Static memory pool implementation
 * The arena lives in BSS — no malloc needed after startup.
 */
#include "pool.h"
#include <string.h>

/* The one and only arena — zero-initialized by the C runtime */
Arena arena;

static uint32_t scratch_used = 0;

void pool_scratch_reset(void) {
    scratch_used = 0;
}

uint8_t *pool_scratch_alloc(size_t n) {
    /* Align to 8 bytes */
    size_t aligned;
    if (n > SCRATCH_SIZE) return NULL;
    aligned = (n + 7u) & ~(size_t)7u;
    if (scratch_used + aligned > SCRATCH_SIZE) {
        return NULL; /* caller must handle this gracefully */
    }
    uint8_t *p = &arena.scratch[scratch_used];
    scratch_used += (uint32_t)aligned;
    return p;
}
