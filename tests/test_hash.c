#include "test_util.h"
#include "test_suites.h"

#include "../src/core/hash.h"
#include "../src/core/rng.h"

#include <string.h>

static void test_hash_basics(void) {
    TG_T_CASE("byte hashing is order sensitive and content sensitive");
    {
        u64 a = tg_hash64_bytes(TG_FNV64_OFFSET, "abc", 3);
        u64 b = tg_hash64_bytes(TG_FNV64_OFFSET, "acb", 3);
        u64 c = tg_hash64_bytes(TG_FNV64_OFFSET, "abd", 3);
        TG_EXPECT(a != b);
        TG_EXPECT(a != c);
        TG_EXPECT_EQ_U64(a, tg_hash64_bytes(TG_FNV64_OFFSET, "abc", 3));
    }

    TG_T_CASE("string hashing matches byte hashing of the same content");
    TG_EXPECT_EQ_U64(tg_hash64_string(TG_FNV64_OFFSET, "quercus"),
                     tg_hash64_bytes(TG_FNV64_OFFSET, "quercus", 7));

    TG_T_CASE("NULL inputs are tolerated and leave the hash unchanged");
    TG_EXPECT_EQ_U64(tg_hash64_bytes(1234, NULL, 99), 1234);
    TG_EXPECT_EQ_U64(tg_hash64_string(1234, NULL), 1234);

    TG_T_CASE("mix64 avalanches: flipping one input bit changes many output bits");
    {
        int bit;
        for (bit = 0; bit < 64; ++bit) {
            u64 a = tg_mix64(0x0123456789ABCDEFull);
            u64 b = tg_mix64(0x0123456789ABCDEFull ^ ((u64)1 << bit));
            u64 diff = a ^ b;
            int popcount = 0;
            int k;
            for (k = 0; k < 64; ++k) { popcount += (int)((diff >> k) & 1u); }
            TG_EXPECT_MSG(popcount >= 16 && popcount <= 48,
                          "bit %d flipped %d output bits", bit, popcount);
        }
    }

    TG_T_CASE("small sequential integers map to well-spread hashes");
    {
        u32 buckets[16];
        u32 i;
        memset(buckets, 0, sizeof buckets);
        for (i = 0; i < 4096; ++i) {
            u64 h = tg_hash64_u32(TG_FNV64_OFFSET, i);
            buckets[(h >> 60) & 15u]++;
        }
        for (i = 0; i < 16; ++i) {
            TG_EXPECT_MSG(buckets[i] > 180 && buckets[i] < 330,
                          "bucket %u has %u of 4096", i, buckets[i]);
        }
    }
}

