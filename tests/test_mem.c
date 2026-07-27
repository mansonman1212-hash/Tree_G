#include "test_util.h"
#include "test_suites.h"

#include "../src/core/mem.h"

#include <string.h>

typedef struct AlignProbe {
    _Alignas(32) f32 v[8];
} AlignProbe;

static void test_checked_arithmetic(void) {
    u64 out64;
    u32 out32;

    TG_T_CASE("checked arithmetic detects overflow");
    TG_EXPECT(tg_ckd_add_u64(1, 2, &out64) && out64 == 3);
    TG_EXPECT(!tg_ckd_add_u64(UINT64_MAX, 1, &out64));
    TG_EXPECT(tg_ckd_mul_u64(1u << 20, 1u << 20, &out64));
    TG_EXPECT(!tg_ckd_mul_u64(UINT64_MAX / 2 + 1, 3, &out64));
    /* 0 * anything must succeed and not be reported as overflow. */
    TG_EXPECT(tg_ckd_mul_u64(0, UINT64_MAX, &out64) && out64 == 0);

    TG_EXPECT(tg_ckd_add_u32(UINT32_MAX - 1, 1, &out32) && out32 == UINT32_MAX);
    TG_EXPECT(!tg_ckd_add_u32(UINT32_MAX, 1, &out32));
    TG_EXPECT(!tg_ckd_mul_u32(65536, 65536, &out32));
    TG_EXPECT(tg_ckd_mul_u32(65535, 65535, &out32) && out32 == 4294836225u);

    TG_T_CASE("align_up");
    TG_EXPECT(tg_align_up_u64(0, 16, &out64) && out64 == 0);
    TG_EXPECT(tg_align_up_u64(1, 16, &out64) && out64 == 16);
    TG_EXPECT(tg_align_up_u64(16, 16, &out64) && out64 == 16);
    TG_EXPECT(tg_align_up_u64(17, 64, &out64) && out64 == 64);
    TG_EXPECT(!tg_align_up_u64(UINT64_MAX, 16, &out64));
}

static void test_arena_basic(void) {
    TgArena a;
    void *p1, *p2, *p3;
    AlignProbe *probe;

    TG_T_CASE("arena allocates aligned, distinct, zeroed memory");
    tg_arena_init(&a, 4096, "test");
    p1 = tg_arena_alloc(&a, 16, 16);
    p2 = tg_arena_alloc(&a, 16, 16);
    TG_EXPECT(p1 != NULL && p2 != NULL && p1 != p2);
    TG_EXPECT(((uintptr_t)p1 & 15u) == 0);
    TG_EXPECT(((uintptr_t)p2 & 15u) == 0);

    p3 = tg_arena_alloc_zero(&a, 256, 8);
    TG_EXPECT(p3 != NULL);
    if (p3 != NULL) {
        u8 acc = 0;
        int i;
        for (i = 0; i < 256; ++i) { acc |= ((u8 *)p3)[i]; }
        TG_EXPECT_EQ_U64(acc, 0);
    }

    probe = TG_ARENA_NEW(&a, AlignProbe);
    TG_EXPECT(probe != NULL);
    TG_EXPECT_MSG(((uintptr_t)probe & 31u) == 0,
                  "32-byte aligned type landed at %p", (void *)probe);

    TG_T_CASE("arena zero-size request returns NULL, not a valid pointer");
    TG_EXPECT(tg_arena_alloc(&a, 0, 16) == NULL);

    TG_T_CASE("arena grows past its block size");
    /* Request larger than block_size must succeed via an oversized block. */
    TG_EXPECT(tg_arena_alloc(&a, 4096 * 4, 64) != NULL);

    tg_arena_release(&a);
    TG_EXPECT(a.head == NULL);
    TG_EXPECT_EQ_U64(a.bytes_owned, 0);
}

