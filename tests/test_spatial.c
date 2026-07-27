#include "test_util.h"
#include "test_suites.h"

#include "../src/core/rng.h"
#include "../src/geom/spatial.h"

#include <string.h>

/* Independent reference implementation. Every grid result is checked against a
 * brute-force scan rather than against the grid's own behaviour -- otherwise the
 * test would only prove the grid is self-consistent. */
static u32 brute_force_radius(const V3 *pts, u32 n, const bool *removed,
                              V3 center, f32 radius, u32 *out, u32 cap) {
    u32 i, count = 0;
    f32 r2 = radius * radius;
    for (i = 0; i < n; ++i) {
        if (removed != NULL && removed[i]) { continue; }
        if (v3_dist_sq(pts[i], center) <= r2) {
            if (count < cap) { out[count] = i; }
            count++;
        }
    }
    return count;
}

static u32 brute_force_nearest(const V3 *pts, u32 n, const bool *removed,
                               V3 center, f32 max_radius, f32 *out_d2) {
    u32 i, best = TG_INVALID_ID;
    f32 bd = 3.402823466e38f;
    f32 r2 = max_radius * max_radius;
    for (i = 0; i < n; ++i) {
        f32 d2;
        if (removed != NULL && removed[i]) { continue; }
        d2 = v3_dist_sq(pts[i], center);
        if (d2 > r2) { continue; }
        if (d2 < bd || (d2 == bd && i < best)) { bd = d2; best = i; }
    }
    if (out_d2 != NULL) { *out_d2 = (best == TG_INVALID_ID) ? 0.0f : bd; }
    return best;
}

static void test_empty_grid(void) {
    SpatialGrid g;
    u32 out[8];
    u32 count = 99, total = 99;

    TG_T_CASE("an empty grid is a valid queryable object");
    /* The generator legitimately exhausts its attraction points, and that must
     * not need a special case at every call site. */
    TG_EXPECT_OK(spatial_build(&g, NULL, 0, 0.0f, 0));
    TG_EXPECT_OK(spatial_query_radius(&g, v3_zero(), 1.0f, out, 8, &count, &total));
    TG_EXPECT_EQ_U64(count, 0);
    TG_EXPECT_EQ_U64(total, 0);
    TG_EXPECT_EQ_U64(spatial_nearest(&g, v3_zero(), 10.0f, NULL), TG_INVALID_ID);
    TG_EXPECT_EQ_U64(spatial_live_count(&g), 0);
    spatial_destroy(&g);
}

static void test_matches_brute_force(void) {
    enum { N = 4000, CAP = 256 };
    static V3 pts[N];
    static u32 got[CAP];
    static u32 want[CAP];
    SpatialGrid g;
    TgRng r = tg_rng_substream(4321, TG_RNG_ATTRACTOR_CLOUD, 0, 0);
    u32 i;

    /* Anisotropic cloud: 8 m tall, 5 m wide, 0.4 m thick. A cubic grid handles
     * this badly if the cell size or the neighbourhood scan is wrong, so it is a
     * more honest test than a uniform cube. */
    for (i = 0; i < N; ++i) {
        pts[i] = v3(tg_rng_range(&r, -2.5f, 2.5f),
                    tg_rng_range(&r, 0.0f, 8.0f),
                    tg_rng_range(&r, -0.2f, 0.2f));
    }

    TG_T_CASE("radius query matches a brute-force scan exactly");
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    for (i = 0; i < 200; ++i) {
        V3 c = v3(tg_rng_range(&r, -3.0f, 3.0f),
                  tg_rng_range(&r, -1.0f, 9.0f),
                  tg_rng_range(&r, -0.5f, 0.5f));
        f32 rad = tg_rng_range(&r, 0.05f, 1.5f);
        u32 gc = 0, gt = 0, wc;
        TG_EXPECT_OK(spatial_query_radius(&g, c, rad, got, CAP, &gc, &gt));
        wc = brute_force_radius(pts, N, NULL, c, rad, want, CAP);
        if (gt != wc) {
            TG_EXPECT_MSG(false, "query %u: grid found %u, brute force %u", i, gt, wc);
            continue;
        }
        {
            /* Aggregate comparison, one check per query, so that this suite's
             * total check count does not depend on the data. It otherwise
             * varies between compilers whenever a point lies exactly on the
             * query radius and rounds differently -- harmless in itself, but it
             * turns the check count into noise instead of a regression signal. */
            u32 k;
            u32 expect_stored = wc < CAP ? wc : CAP;
            u32 first_bad = TG_INVALID_ID;
            for (k = 0; k < expect_stored; ++k) {
                if (got[k] != want[k]) { first_bad = k; break; }
            }
            TG_EXPECT_EQ_U64(gc, expect_stored);
            TG_EXPECT_MSG(first_bad == TG_INVALID_ID,
                          "query %u element %u: grid %u, brute force %u", i,
                          first_bad,
                          first_bad == TG_INVALID_ID ? 0u : got[first_bad],
                          first_bad == TG_INVALID_ID ? 0u : want[first_bad]);
        }
    }

    TG_T_CASE("results are in ascending index order");
    {
        u32 gc = 0, gt = 0, k;
        TG_EXPECT_OK(spatial_query_radius(&g, v3(0, 4, 0), 2.0f, got, CAP, &gc, &gt));
        TG_EXPECT(gc > 8);
        for (k = 1; k < gc; ++k) {
            TG_EXPECT_MSG(got[k - 1] < got[k], "not ascending at %u", k);
        }
    }

    TG_T_CASE("nearest matches brute force including the tie-break rule");
    for (i = 0; i < 300; ++i) {
        V3 c = v3(tg_rng_range(&r, -3.0f, 3.0f),
                  tg_rng_range(&r, -1.0f, 9.0f),
                  tg_rng_range(&r, -0.5f, 0.5f));
        f32 rad = tg_rng_range(&r, 0.05f, 2.0f);
        f32 gd = 0.0f, wd = 0.0f;
        u32 gi = spatial_nearest(&g, c, rad, &gd);
        u32 wi = brute_force_nearest(pts, N, NULL, c, rad, &wd);
        TG_EXPECT_EQ_U64(gi, wi);
        /* Unconditional: both implementations write 0 when nothing is found, so
         * this needs no guard. A guard here would make the check count depend on
         * whether a point happened to lie exactly on max_radius, which differs
         * between compilers and turned the count into noise. */
        TG_EXPECT_NEAR(gd, wd, 1e-6);
    }

    spatial_destroy(&g);
}

