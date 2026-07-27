/* mem.h -- arena allocator, checked dynamic array, allocation accounting and
 *          deterministic allocation-failure injection.
 *
 * Layer 0. Portable C17.
 *
 * Two allocation strategies, chosen by lifetime:
 *
 *   TgArena  Bump allocator over chained blocks. For generation-phase scratch
 *            and for immutable result data whose total size is known or bounded.
 *            Freed wholesale. No individual free. Supports marks for
 *            stage-local scratch reuse.
 *
 *   TgArray  Growable, contiguous, element-typed array backed by the system
 *            allocator. For collections whose final count is not known while
 *            they are being built (organs, vertices, indices). Because the
 *            backing pointer moves on growth, callers must address elements by
 *            index, never by cached pointer -- this is a project-wide rule.
 *
 * All sizes are computed with checked arithmetic. Every allocation is checked.
 */
#ifndef TG_MEM_H
#define TG_MEM_H

#include "core_types.h"

/* --------------------------------------------------------------------------
 * Global allocation accounting and failure injection.
 * -------------------------------------------------------------------------- */
typedef struct TgAllocStats {
    u64 live_bytes;      /* currently held by arenas + arrays              */
    u64 peak_bytes;      /* high-water mark since tg_mem_reset_stats       */
    u64 total_requests;  /* successful backing allocations                 */
    u64 failed_requests; /* refused: OOM, overflow, or injected failure    */
} TgAllocStats;

void tg_mem_reset_stats(void);
TgAllocStats tg_mem_stats(void);

/* Deterministic allocation-failure injection for adversarial tests.
 * After `n` further successful backing allocations, the next one fails.
 * `n == UINT64_MAX` (the default) disables injection.
 * This is a test instrument; production code never calls it. */
void tg_mem_set_failure_after(u64 n);
u64  tg_mem_failure_countdown(void);

/* --------------------------------------------------------------------------
 * Arena.
 * -------------------------------------------------------------------------- */
typedef struct TgArenaBlock TgArenaBlock;

typedef struct TgArena {
    TgArenaBlock *head;        /* most recent block                          */
    u64           block_size;  /* default size for new blocks                */
    u64           bytes_used;  /* sum of used bytes across blocks            */
    u64           bytes_owned; /* sum of block capacities                    */
    const char   *tag;
} TgArena;

/* A restore point. Only valid for the arena it came from, and only until an
 * older mark is restored. */
typedef struct TgArenaMark {
    TgArenaBlock *block;
    u64           used;
} TgArenaMark;

void     tg_arena_init(TgArena *a, u64 block_size, const char *tag);
void     tg_arena_release(TgArena *a);
/* Frees every block but the first and rewinds it, so the arena can be reused
 * without returning memory to the system. */
void     tg_arena_reset(TgArena *a);
void    *tg_arena_alloc(TgArena *a, u64 size, u64 align);
void    *tg_arena_alloc_zero(TgArena *a, u64 size, u64 align);
/* count * elem_size with overflow check. Returns NULL on overflow. */
void    *tg_arena_alloc_array(TgArena *a, u64 count, u64 elem_size, u64 align);
TgArenaMark tg_arena_mark(const TgArena *a);
void     tg_arena_restore(TgArena *a, TgArenaMark mark);

#define TG_ARENA_NEW(arena, T) \
    ((T *)tg_arena_alloc_zero((arena), sizeof(T), _Alignof(T)))
#define TG_ARENA_NEW_ARRAY(arena, T, n) \
    ((T *)tg_arena_alloc_array((arena), (u64)(n), sizeof(T), _Alignof(T)))

/* --------------------------------------------------------------------------
 * Dynamic array.
 * -------------------------------------------------------------------------- */
typedef struct TgArray {
    void       *data;
    u64         count;
    u64         capacity;
    u32         elem_size;
    u32         elem_align;
    const char *tag;
} TgArray;

TgResult tg_array_init(TgArray *arr, u32 elem_size, u32 elem_align,
                       u64 initial_capacity, const char *tag);
void     tg_array_free(TgArray *arr);
/* Keeps capacity, sets count to 0. Does not scrub memory. */
void     tg_array_clear(TgArray *arr);
TgResult tg_array_reserve(TgArray *arr, u64 min_capacity);
/* Appends `n` elements copied from `elems` (may be NULL to append zeroed
 * elements). Writes the index of the first appended element to *out_first. */
TgResult tg_array_push_n(TgArray *arr, const void *elems, u64 n, u64 *out_first);
TgResult tg_array_push(TgArray *arr, const void *elem, u64 *out_index);
/* Grows with zero fill, or shrinks (count only). */
TgResult tg_array_resize(TgArray *arr, u64 new_count);
/* Releases unused capacity. Never fails destructively: on failure the array is
 * left valid and unchanged. */
void     tg_array_shrink_to_fit(TgArray *arr);

static inline void *tg_array_at(const TgArray *arr, u64 index) {
    TG_ASSERT(arr != NULL);
    TG_ASSERT(index < arr->count);
    return (u8 *)arr->data + index * (u64)arr->elem_size;
}

static inline const void *tg_array_at_const(const TgArray *arr, u64 index) {
    TG_ASSERT(arr != NULL);
    TG_ASSERT(index < arr->count);
    return (const u8 *)arr->data + index * (u64)arr->elem_size;
}

#define TG_ARRAY_INIT(arr, T, cap, tag) \
    tg_array_init((arr), (u32)sizeof(T), (u32)_Alignof(T), (u64)(cap), (tag))
#define TG_ARRAY_AT(arr, T, i) (*(T *)tg_array_at((arr), (u64)(i)))
#define TG_ARRAY_DATA(arr, T)  ((T *)(arr)->data)

#endif /* TG_MEM_H */
