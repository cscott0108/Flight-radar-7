#pragma once
/* 0.1.6: when the most recent failed heap allocation happened.
 *
 * Written from the heap's failed-allocation callback (expert_debug.c FailedAllocHook), which runs in
 * whatever context the failing allocation ran in: any task, possibly an ISR, possibly with the flash
 * cache disabled, and possibly while the allocator is short of memory. The callback therefore must not
 * take locks, allocate, log or call time(): time()/gettimeofday() take a lock and are not in IRAM, so
 * a wall-clock read there is not safe. What it CAN do safely is read esp_timer_get_time() (uptime in
 * microseconds since boot: a systimer register read, placed in IRAM with CONFIG_ESP_TIMER_IN_IRAM=y,
 * no lock, no allocation) and store it. The wall-clock time is derived when /diag is rendered:
 * failure time = now - (uptime now - uptime at failure), shown only if the clock is synchronized then.
 *
 * The 64-bit value is stored as two 32-bit words with a sequence counter (odd while being written),
 * so a reader never shows a torn value; it retries a few times and otherwise reports "unavailable".
 * Header-only (static inline, always inlined into the IRAM callback) so host tests run the same code. */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t seq;   /* even: stable; odd: a write is in progress */
    volatile uint32_t lo, hi;
    volatile uint32_t valid; /* 1 once a time has been stored */
} FailedAllocStamp;

static inline __attribute__((always_inline)) void FailedAllocStamp_Record(FailedAllocStamp *s, int64_t uptimeUs)
{
    /* Explicit odd/even values (not two increments): even if failures on both cores interleave here,
     * the counter always ends even, so readers can never be locked out. */
    const uint32_t odd = s->seq | 1u;
    s->seq = odd;
    s->lo = (uint32_t)((uint64_t)uptimeUs & 0xFFFFFFFFu);
    s->hi = (uint32_t)((uint64_t)uptimeUs >> 32);
    s->valid = 1u;
    s->seq = odd + 1u;
}

/* True with the stored uptime; false if none was stored or no consistent value could be read. */
static inline bool FailedAllocStamp_Read(const FailedAllocStamp *s, int64_t *uptimeUs)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t a = s->seq;
        if (a & 1u)
            continue;
        const uint32_t lo = s->lo, hi = s->hi, valid = s->valid;
        if (s->seq != a)
            continue;
        if (!valid)
            return false;
        *uptimeUs = (int64_t)(((uint64_t)hi << 32) | lo);
        return true;
    }
    return false;
}