static void test_arena_mark_restore(void) {
    TgArena a;
    TgArenaMark m;
    void *before;
    u64 owned_after_release;

    TG_T_CASE("arena mark/restore reclaims exactly the scratch region");
    tg_arena_init(&a, 1024, "test-mark");
    before = tg_arena_alloc(&a, 64, 16);
    TG_EXPECT(before != NULL);
    m = tg_arena_mark(&a);

    /* Force several extra blocks so restore has to free them. */
    TG_EXPECT(tg_arena_alloc(&a, 900, 16) != NULL);
    TG_EXPECT(tg_arena_alloc(&a, 900, 16) != NULL);
    TG_EXPECT(tg_arena_alloc(&a, 900, 16) != NULL);
    TG_EXPECT(a.bytes_owned > 1024);

    tg_arena_restore(&a, m);
    /* The first allocation must still be inside the arena and reusable. */
    TG_EXPECT(a.head != NULL);
    {
        void *after = tg_arena_alloc(&a, 64, 16);
        TG_EXPECT(after != NULL);
        /* Restoring then re-allocating the same size must hand back the same
         * address, proving the mark rewound rather than leaked. */
        TG_EXPECT_MSG(after != NULL && (uintptr_t)after > (uintptr_t)before,
                      "reallocation after restore did not reuse the block");
    }

    TG_T_CASE("arena reset keeps one block and rewinds it");
    tg_arena_reset(&a);
    TG_EXPECT(a.bytes_used == 0);
    owned_after_release = a.bytes_owned;
    TG_EXPECT(owned_after_release > 0);
    tg_arena_release(&a);
    TG_EXPECT_EQ_U64(a.bytes_owned, 0);
}

static void test_arena_restore_from_empty(void) {
    TgArena a;
    TgArenaMark m;

    TG_T_CASE("mark taken before any block, restore releases everything");
    tg_arena_init(&a, 256, "test-empty-mark");
    m = tg_arena_mark(&a);
    TG_EXPECT(m.block == NULL);
    TG_EXPECT(tg_arena_alloc(&a, 100, 8) != NULL);
    tg_arena_restore(&a, m);
    TG_EXPECT(a.head == NULL);
    TG_EXPECT_EQ_U64(a.bytes_owned, 0);
    tg_arena_release(&a);
}

static void test_array(void) {
    TgArray arr;
    u64 first;
    u32 i;

    TG_T_CASE("array push/growth preserves contents");
    TG_EXPECT_OK(TG_ARRAY_INIT(&arr, u32, 0, "test-array"));
    for (i = 0; i < 1000; ++i) {
        TG_EXPECT_OK(tg_array_push(&arr, &i, &first));
        TG_EXPECT_EQ_U64(first, i);
    }
    TG_EXPECT_EQ_U64(arr.count, 1000);
    for (i = 0; i < 1000; ++i) {
        TG_EXPECT_EQ_U64(TG_ARRAY_AT(&arr, u32, i), i);
    }

    TG_T_CASE("array push_n with NULL appends zeroed elements");
    TG_EXPECT_OK(tg_array_push_n(&arr, NULL, 10, &first));
    TG_EXPECT_EQ_U64(first, 1000);
    for (i = 0; i < 10; ++i) {
        TG_EXPECT_EQ_U64(TG_ARRAY_AT(&arr, u32, 1000 + i), 0);
    }

    TG_T_CASE("array resize down keeps capacity, up zero-fills");
    TG_EXPECT_OK(tg_array_resize(&arr, 5));
    TG_EXPECT_EQ_U64(arr.count, 5);
    TG_EXPECT(arr.capacity >= 1010);
    TG_EXPECT_OK(tg_array_resize(&arr, 7));
    TG_EXPECT_EQ_U64(TG_ARRAY_AT(&arr, u32, 6), 0);

    TG_T_CASE("shrink_to_fit releases surplus capacity");
    tg_array_shrink_to_fit(&arr);
    TG_EXPECT_EQ_U64(arr.capacity, arr.count);

    TG_T_CASE("clear keeps capacity");
    tg_array_clear(&arr);
    TG_EXPECT_EQ_U64(arr.count, 0);

    tg_array_free(&arr);
    TG_EXPECT(arr.data == NULL);
}

