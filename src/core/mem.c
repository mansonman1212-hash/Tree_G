#include "mem.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

#define TG_MEM_SUB "mem"

/* Hard ceiling on any single backing allocation. Protects against a corrupted
 * or adversarial count producing a request the platform would satisfy with
 * overcommit and then fault on. Reported as TG_ERR_LIMIT_EXCEEDED, not a crash. */
#define TG_MAX_SINGLE_ALLOC ((u64)3 * 1024 * 1024 * 1024)

/* Strictest alignment the arena will satisfy. 64 bytes covers a cache line and
 * every GPU-visible structure this engine uses. */
#define TG_ARENA_MAX_ALIGN ((u64)64)

static TgAllocStats g_stats;
static u64          g_fail_after = UINT64_MAX;

void tg_mem_reset_stats(void) {
    /* live_bytes is genuine outstanding state and must survive a stats reset,
     * otherwise peak tracking after the reset would be nonsense. */
    u64 live = g_stats.live_bytes;
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.live_bytes = live;
    g_stats.peak_bytes = live;
}

TgAllocStats tg_mem_stats(void) { return g_stats; }

void tg_mem_set_failure_after(u64 n) { g_fail_after = n; }
u64  tg_mem_failure_countdown(void) { return g_fail_after; }

static void account_alloc(u64 bytes) {
    g_stats.live_bytes += bytes;
    if (g_stats.live_bytes > g_stats.peak_bytes) {
        g_stats.peak_bytes = g_stats.live_bytes;
    }
    g_stats.total_requests++;
}

static void account_free(u64 bytes) {
    TG_ASSERT(g_stats.live_bytes >= bytes);
    g_stats.live_bytes -= bytes;
}

/* Single funnel for every backing allocation in the engine, so accounting and
 * failure injection cannot be bypassed. */
static void *backing_alloc(u64 bytes) {
    void *p;
    if (bytes == 0) { return NULL; }
    if (bytes > TG_MAX_SINGLE_ALLOC) {
        g_stats.failed_requests++;
        TG_LOG_ERRORF(TG_MEM_SUB,
                      "refused allocation of %llu bytes (cap %llu)",
                      (unsigned long long)bytes,
                      (unsigned long long)TG_MAX_SINGLE_ALLOC);
        return NULL;
    }
    if (g_fail_after != UINT64_MAX) {
        if (g_fail_after == 0) {
            g_stats.failed_requests++;
            return NULL;
        }
        g_fail_after--;
    }
    p = malloc((size_t)bytes);
    if (p == NULL) {
        g_stats.failed_requests++;
        return NULL;
    }
    account_alloc(bytes);
    return p;
}

static void backing_free(void *p, u64 bytes) {
    if (p == NULL) { return; }
    account_free(bytes);
    free(p);
}

/* ------------------------------------------------------------------------- */
/* Raw accounted allocation                                                  */
/* ------------------------------------------------------------------------- */

void *tg_alloc(u64 bytes) { return backing_alloc(bytes); }

void *tg_alloc_zero(u64 bytes) {
    void *p = backing_alloc(bytes);
    if (p != NULL) { memset(p, 0, (size_t)bytes); }
    return p;
}

void tg_free(void *p, u64 bytes) { backing_free(p, bytes); }

/* ------------------------------------------------------------------------- */
/* Arena                                                                     */
/* ------------------------------------------------------------------------- */

struct TgArenaBlock {
    TgArenaBlock *prev;
    u64           capacity; /* usable payload bytes                     */
    u64           used;
    u64           total;    /* header + payload, for accounting         */
    u8           *payload;
};

#define TG_ARENA_DEFAULT_BLOCK ((u64)1 << 20)

void tg_arena_init(TgArena *a, u64 block_size, const char *tag) {
    TG_CHECK(a != NULL);
    a->head = NULL;
    a->block_size = block_size != 0 ? block_size : TG_ARENA_DEFAULT_BLOCK;
    a->bytes_used = 0;
    a->bytes_owned = 0;
    a->tag = tag != NULL ? tag : "arena";
}

