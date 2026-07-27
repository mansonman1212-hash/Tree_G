#include "test_util.h"
#include "test_suites.h"

#include "../src/core/rng.h"

#include <string.h>

static void test_determinism(void) {
    TgRng a, b;
    int i;

    TG_T_CASE("same seed and stream produce the same sequence");
    tg_rng_seed(&a, 12345, 7);
    tg_rng_seed(&b, 12345, 7);
    for (i = 0; i < 1000; ++i) {
        TG_EXPECT_EQ_U64(tg_rng_u32(&a), tg_rng_u32(&b));
    }

    TG_T_CASE("different stream produces a different sequence");
    tg_rng_seed(&a, 12345, 7);
    tg_rng_seed(&b, 12345, 8);
    {
        u32 same = 0;
        for (i = 0; i < 200; ++i) {
            if (tg_rng_u32(&a) == tg_rng_u32(&b)) { same++; }
        }
        TG_EXPECT_MSG(same < 3, "%u/200 values coincided between streams", same);
    }

    TG_T_CASE("substream depends on all four key components");
    {
        TgRng s0 = tg_rng_substream(1, TG_RNG_LEAF_SHAPE, 10, 0);
        TgRng s1 = tg_rng_substream(2, TG_RNG_LEAF_SHAPE, 10, 0);
        TgRng s2 = tg_rng_substream(1, TG_RNG_LEAF_DAMAGE, 10, 0);
        TgRng s3 = tg_rng_substream(1, TG_RNG_LEAF_SHAPE, 11, 0);
        TgRng s4 = tg_rng_substream(1, TG_RNG_LEAF_SHAPE, 10, 1);
        u32 v0 = tg_rng_u32(&s0);
        TG_EXPECT(v0 != tg_rng_u32(&s1));
        TG_EXPECT(v0 != tg_rng_u32(&s2));
        TG_EXPECT(v0 != tg_rng_u32(&s3));
        TG_EXPECT(v0 != tg_rng_u32(&s4));
    }

    TG_T_CASE("substream is order-independent: this is the parallelism contract");
    {
        /* Draw for ids 0..99 forward, then backward, and require identical
         * results. If this ever fails, a parallel stage can produce a different
         * tree depending on scheduling. */
        f32 forward[100];
        f32 backward[100];
        int k;
        for (k = 0; k < 100; ++k) {
            TgRng r = tg_rng_substream(0xBEEF, TG_RNG_LEAF_PLACEMENT, (u32)k, 0);
            forward[k] = tg_rng_f32(&r);
        }
        for (k = 99; k >= 0; --k) {
            TgRng r = tg_rng_substream(0xBEEF, TG_RNG_LEAF_PLACEMENT, (u32)k, 0);
            backward[k] = tg_rng_f32(&r);
        }
        for (k = 0; k < 100; ++k) {
            TG_EXPECT_MSG(forward[k] == backward[k],
                          "substream %d order-dependent: %.9g vs %.9g",
                          k, (double)forward[k], (double)backward[k]);
        }
    }

    TG_T_CASE("sequential ids do not produce correlated first draws");
    {
        /* A naive implementation that used the id directly as the PCG stream
         * yields visibly correlated first outputs. Require the first draws of
         * 4096 consecutive ids to be spread across all 16 buckets reasonably
         * evenly. */
        u32 buckets[16];
        u32 k;
        memset(buckets, 0, sizeof buckets);
        for (k = 0; k < 4096; ++k) {
            TgRng r = tg_rng_substream(1, TG_RNG_BUD_PLACEMENT, k, 0);
            buckets[tg_rng_u32(&r) >> 28]++;
        }
        for (k = 0; k < 16; ++k) {
            TG_EXPECT_MSG(buckets[k] > 180 && buckets[k] < 330,
                          "bucket %u has %u of 4096 (expected ~256)", k, buckets[k]);
        }
    }
}