static void test_removal(void) {
    enum { N = 1500, CAP = 128 };
    static V3 pts[N];
    static bool removed[N];
    static u32 got[CAP];
    static u32 want[CAP];
    SpatialGrid g;
    TgRng r = tg_rng_substream(777, TG_RNG_ATTRACTOR_CLOUD, 1, 0);
    u32 i;

    for (i = 0; i < N; ++i) { pts[i] = tg_rng_in_unit_ball(&r); }
    memset(removed, 0, sizeof removed);

    TG_T_CASE("removal is honoured by both query kinds, with no rebuild");
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    TG_EXPECT_EQ_U64(spatial_live_count(&g), N);

    for (i = 0; i < N; i += 3) {
        TG_EXPECT(spatial_remove(&g, i));
        removed[i] = true;
    }
    TG_EXPECT_EQ_U64(spatial_live_count(&g), N - (N + 2) / 3);

    TG_T_CASE("removing twice is refused and does not double-decrement");
    {
        u32 before = spatial_live_count(&g);
        TG_EXPECT(!spatial_remove(&g, 0));
        TG_EXPECT_EQ_U64(spatial_live_count(&g), before);
        TG_EXPECT(spatial_is_removed(&g, 0));
        TG_EXPECT(!spatial_is_removed(&g, 1));
    }

    TG_T_CASE("out-of-range removal is refused");
    TG_EXPECT(!spatial_remove(&g, N));
    TG_EXPECT(spatial_is_removed(&g, N + 100));

    for (i = 0; i < 100; ++i) {
        V3 c = tg_rng_in_unit_ball(&r);
        f32 rad = tg_rng_range(&r, 0.05f, 0.6f);
        u32 gc = 0, gt = 0, wc;
        TG_EXPECT_OK(spatial_query_radius(&g, c, rad, got, CAP, &gc, &gt));
        wc = brute_force_radius(pts, N, removed, c, rad, want, CAP);
        TG_EXPECT_EQ_U64(gt, wc);
        {
            f32 gd, wd;
            u32 gi = spatial_nearest(&g, c, rad, &gd);
            u32 wi = brute_force_nearest(pts, N, removed, c, rad, &wd);
            TG_EXPECT_EQ_U64(gi, wi);
        }
    }

    TG_T_CASE("removing everything leaves an empty but usable grid");
    for (i = 0; i < N; ++i) { (void)spatial_remove(&g, i); }
    TG_EXPECT_EQ_U64(spatial_live_count(&g), 0);
    {
        u32 gc = 0, gt = 0;
        TG_EXPECT_OK(spatial_query_radius(&g, v3_zero(), 10.0f, got, CAP, &gc, &gt));
        TG_EXPECT_EQ_U64(gt, 0);
        TG_EXPECT_EQ_U64(spatial_nearest(&g, v3_zero(), 10.0f, NULL), TG_INVALID_ID);
    }
    spatial_destroy(&g);
}