static void test_fingerprint(void) {
    TG_T_CASE("fingerprint is stable and content sensitive");
    {
        TgFingerprint a = tg_fp_begin(7);
        TgFingerprint b = tg_fp_begin(7);
        f32 data[4] = { 1.0f, 2.5f, -3.25f, 1e6f };
        tg_fp_add_f32_array(&a, data, 4);
        tg_fp_add_f32_array(&b, data, 4);
        TG_EXPECT(tg_fp_equal(a, b));
        TG_EXPECT_EQ_U64(a.element_count, 4);
        TG_EXPECT(!a.saw_non_finite);

        /* Mutate by exactly one ulp via the bit pattern. Writing a decimal
         * literal here would be wrong: -3.2500001 rounds back to -3.25 in
         * float, so the "changed" value would be bit-identical and the test
         * would be asserting nothing. */
        {
            u32 bits;
            TgFingerprint c;
            memcpy(&bits, &data[2], sizeof bits);
            bits += 1u;
            memcpy(&data[2], &bits, sizeof data[2]);
            TG_EXPECT_MSG(data[2] != -3.25f, "one-ulp mutation had no effect");
            c = tg_fp_begin(7);
            tg_fp_add_f32_array(&c, data, 4);
            TG_EXPECT_MSG(!tg_fp_equal(a, c),
                          "a one-ulp position change was not detected");
        }
    }

    TG_T_CASE("different salt gives a different fingerprint");
    {
        TgFingerprint a = tg_fp_begin(1);
        TgFingerprint b = tg_fp_begin(2);
        tg_fp_add_u32(&a, 5);
        tg_fp_add_u32(&b, 5);
        TG_EXPECT(!tg_fp_equal(a, b));
    }

    TG_T_CASE("negative zero is folded: signed zero must not read as a change");
    {
        TgFingerprint a = tg_fp_begin(0);
        TgFingerprint b = tg_fp_begin(0);
        tg_fp_add_f32(&a, 0.0f);
        tg_fp_add_f32(&b, -0.0f);
        TG_EXPECT_MSG(tg_fp_equal(a, b), "signed zero produced a false difference");
    }

    TG_T_CASE("non-finite values are flagged, not silently absorbed");
    {
        volatile f32 zero = 0.0f;
        TgFingerprint a = tg_fp_begin(0);
        tg_fp_add_f32(&a, 1.0f / zero);
        TG_EXPECT(a.saw_non_finite);
        {
            TgFingerprint b = tg_fp_begin(0);
            tg_fp_add_f32(&b, (0.0f / zero));
            TG_EXPECT(b.saw_non_finite);
            /* NaN and infinity must be distinguishable. */
            TG_EXPECT(!tg_fp_equal(a, b));
        }
    }

    TG_T_CASE("element count difference alone is a difference");
    {
        TgFingerprint a = tg_fp_begin(0);
        TgFingerprint b = tg_fp_begin(0);
        tg_fp_add_u32(&a, 0);
        TG_EXPECT(!tg_fp_equal(a, b));
    }

    TG_T_CASE("index reordering is detected");
    {
        u32 ia[3] = { 0, 1, 2 };
        u32 ib[3] = { 0, 2, 1 };
        TgFingerprint a = tg_fp_begin(9);
        TgFingerprint b = tg_fp_begin(9);
        tg_fp_add_u32_array(&a, ia, 3);
        tg_fp_add_u32_array(&b, ib, 3);
        TG_EXPECT_MSG(!tg_fp_equal(a, b), "triangle winding change not detected");
    }

    TG_T_CASE("fingerprint over a large array has no collisions under mutation");
    {
        /* Mutate each of 4096 floats by one ulp in turn and require a distinct
         * fingerprint every time. This is the property the static-object
         * regression test depends on. */
        enum { N = 4096 };
        static f32 base[N];
        TgRng r = tg_rng_substream(77, TG_RNG_GLOBAL, 0, 0);
        TgFingerprint ref;
        u64 seen[64];
        int i;
        for (i = 0; i < N; ++i) { base[i] = tg_rng_range(&r, -100.0f, 100.0f); }
        ref = tg_fp_begin(5);
        tg_fp_add_f32_array(&ref, base, N);

        /* Spot-check 64 positions rather than all 4096 to keep the suite fast. */
        for (i = 0; i < 64; ++i) {
            int idx = i * (N / 64);
            f32 saved = base[idx];
            u32 bits;
            TgFingerprint mutated;
            int j;
            memcpy(&bits, &saved, sizeof bits);
            bits += 1;
            memcpy(&base[idx], &bits, sizeof base[idx]);
            mutated = tg_fp_begin(5);
            tg_fp_add_f32_array(&mutated, base, N);
            TG_EXPECT_MSG(mutated.value != ref.value,
                          "one-ulp mutation at %d collided with the reference", idx);
            seen[i] = mutated.value;
            for (j = 0; j < i; ++j) {
                TG_EXPECT_MSG(seen[j] != seen[i],
                              "mutations at two positions collided");
            }
            base[idx] = saved;
        }
    }
}

void test_suite_hash(void) {
    test_hash_basics();
    test_fingerprint();
}