static void test_float_range(void) {
    TgRng r;
    int i;
    f32 mn = 2.0f, mx = -1.0f;

    TG_T_CASE("f32 output is in [0,1) and never reaches 1.0");
    tg_rng_seed(&r, 999, 1);
    for (i = 0; i < 200000; ++i) {
        f32 v = tg_rng_f32(&r);
        if (v < mn) { mn = v; }
        if (v > mx) { mx = v; }
        if (!(v >= 0.0f && v < 1.0f)) {
            TG_EXPECT_MSG(false, "tg_rng_f32 produced %.9g", (double)v);
            break;
        }
    }
    TG_EXPECT(mn < 0.001f);
    TG_EXPECT(mx > 0.999f);
    TG_EXPECT(mx < 1.0f);

    TG_T_CASE("range respects bounds and degenerate input");
    for (i = 0; i < 10000; ++i) {
        f32 v = tg_rng_range(&r, -3.0f, 7.0f);
        TG_EXPECT(v >= -3.0f && v < 7.0f);
    }
    TG_EXPECT_NEAR(tg_rng_range(&r, 5.0f, 5.0f), 5.0f, 0.0);
    TG_EXPECT_NEAR(tg_rng_range(&r, 5.0f, 1.0f), 5.0f, 0.0);

    TG_T_CASE("signed output is in [-1,1)");
    for (i = 0; i < 10000; ++i) {
        f32 v = tg_rng_signed(&r);
        TG_EXPECT(v >= -1.0f && v < 1.0f);
    }
}

static void test_below_unbiased(void) {
    TgRng r;
    u32 counts[7];
    u32 i;
    const u32 n = 7;          /* not a power of two: the biased case */
    const u32 draws = 700000;

    TG_T_CASE("below(n) is in range and unbiased for non-power-of-two n");
    memset(counts, 0, sizeof counts);
    tg_rng_seed(&r, 4242, 3);
    for (i = 0; i < draws; ++i) {
        u32 v = tg_rng_below(&r, n);
        if (v >= n) {
            TG_EXPECT_MSG(false, "below(%u) returned %u", n, v);
            return;
        }
        counts[v]++;
    }
    for (i = 0; i < n; ++i) {
        f64 expected = (f64)draws / (f64)n;
        f64 dev = ((f64)counts[i] - expected) / expected;
        /* 5 sigma for this sample size is well under 2%; 2% is a generous gate
         * that still catches modulo bias, which for n=7 would show up as a
         * systematic skew of the low buckets. */
        TG_EXPECT_MSG(dev > -0.02 && dev < 0.02,
                      "bucket %u deviation %.4f (count %u, expected %.0f)",
                      i, dev, counts[i], expected);
    }

    TG_T_CASE("below(0) and below(1) are well defined");
    TG_EXPECT_EQ_U64(tg_rng_below(&r, 0), 0);
    TG_EXPECT_EQ_U64(tg_rng_below(&r, 1), 0);
}

static void test_normal(void) {
    TgRng r;
    int i;
    f64 sum = 0.0, sum_sq = 0.0;
    const int n = 200000;

    TG_T_CASE("normal has ~zero mean and ~unit variance");
    tg_rng_seed(&r, 31337, 5);
    for (i = 0; i < n; ++i) {
        f32 v = tg_rng_normal(&r);
        TG_EXPECT_MSG(tg_finitef(v), "normal produced non-finite value");
        sum += (f64)v;
        sum_sq += (f64)v * (f64)v;
    }
    {
        f64 mean = sum / (f64)n;
        f64 var = sum_sq / (f64)n - mean * mean;
        TG_EXPECT_MSG(mean > -0.02 && mean < 0.02, "mean %.5f", mean);
        TG_EXPECT_MSG(var > 0.96 && var < 1.04, "variance %.5f", var);
    }

    TG_T_CASE("normal_clamped honours the sigma bound");
    for (i = 0; i < 20000; ++i) {
        f32 v = tg_rng_normal_clamped(&r, 10.0f, 2.0f, 1.5f);
        TG_EXPECT(v >= 10.0f - 3.0f - 1e-4f && v <= 10.0f + 3.0f + 1e-4f);
    }
}