static void test_bounded_output_is_deterministic(void) {
    enum { N = 800 };
    static V3 pts[N];
    static u32 small[4];
    static u32 large[N];
    SpatialGrid g;
    TgRng r = tg_rng_substream(31, TG_RNG_ATTRACTOR_CLOUD, 2, 0);
    u32 i, sc = 0, st = 0, lc = 0, lt = 0;

    for (i = 0; i < N; ++i) { pts[i] = tg_rng_in_unit_ball(&r); }

    TG_T_CASE("a too-small output buffer keeps the numerically smallest indices");
    /* This is the determinism contract: an overflowing query must not return
     * whichever points happened to be visited first. */
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    TG_EXPECT_OK(spatial_query_radius(&g, v3_zero(), 2.0f, small, 4, &sc, &st));
    TG_EXPECT_OK(spatial_query_radius(&g, v3_zero(), 2.0f, large, N, &lc, &lt));
    TG_EXPECT_EQ_U64(st, lt);
    TG_EXPECT_EQ_U64(sc, 4);
    for (i = 0; i < 4; ++i) { TG_EXPECT_EQ_U64(small[i], large[i]); }
    spatial_destroy(&g);
}

static void test_cell_size_invariance(void) {
    enum { N = 2000, CAP = 512 };
    static V3 pts[N];
    static u32 a[CAP];
    static u32 b[CAP];
    SpatialGrid ga, gb;
    TgRng r = tg_rng_substream(1010, TG_RNG_ATTRACTOR_CLOUD, 3, 0);
    u32 i;

    for (i = 0; i < N; ++i) {
        pts[i] = v3(tg_rng_range(&r, -3.0f, 3.0f),
                    tg_rng_range(&r, 0.0f, 10.0f),
                    tg_rng_range(&r, -3.0f, 3.0f));
    }

    /* THE test that the neighbourhood scan is derived from the radius rather
     * than hard-coded to 3x3x3. With a cell much smaller than the query radius,
     * a fixed neighbourhood silently misses points -- which in the growth
     * simulation appears as branches ignoring nearby attractors, a subtle and
     * very hard-to-diagnose loss of crown quality. */
    TG_T_CASE("query results do not depend on the cell size");
    TG_EXPECT_OK(spatial_build(&ga, pts, N, 0.05f, 1u << 24));  /* tiny cells */
    TG_EXPECT_OK(spatial_build(&gb, pts, N, 4.0f, 1u << 24));   /* huge cells */
    for (i = 0; i < 60; ++i) {
        V3 c = v3(tg_rng_range(&r, -3.0f, 3.0f),
                  tg_rng_range(&r, 0.0f, 10.0f),
                  tg_rng_range(&r, -3.0f, 3.0f));
        f32 rad = tg_rng_range(&r, 0.3f, 2.5f);
        u32 ac = 0, at = 0, bc = 0, bt = 0, k;
        TG_EXPECT_OK(spatial_query_radius(&ga, c, rad, a, CAP, &ac, &at));
        TG_EXPECT_OK(spatial_query_radius(&gb, c, rad, b, CAP, &bc, &bt));
        TG_EXPECT_MSG(at == bt, "cell size changed the result: %u vs %u", at, bt);
        TG_EXPECT_EQ_U64(ac, bc);
        for (k = 0; k < ac && k < bc; ++k) {
            if (a[k] != b[k]) {
                TG_EXPECT_MSG(false, "element %u differs: %u vs %u", k, a[k], b[k]);
                break;
            }
        }
        TG_EXPECT_EQ_U64(spatial_nearest(&ga, c, rad, NULL),
                         spatial_nearest(&gb, c, rad, NULL));
    }
    spatial_destroy(&ga);
    spatial_destroy(&gb);
}