static TgArenaBlock *arena_new_block(TgArena *a, u64 min_payload) {
    u64 payload = a->block_size;
    u64 total, overhead;
    void *raw_mem;
    u8 *raw;
    u8 *aligned_payload;
    TgArenaBlock *b;

    /* malloc guarantees alignment suitable for any scalar (16 bytes on the
     * supported platforms), which is NOT enough for the 64-byte alignment the
     * arena advertises. So the payload pointer is explicitly aligned up inside
     * an over-allocated block rather than assumed. */
    overhead = sizeof(TgArenaBlock) + TG_ARENA_MAX_ALIGN;

    if (payload < min_payload) { payload = min_payload; }
    if (!tg_ckd_add_u64(payload, overhead, &total)) { return NULL; }

    raw_mem = backing_alloc(total);
    if (raw_mem == NULL) { return NULL; }
    /* Going through void* is required, not stylistic: casting u8* to
     * TgArenaBlock* directly is a stricter-alignment cast that the compiler
     * cannot prove safe. malloc's result is suitably aligned for any type. */
    b = (TgArenaBlock *)raw_mem;
    raw = (u8 *)raw_mem;

    {
        u64 addr = (u64)(uintptr_t)(raw + sizeof(TgArenaBlock));
        u64 aligned_addr;
        if (!tg_align_up_u64(addr, TG_ARENA_MAX_ALIGN, &aligned_addr)) {
            backing_free(raw_mem, total);
            return NULL;
        }
        aligned_payload = raw + (aligned_addr - (u64)(uintptr_t)raw);
        /* The alignment shift consumed part of the overhead; the remaining
         * usable payload is therefore total minus the actual offset. */
        payload = total - (u64)(aligned_payload - raw);
    }

    b->prev = a->head;
    b->capacity = payload;
    b->used = 0;
    b->total = total;
    b->payload = aligned_payload;

    TG_ASSERT(((u64)(uintptr_t)b->payload & (TG_ARENA_MAX_ALIGN - 1)) == 0);

    a->head = b;
    a->bytes_owned += payload;
    return b;
}

void tg_arena_release(TgArena *a) {
    TgArenaBlock *b;
    if (a == NULL) { return; }
    b = a->head;
    while (b != NULL) {
        TgArenaBlock *prev = b->prev;
        backing_free(b, b->total);
        b = prev;
    }
    a->head = NULL;
    a->bytes_used = 0;
    a->bytes_owned = 0;
}

void tg_arena_reset(TgArena *a) {
    TgArenaBlock *b;
    if (a == NULL) { return; }
    b = a->head;
    /* Keep the oldest block (the first allocated) to avoid churn. */
    while (b != NULL && b->prev != NULL) {
        TgArenaBlock *prev = b->prev;
        a->bytes_owned -= b->capacity;
        backing_free(b, b->total);
        b = prev;
    }
    a->head = b;
    if (b != NULL) { b->used = 0; }
    a->bytes_used = 0;
}

void *tg_arena_alloc(TgArena *a, u64 size, u64 align) {
    TgArenaBlock *b;
    u64 base, aligned, end;

    TG_CHECK(a != NULL);
    TG_CHECK(tg_is_power_of_two_u64(align));
    TG_CHECK_MSG(align <= TG_ARENA_MAX_ALIGN,
                 "arena alignment above TG_ARENA_MAX_ALIGN not supported");
    if (size == 0) { return NULL; }

    b = a->head;
    if (b != NULL) {
        base = (u64)b->used;
        if (!tg_align_up_u64(base, align, &aligned)) { return NULL; }
        if (tg_ckd_add_u64(aligned, size, &end) && end <= b->capacity) {
            b->used = end;
            a->bytes_used += size;
            return b->payload + aligned;
        }
    }

    /* Need a fresh block. Payload start is 64-aligned so `align` is satisfied
     * at offset 0. */
    b = arena_new_block(a, size);
    if (b == NULL) {
        TG_LOG_ERRORF(TG_MEM_SUB, "arena '%s': block allocation failed for %llu bytes",
                      a->tag, (unsigned long long)size);
        return NULL;
    }
    b->used = size;
    a->bytes_used += size;
    return b->payload;
}

void *tg_arena_alloc_zero(TgArena *a, u64 size, u64 align) {
    void *p = tg_arena_alloc(a, size, align);
    if (p != NULL) { memset(p, 0, (size_t)size); }
    return p;
}

void *tg_arena_alloc_array(TgArena *a, u64 count, u64 elem_size, u64 align) {
    u64 bytes;
    if (count == 0) { return NULL; }
    if (!tg_ckd_mul_u64(count, elem_size, &bytes)) {
        TG_LOG_ERRORF(TG_MEM_SUB, "arena '%s': count*size overflow (%llu * %llu)",
                      a != NULL ? a->tag : "?", (unsigned long long)count,
                      (unsigned long long)elem_size);
        return NULL;
    }
    return tg_arena_alloc_zero(a, bytes, align);
}

TgArenaMark tg_arena_mark(const TgArena *a) {
    TgArenaMark m;
    TG_CHECK(a != NULL);
    m.block = a->head;
    m.used = (a->head != NULL) ? a->head->used : 0;
    return m;
}

void tg_arena_restore(TgArena *a, TgArenaMark mark) {
    TgArenaBlock *b;
    TG_CHECK(a != NULL);
    b = a->head;
    while (b != NULL && b != mark.block) {
        TgArenaBlock *prev = b->prev;
        a->bytes_owned -= b->capacity;
        backing_free(b, b->total);
        b = prev;
    }
    /* If mark.block was NULL the arena had no blocks when marked; releasing
     * everything above is correct and `b` is now NULL. */
    a->head = b;
    if (b != NULL) {
        TG_ASSERT(mark.used <= b->used);
        b->used = mark.used;
    }
    /* bytes_used is a coarse counter; recompute conservatively. */
    a->bytes_used = 0;
    for (b = a->head; b != NULL; b = b->prev) { a->bytes_used += b->used; }
}