static void test_direction_sampling(void) {
    TgRng r;
    int i;
    V3 sum = { 0, 0, 0 };
    const int n = 100000;

    TG_T_CASE("unit_sphere yields unit vectors with no directional bias");
    tg_rng_seed(&r, 5150, 11);
    for (i = 0; i < n; ++i) {
        V3 v = tg_rng_unit_sphere(&r);
        TG_EXPECT_MSG(tg_nearf(v3_len(v), 1.0f, 1e-4f), "non-unit sphere sample");
        sum = v3_add(sum, v);
    }
    {
        /* The mean of a uniform sphere sample must approach zero. A biased
         * sampler (for example uniform angles) shows a systematic offset. */
        V3 mean = v3_scale(sum, 1.0f / (f32)n);
        TG_EXPECT_MSG(v3_len(mean) < 0.02f, "sphere sample mean length %.4f",
                      (double)v3_len(mean));
    }

    TG_T_CASE("in_unit_ball stays inside and fills the volume");
    {
        f64 r_sum = 0.0;
        for (i = 0; i < 50000; ++i) {
            V3 v = tg_rng_in_unit_ball(&r);
            f32 len = v3_len(v);
            TG_EXPECT_MSG(len <= 1.0f + 1e-5f, "ball sample length %.6f", (double)len);
            r_sum += (f64)len;
        }
        /* For a uniform ball the mean radius is 3/4. */
        TG_EXPECT_NEAR(r_sum / 50000.0, 0.75, 0.01);
    }

    TG_T_CASE("cone sampling stays within the cone and is solid-angle uniform");
    {
        V3 axis = v3_norm_or(v3(0.3f, 1.0f, -0.2f), v3(0, 1, 0));
        f64 cos_sum = 0.0;
        const f32 max_angle = 0.6f;
        for (i = 0; i < 50000; ++i) {
            V3 v = tg_rng_cone(&r, axis, max_angle);
            f32 ang = v3_angle_between(axis, v);
            TG_EXPECT_MSG(ang <= max_angle + 1e-3f, "cone sample at %.5f rad",
                          (double)ang);
            cos_sum += (f64)v3_dot(axis, v);
        }
        {
            /* For solid-angle-uniform sampling, cos(theta) is uniform in
             * [cos(max),1], so its mean is (1+cos(max))/2. A naive
             * uniform-angle sampler gives a noticeably higher mean. */
            f64 expected = (1.0 + (f64)cosf(max_angle)) * 0.5;
            TG_EXPECT_NEAR(cos_sum / 50000.0, expected, 0.005);
        }
    }

    TG_T_CASE("cone degenerate cases");
    TG_EXPECT_V3_NEAR(tg_rng_cone(&r, v3(0, 1, 0), 0.0f), v3(0, 1, 0), 1e-6);
    TG_EXPECT_NEAR(v3_len(tg_rng_cone(&r, v3(0, 1, 0), TG_PI_F)), 1.0f, 1e-4);
    TG_EXPECT(v3_finite(tg_rng_cone(&r, v3_zero(), 0.5f)));
}

static void test_chance(void) {
    TgRng r;
    int i;
    u32 hits = 0;

    TG_T_CASE("chance() frequency matches the requested probability");
    tg_rng_seed(&r, 271828, 13);
    for (i = 0; i < 100000; ++i) {
        if (tg_rng_chance(&r, 0.25f)) { hits++; }
    }
    TG_EXPECT_NEAR((f64)hits / 100000.0, 0.25, 0.01);

    TG_T_CASE("chance() boundary values are exact");
    for (i = 0; i < 100; ++i) {
        TG_EXPECT(!tg_rng_chance(&r, 0.0f));
        TG_EXPECT(!tg_rng_chance(&r, -1.0f));
        TG_EXPECT(tg_rng_chance(&r, 1.0f));
        TG_EXPECT(tg_rng_chance(&r, 2.0f));
    }
}

static void test_purpose_names(void) {
    int i;
    TG_T_CASE("every RNG purpose has a distinct name");
    for (i = 0; i < (int)TG_RNG_PURPOSE_COUNT; ++i) {
        const char *a = tg_rng_purpose_name((TgRngPurpose)i);
        int j;
        TG_EXPECT_MSG(strcmp(a, "unknown") != 0, "purpose %d has no name", i);
        for (j = i + 1; j < (int)TG_RNG_PURPOSE_COUNT; ++j) {
            const char *b = tg_rng_purpose_name((TgRngPurpose)j);
            TG_EXPECT_MSG(strcmp(a, b) != 0, "purposes %d and %d share name '%s'",
                          i, j, a);
        }
    }
}

void test_suite_rng(void) {
    test_determinism();
    test_float_range();
    test_below_unbiased();
    test_normal();
    test_direction_sampling();
    test_chance();
    test_purpose_names();
}