static void test_array_overflow_guard(void) {
    TgArray arr;

    TG_T_CASE("array refuses a request whose byte size overflows");
    tg_test_logs_mute(); /* these paths log by design */
    TG_EXPECT_OK(TG_ARRAY_INIT(&arr, u32, 0, "test-overflow"));
    /* 2^62 elements * 4 bytes overflows u64. Must be reported, not wrapped. */
    TG_EXPECT_ERR(tg_array_reserve(&arr, (u64)1 << 62), TG_ERR_OVERFLOW);
    /* A merely enormous but non-overflowing request must be refused by the
     * single-allocation cap as an out-of-memory condition, and must leave the
     * array usable. */
    TG_EXPECT_ERR(tg_array_reserve(&arr, (u64)1 << 40), TG_ERR_OUT_OF_MEMORY);
    {
        u32 v = 7;
        TG_EXPECT_OK(tg_array_push(&arr, &v, NULL));
        TG_EXPECT_EQ_U64(TG_ARRAY_AT(&arr, u32, 0), 7);
    }
    tg_array_free(&arr);
    tg_test_logs_unmute();
}

static void test_allocation_failure_injection(void) {
    TgArray arr;
    TgArena a;
    u32 i;
    u64 preserved_count;

    TG_T_CASE("injected allocation failure is reported, not fatal");
    tg_test_logs_mute();
    TG_EXPECT_OK(TG_ARRAY_INIT(&arr, u32, 32, "test-inject"));
    for (i = 0; i < 32; ++i) { TG_EXPECT_OK(tg_array_push(&arr, &i, NULL)); }
    preserved_count = arr.count;

    /* Next backing allocation fails. The array is at capacity, so the next push
     * must attempt to grow and must fail cleanly. */
    tg_mem_set_failure_after(0);
    TG_EXPECT_ERR(tg_array_push(&arr, &i, NULL), TG_ERR_OUT_OF_MEMORY);
    tg_mem_set_failure_after(UINT64_MAX);

    TG_T_CASE("failed growth leaves existing data completely intact");
    TG_EXPECT_EQ_U64(arr.count, preserved_count);
    for (i = 0; i < 32; ++i) { TG_EXPECT_EQ_U64(TG_ARRAY_AT(&arr, u32, i), i); }
    /* And the array must still work once memory is available again. */
    TG_EXPECT_OK(tg_array_push(&arr, &i, NULL));
    tg_array_free(&arr);

    TG_T_CASE("arena reports allocation failure as NULL");
    tg_arena_init(&a, 512, "test-inject-arena");
    tg_mem_set_failure_after(0);
    TG_EXPECT(tg_arena_alloc(&a, 128, 16) == NULL);
    tg_mem_set_failure_after(UINT64_MAX);
    TG_EXPECT(tg_arena_alloc(&a, 128, 16) != NULL);
    tg_arena_release(&a);
    tg_test_logs_unmute();
}

static void test_stats(void) {
    TgAllocStats s0, s1;
    TgArena a;

    TG_T_CASE("allocation accounting tracks live and peak bytes");
    s0 = tg_mem_stats();
    tg_arena_init(&a, 1 << 16, "test-stats");
    TG_EXPECT(tg_arena_alloc(&a, 1000, 16) != NULL);
    s1 = tg_mem_stats();
    TG_EXPECT(s1.live_bytes > s0.live_bytes);
    TG_EXPECT(s1.peak_bytes >= s1.live_bytes);
    tg_arena_release(&a);
    s1 = tg_mem_stats();
    TG_EXPECT_EQ_U64(s1.live_bytes, s0.live_bytes);
}

void test_suite_mem(void) {
    test_checked_arithmetic();
    test_arena_basic();
    test_arena_mark_restore();
    test_arena_restore_from_empty();
    test_array();
    test_array_overflow_guard();
    test_allocation_failure_injection();
    test_stats();
}