static void test_degenerate_distributions(void) {
    enum { N = 500, CAP = 600 };
    static V3 pts[N];
    static u32 out[CAP];
    SpatialGrid g;
    u32 i, count = 0, total = 0;

    TG_T_CASE("all points coincident");
    for (i = 0; i < N; ++i) { pts[i] = v3(1.0f, 2.0f, 3.0f); }
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    TG_EXPECT_OK(spatial_query_radius(&g, v3(1, 2, 3), 0.001f, out, CAP,
                                      &count, &total));
    TG_EXPECT_EQ_U64(total, N);
    TG_EXPECT_EQ_U64(spatial_nearest(&g, v3(1, 2, 3), 1.0f, NULL), 0);
    spatial_destroy(&g);

    TG_T_CASE("all points collinear -- the case that would demand a huge grid");
    for (i = 0; i < N; ++i) { pts[i] = v3(0.0f, (f32)i * 0.01f, 0.0f); }
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    TG_EXPECT_OK(spatial_query_radius(&g, v3(0, 1.0f, 0), 0.025f, out, CAP,
                                      &count, &total));
    /* Points at 0.98..1.02 inclusive: indices 98..102. */
    TG_EXPECT_EQ_U64(total, 5);
    TG_EXPECT_EQ_U64(out[0], 98);
    spatial_destroy(&g);

    TG_T_CASE("a tiny cell budget forces a coarser grid but stays correct");
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.001f, 8));
    TG_EXPECT(g.cell_count <= 8);
    TG_EXPECT_OK(spatial_query_radius(&g, v3(0, 1.0f, 0), 0.025f, out, CAP,
                                      &count, &total));
    TG_EXPECT_EQ_U64(total, 5);
    spatial_destroy(&g);

    TG_T_CASE("a single point");
    pts[0] = v3(5, 5, 5);
    TG_EXPECT_OK(spatial_build(&g, pts, 1, 0.0f, 0));
    TG_EXPECT_EQ_U64(spatial_nearest(&g, v3(5, 5, 5), 1.0f, NULL), 0);
    TG_EXPECT_EQ_U64(spatial_nearest(&g, v3(50, 5, 5), 1.0f, NULL), TG_INVALID_ID);
    spatial_destroy(&g);

    TG_T_CASE("negative coordinates map to distinct cells (floor, not truncate)");
    {
        /* Truncation toward zero folds [-1,0) and [0,1) into the same cell,
         * which halves the resolution across every axis origin. */
        static V3 two[2];
        two[0] = v3(-0.5f, 0.0f, 0.0f);
        two[1] = v3(0.5f, 0.0f, 0.0f);
        TG_EXPECT_OK(spatial_build(&g, two, 2, 0.4f, 0));
        TG_EXPECT_OK(spatial_query_radius(&g, v3(-0.5f, 0, 0), 0.1f, out, CAP,
                                          &count, &total));
        TG_EXPECT_EQ_U64(total, 1);
        TG_EXPECT_EQ_U64(out[0], 0);
        spatial_destroy(&g);
    }
}

static void test_rejects_bad_input(void) {
    SpatialGrid g;
    V3 pts[3];
    volatile f32 zero = 0.0f;

    TG_T_CASE("a non-finite point is rejected rather than corrupting the grid");
    tg_test_logs_mute();
    pts[0] = v3(0, 0, 0);
    pts[1] = v3(1.0f / zero, 0, 0);
    pts[2] = v3(1, 1, 1);
    TG_EXPECT_ERR(spatial_build(&g, pts, 3, 0.0f, 0), TG_ERR_INVALID_ARGUMENT);
    spatial_destroy(&g);

    TG_T_CASE("a non-finite or non-positive query is a no-op, not a crash");
    pts[1] = v3(1, 0, 0);
    TG_EXPECT_OK(spatial_build(&g, pts, 3, 0.0f, 0));
    {
        u32 out[4], count = 7, total = 7;
        TG_EXPECT_OK(spatial_query_radius(&g, v3(1.0f / zero, 0, 0), 1.0f, out, 4,
                                          &count, &total));
        TG_EXPECT_EQ_U64(total, 0);
        TG_EXPECT_OK(spatial_query_radius(&g, v3_zero(), -1.0f, out, 4,
                                          &count, &total));
        TG_EXPECT_EQ_U64(total, 0);
        TG_EXPECT_EQ_U64(spatial_nearest(&g, v3_zero(), 0.0f, NULL), TG_INVALID_ID);
    }
    spatial_destroy(&g);
    tg_test_logs_unmute();
}

static void test_stats(void) {
    enum { N = 1000 };
    static V3 pts[N];
    SpatialGrid g;
    TgRng r = tg_rng_substream(5, TG_RNG_ATTRACTOR_CLOUD, 4, 0);
    u32 i;
    SpatialStats st;

    TG_T_CASE("stats account for every point exactly once");
    for (i = 0; i < N; ++i) { pts[i] = tg_rng_in_unit_ball(&r); }
    TG_EXPECT_OK(spatial_build(&g, pts, N, 0.0f, 0));
    st = spatial_stats(&g);
    TG_EXPECT(st.occupied_cells > 0);
    TG_EXPECT(st.max_cell_occupancy >= 1);
    TG_EXPECT(st.mean_occupancy_of_occupied >= 1.0f);
    TG_EXPECT_MSG(st.mean_occupancy_of_occupied * (f32)st.occupied_cells
                      > (f32)N - 0.5f,
                  "stats lost points: %.2f * %u vs %u",
                  (double)st.mean_occupancy_of_occupied, st.occupied_cells, N);
    TG_EXPECT(st.bytes > 0);
    spatial_destroy(&g);
}

void test_suite_spatial(void) {
    test_empty_grid();
    test_matches_brute_force();
    test_removal();
    test_bounded_output_is_deterministic();
    test_cell_size_invariance();
    test_degenerate_distributions();
    test_rejects_bad_input();
    test_stats();
}