/* ------------------------------------------------------------------------- */
/* Dynamic array                                                             */
/* ------------------------------------------------------------------------- */

TgResult tg_array_init(TgArray *arr, u32 elem_size, u32 elem_align,
                       u64 initial_capacity, const char *tag) {
    TG_CHECK(arr != NULL);
    TG_CHECK(elem_size != 0);
    TG_CHECK(tg_is_power_of_two_u64(elem_align));

    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elem_size = elem_size;
    arr->elem_align = elem_align;
    arr->tag = tag != NULL ? tag : "array";

    if (initial_capacity == 0) { return TG_OK; }
    return tg_array_reserve(arr, initial_capacity);
}

void tg_array_free(TgArray *arr) {
    u64 bytes;
    if (arr == NULL) { return; }
    if (arr->data != NULL) {
        bytes = arr->capacity * (u64)arr->elem_size;
        backing_free(arr->data, bytes);
    }
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

void tg_array_clear(TgArray *arr) {
    TG_CHECK(arr != NULL);
    arr->count = 0;
}

TgResult tg_array_reserve(TgArray *arr, u64 min_capacity) {
    u64 new_cap, new_bytes, old_bytes;
    void *p;

    TG_CHECK(arr != NULL);
    if (min_capacity <= arr->capacity) { return TG_OK; }

    /* 1.5x growth, floored at 16 elements, never below the request. */
    new_cap = arr->capacity + (arr->capacity >> 1);
    if (new_cap < 16) { new_cap = 16; }
    if (new_cap < min_capacity) { new_cap = min_capacity; }

    if (!tg_ckd_mul_u64(new_cap, (u64)arr->elem_size, &new_bytes)) {
        TG_LOG_ERRORF(TG_MEM_SUB, "array '%s': capacity overflow at %llu elements",
                      arr->tag, (unsigned long long)new_cap);
        return TG_ERR_OVERFLOW;
    }

    /* Allocate-then-copy rather than realloc so that a failure leaves the
     * existing array completely intact (directive: a failed operation must not
     * destroy valid state). malloc also guarantees suitable alignment for any
     * scalar; we assert the element alignment is within that guarantee. */
    TG_CHECK_MSG(arr->elem_align <= _Alignof(max_align_t),
                 "array element alignment exceeds malloc guarantee");

    p = backing_alloc(new_bytes);
    if (p == NULL) {
        TG_LOG_ERRORF(TG_MEM_SUB, "array '%s': allocation of %llu bytes failed",
                      arr->tag, (unsigned long long)new_bytes);
        return TG_ERR_OUT_OF_MEMORY;
    }
    if (arr->data != NULL) {
        old_bytes = arr->count * (u64)arr->elem_size;
        memcpy(p, arr->data, (size_t)old_bytes);
        backing_free(arr->data, arr->capacity * (u64)arr->elem_size);
    }
    arr->data = p;
    arr->capacity = new_cap;
    return TG_OK;
}

TgResult tg_array_push_n(TgArray *arr, const void *elems, u64 n, u64 *out_first) {
    u64 needed;
    TgResult r;

    TG_CHECK(arr != NULL);
    if (n == 0) {
        if (out_first != NULL) { *out_first = arr->count; }
        return TG_OK;
    }
    if (!tg_ckd_add_u64(arr->count, n, &needed)) { return TG_ERR_OVERFLOW; }
    r = tg_array_reserve(arr, needed);
    if (r != TG_OK) { return r; }

    {
        u8 *dst = (u8 *)arr->data + arr->count * (u64)arr->elem_size;
        u64 bytes = n * (u64)arr->elem_size; /* cannot overflow: reserve checked */
        if (elems != NULL) {
            memcpy(dst, elems, (size_t)bytes);
        } else {
            memset(dst, 0, (size_t)bytes);
        }
    }
    if (out_first != NULL) { *out_first = arr->count; }
    arr->count = needed;
    return TG_OK;
}

TgResult tg_array_push(TgArray *arr, const void *elem, u64 *out_index) {
    return tg_array_push_n(arr, elem, 1, out_index);
}

TgResult tg_array_resize(TgArray *arr, u64 new_count) {
    TG_CHECK(arr != NULL);
    if (new_count <= arr->count) {
        arr->count = new_count;
        return TG_OK;
    }
    return tg_array_push_n(arr, NULL, new_count - arr->count, NULL);
}

void tg_array_shrink_to_fit(TgArray *arr) {
    u64 bytes;
    void *p;
    TG_CHECK(arr != NULL);
    if (arr->count == arr->capacity) { return; }
    if (arr->count == 0) {
        tg_array_free(arr);
        return;
    }
    bytes = arr->count * (u64)arr->elem_size;
    p = backing_alloc(bytes);
    if (p == NULL) { return; } /* keep oversized buffer; not an error */
    memcpy(p, arr->data, (size_t)bytes);
    backing_free(arr->data, arr->capacity * (u64)arr->elem_size);
    arr->data = p;
    arr->capacity = arr->count;
}
