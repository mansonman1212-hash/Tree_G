/* test_tree_bark.c -- the bark relief field.
 *
 * WHAT THESE TESTS ARE FOR
 *   Bark is a displacement of the wood surface, so a defect in the field is a
 *   defect in the solid: too deep and the furrow reaches the pith and inverts the
 *   section; not periodic in the angle and every limb carries a seam down its
 *   length; not deterministic and the tree is not reproducible. Those are the
 *   three things checked here, plus the two that were actually got wrong and cost
 *   render cycles to find: relief that was present in the mesh but too shallow to
 *   see, and a sample rate that could not resolve the feature the shaping function
 *   produced.
 */
#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_bark.h"
#include "../src/tree/tree_profile.h"

#include <math.h>
#include <string.h>

static TreeResolved resolve(TreeCategory cat, f32 age, TreeQuality q) {
    TreeSettings s = tree_settings_default(cat);
    TreeResolved r;
    s.age_years = age;
    s.quality = q;
    s.seed = 0xB0FFEEull;
    (void)tree_profile_resolve(&s, &r);
    return r;
}

/* ------------------------------------------------------------------------- */

static void test_periodic_in_angle(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    BarkAxisField f;
    f32 radius = r.trunk_base_radius_m * 0.8f;
    f32 maturity = 0.0f;
    BarkFamily fam = tree_bark_family_at(&r, radius, &maturity);
    u32 i;
    f32 worst = 0.0f;

    TG_T_CASE("the field is EXACTLY periodic around the axis");
    /* A branch surface is a cylinder, so the angular coordinate wraps. Noise that
     * does not wrap with it leaves a visible seam running the length of every limb,
     * and a seam is not a small error -- it is a straight line on an object that
     * has none. The lattice wraps by integer cell index, which makes the seam
     * impossible rather than merely small, and this test is what holds that. */
    tree_bark_axis_setup(&r, 7u, radius, fam, &f);
    TG_EXPECT(f.active);
    for (i = 0; i < 64u; ++i) {
        f32 theta = (f32)i / 64.0f * TG_TAU_F;
        f32 arc = 0.13f + (f32)i * 0.211f;
        BarkSample a = tree_bark_sample(&f, theta, arc, radius, maturity);
        BarkSample b = tree_bark_sample(&f, theta + TG_TAU_F, arc, radius,
                                        maturity);
        BarkSample c = tree_bark_sample(&f, theta - TG_TAU_F, arc, radius,
                                        maturity);
        f32 d1 = tg_absf(a.displacement_m - b.displacement_m);
        f32 d2 = tg_absf(a.displacement_m - c.displacement_m);
        if (d1 > worst) { worst = d1; }
        if (d2 > worst) { worst = d2; }
    }
    /* Not a tolerance on appearance: the lattice index arithmetic is exact, so the
     * only slack allowed here is float rounding in the angle itself. */
    TG_EXPECT_MSG(worst < 1e-4f,
                  "field differs by %.6f m across a full turn: the pattern is not "
                  "periodic and the axis carries a seam", (double)worst);
}

static void test_depth_cannot_reach_the_pith(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 120.0f, QUALITY_REFERENCE);
    u32 k;

    TG_T_CASE("displacement is inward and never a large fraction of the radius");
    /* A furrow deeper than the radius turns the section inside out. The clamp is in
     * the field rather than in the caller because the caller cannot know the
     * family's depth ratio, and a limb near the radius threshold is exactly where
     * the two interact. */
    for (k = 0; k < 40u; ++k) {
        f32 radius = 0.035f + (f32)k * 0.010f;
        f32 maturity = 0.0f;
        BarkFamily fam = tree_bark_family_at(&r, radius, &maturity);
        BarkAxisField f;
        u32 i;
        tree_bark_axis_setup(&r, k, radius, fam, &f);
        for (i = 0; i < 128u; ++i) {
            f32 theta = (f32)i / 128.0f * TG_TAU_F;
            BarkSample s = tree_bark_sample(&f, theta, (f32)i * 0.037f, radius,
                                            1.0f);
            TG_EXPECT_MSG(s.displacement_m <= 0.0f,
                          "displacement %.5f m pushes the surface OUTWARD",
                          (double)s.displacement_m);
            TG_EXPECT_MSG(-s.displacement_m < radius * 0.30f,
                          "furrow %.5f m deep on a %.4f m radius reaches the pith",
                          (double)(-s.displacement_m), (double)radius);
        }
    }
}

static void test_thin_axes_carry_no_relief(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    BarkAxisField f;
    f32 thin = tree_bark_min_radius(&r) * 0.5f;
    f32 maturity = 0.0f;
    BarkFamily fam = tree_bark_family_at(&r, thin, &maturity);

    TG_T_CASE("a twig has smooth bark, and pays nothing for the bark model");
    tree_bark_axis_setup(&r, 3u, thin, fam, &f);
    TG_EXPECT_MSG(!f.active, "a %.4f m radius twig was given bark relief",
                  (double)thin);
    TG_EXPECT_MSG(tree_bark_ring_segments(&f) == 0u,
                  "an inactive field still asked for %u ring segments",
                  tree_bark_ring_segments(&f));
    /* The spacing must be large enough that the caller keeps one ring per
     * internode rather than subdividing. */
    TG_EXPECT(tree_bark_ring_spacing(&f) > 1000.0f);
    {
        BarkSample s = tree_bark_sample(&f, 1.0f, 2.0f, thin, 1.0f);
        TG_EXPECT(s.displacement_m == 0.0f);
        TG_EXPECT(s.exposure == 1.0f);
    }
}

static void test_relief_is_deep_enough_to_see(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    f32 radius = r.trunk_base_radius_m * 0.85f;
    f32 maturity = 0.0f;
    BarkFamily fam = tree_bark_family_at(&r, radius, &maturity);
    BarkAxisField f;
    f32 lo = 1.0e9f, hi = -1.0e9f;
    u32 i, j;

    TG_T_CASE("mature trunk relief has real depth, not a token displacement");
    /* This test exists because bark was once present in the mesh and INVISIBLE in
     * the render, twice for different reasons: first the maturity was taken from a
     * binary material flag and came back zero, then the sample rate could not
     * resolve the shaped furrow. A number here would have caught both immediately,
     * where a render took a full generation to produce and a human to look at. */
    tree_bark_axis_setup(&r, 0u, radius, fam, &f);
    TG_EXPECT(f.active);
    TG_EXPECT_MSG(maturity > 0.5f,
                  "an 90-year trunk reports bark maturity %.3f", (double)maturity);
    for (j = 0; j < 24u; ++j) {
        for (i = 0; i < 96u; ++i) {
            f32 theta = (f32)i / 96.0f * TG_TAU_F;
            BarkSample s = tree_bark_sample(&f, theta, (f32)j * 0.05f, radius,
                                            maturity);
            if (s.displacement_m < lo) { lo = s.displacement_m; }
            if (s.displacement_m > hi) { hi = s.displacement_m; }
        }
    }
    TG_EXPECT_MSG(hi - lo > 0.010f,
                  "peak-to-trough relief is only %.1f mm on a %.3f m radius trunk",
                  (double)((hi - lo) * 1000.0f), (double)radius);
    TG_T_CASE("the field reaches both a crest and a floor");
    /* A field that never reaches full depth is a wobble; one that is at full depth
     * everywhere has merely shrunk the trunk. */
    TG_EXPECT_MSG(hi > -0.001f, "no point on the surface is at crest height");
    TG_EXPECT_MSG(lo < -0.008f, "no point on the surface is in a furrow floor");
}

static void test_sample_rate_resolves_the_feature(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    f32 radius = r.trunk_base_radius_m * 0.85f;
    f32 maturity = 0.0f;
    BarkFamily fam = tree_bark_family_at(&r, radius, &maturity);
    BarkAxisField f;
    u32 nseg;

    TG_T_CASE("the requested tessellation resolves the field it has to carry");
    tree_bark_axis_setup(&r, 0u, radius, fam, &f);
    nseg = tree_bark_ring_segments(&f);
    /* At least four samples per lattice cell. Two and a half was tried and the
     * furrows were interpolated away: the shaping function narrows a furrow to a
     * fraction of a cell, so the rate has to resolve the FEATURE, not the cell. */
    TG_EXPECT_MSG(nseg >= f.cells_around * 4u,
                  "%u ring segments for %u lattice cells is under four samples "
                  "per cell", nseg, f.cells_around);
    {
        f32 spacing = tree_bark_ring_spacing(&f);
        f32 cell = 1.0f / tg_maxf(f.cells_per_metre, 1e-6f);
        TG_EXPECT_MSG(spacing < cell * 0.30f,
                      "ring spacing %.4f m against a %.4f m longitudinal cell",
                      (double)spacing, (double)cell);
    }
}

static void test_determinism_and_seed_sensitivity(void) {
    TreeResolved a = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    TreeSettings s2;
    TreeResolved b;
    f32 radius = a.trunk_base_radius_m * 0.8f;
    f32 ma = 0.0f, mb = 0.0f;
    BarkAxisField fa, fb;
    u32 i;
    u32 same = 0, differ = 0;

    TG_T_CASE("the field is identical for identical inputs");
    tree_bark_axis_setup(&a, 5u, radius, tree_bark_family_at(&a, radius, &ma),
                         &fa);
    tree_bark_axis_setup(&a, 5u, radius, tree_bark_family_at(&a, radius, &ma),
                         &fb);
    for (i = 0; i < 256u; ++i) {
        f32 th = (f32)i * 0.0245f, arc = (f32)i * 0.031f;
        BarkSample x = tree_bark_sample(&fa, th, arc, radius, ma);
        BarkSample y = tree_bark_sample(&fb, th, arc, radius, ma);
        if (x.displacement_m == y.displacement_m) { same++; }
    }
    TG_EXPECT_MSG(same == 256u, "%u of 256 samples reproduced", same);

    TG_T_CASE("a different seed produces a different bark pattern");
    /* Otherwise every tree in a stand carries identical bark, which is the kind of
     * repetition the directive names explicitly. */
    s2 = a.settings;
    s2.seed = a.settings.seed ^ 0xABCDEF01ull;
    (void)tree_profile_resolve(&s2, &b);
    tree_bark_axis_setup(&b, 5u, radius, tree_bark_family_at(&b, radius, &mb),
                         &fb);
    for (i = 0; i < 256u; ++i) {
        f32 th = (f32)i * 0.0245f, arc = (f32)i * 0.031f;
        BarkSample x = tree_bark_sample(&fa, th, arc, radius, ma);
        BarkSample y = tree_bark_sample(&fb, th, arc, radius, mb);
        if (tg_absf(x.displacement_m - y.displacement_m) > 1e-5f) { differ++; }
    }
    TG_EXPECT_MSG(differ > 200u,
                  "only %u of 256 samples differ between two seeds", differ);

    TG_T_CASE("neighbouring axes on ONE tree do not share a pattern");
    tree_bark_axis_setup(&a, 6u, radius, tree_bark_family_at(&a, radius, &ma),
                         &fb);
    differ = 0;
    for (i = 0; i < 256u; ++i) {
        f32 th = (f32)i * 0.0245f, arc = (f32)i * 0.031f;
        BarkSample x = tree_bark_sample(&fa, th, arc, radius, ma);
        BarkSample y = tree_bark_sample(&fb, th, arc, radius, ma);
        if (tg_absf(x.displacement_m - y.displacement_m) > 1e-5f) { differ++; }
    }
    TG_EXPECT_MSG(differ > 200u,
                  "axis 5 and axis 6 share %u of 256 samples", 256u - differ);
}

static void test_family_follows_radius_not_chance(void) {
    TreeResolved r = resolve(TREE_CATEGORY_BROADLEAF, 140.0f, QUALITY_STANDARD);
    f32 m_thin = 0.0f, m_thick = 0.0f;
    BarkFamily thin, thick;
    TreeResolved young;
    f32 m_young = 0.0f;

    TG_T_CASE("one tree shows juvenile bark on thin wood and mature bark on thick");
    /* This is the coherence rule the profile exists to enforce: the family is a
     * function of local radius and cambial age, never a draw, which is what lets a
     * single trunk legitimately carry both at once without mixing them. */
    thin = tree_bark_family_at(&r, tree_bark_min_radius(&r) * 1.05f, &m_thin);
    thick = tree_bark_family_at(&r, r.trunk_base_radius_m * 0.9f, &m_thick);
    TG_EXPECT_MSG(m_thick > m_thin,
                  "maturity %.3f on thick wood against %.3f on thin",
                  (double)m_thick, (double)m_thin);
    TG_EXPECT_MSG(thin == r.profile->bark_juvenile,
                  "thin wood shows %s, not the profile's juvenile family %s",
                  tree_bark_family_name(thin),
                  tree_bark_family_name(r.profile->bark_juvenile));
    TG_EXPECT_MSG(thick == r.profile->bark_mature,
                  "thick wood shows %s, not the profile's mature family %s",
                  tree_bark_family_name(thick),
                  tree_bark_family_name(r.profile->bark_mature));

    TG_T_CASE("a young tree has no mature bark anywhere, however thick its stem");
    young = resolve(TREE_CATEGORY_BROADLEAF, 6.0f, QUALITY_STANDARD);
    (void)tree_bark_family_at(&young, young.trunk_base_radius_m * 0.9f, &m_young);
    TG_EXPECT_MSG(m_young < m_thick,
                  "a 6-year tree reports bark maturity %.3f against a 140-year "
                  "tree's %.3f", (double)m_young, (double)m_thick);
}

static void test_conifer_and_broadleaf_differ(void) {
    TreeResolved bl = resolve(TREE_CATEGORY_BROADLEAF, 90.0f, QUALITY_STANDARD);
    TreeResolved cf = resolve(TREE_CATEGORY_CONIFER, 90.0f, QUALITY_STANDARD);
    f32 mb = 0.0f, mc = 0.0f;
    BarkFamily fb = tree_bark_family_at(&bl, bl.trunk_base_radius_m * 0.85f, &mb);
    BarkFamily fc = tree_bark_family_at(&cf, cf.trunk_base_radius_m * 0.85f, &mc);

    TG_T_CASE("the two categories reach different mature bark families");
    TG_EXPECT_MSG(fb != fc, "both categories show %s",
                  tree_bark_family_name(fb));

    TG_T_CASE("and their relief statistics differ, not just their labels");
    /* A pair of families that produce the same surface is two names for one thing.
     * Plated conifer bark should be less anisotropic than an oak's interlacing
     * vertical ridges, so their vertical correlation must differ. */
    {
        BarkAxisField a, b;
        f32 ra = bl.trunk_base_radius_m * 0.85f;
        f32 rc = cf.trunk_base_radius_m * 0.85f;
        f32 va = 0.0f, vc = 0.0f;
        u32 i;
        tree_bark_axis_setup(&bl, 0u, ra, fb, &a);
        tree_bark_axis_setup(&cf, 0u, rc, fc, &b);
        /* Vertical variation over a 0.4 m run at fixed angle. */
        for (i = 1; i < 40u; ++i) {
            f32 arc0 = (f32)(i - 1u) * 0.01f, arc1 = (f32)i * 0.01f;
            va += tg_absf(tree_bark_sample(&a, 1.0f, arc1, ra, mb).displacement_m
                          - tree_bark_sample(&a, 1.0f, arc0, ra, mb)
                                .displacement_m);
            vc += tg_absf(tree_bark_sample(&b, 1.0f, arc1, rc, mc).displacement_m
                          - tree_bark_sample(&b, 1.0f, arc0, rc, mc)
                                .displacement_m);
        }
        TG_EXPECT_MSG(tg_absf(va - vc) > 1e-4f,
                      "vertical variation %.6f m against %.6f m: the two families "
                      "produce the same surface", (double)va, (double)vc);
    }
}

void test_suite_tree_bark(void) {
    test_periodic_in_angle();
    test_depth_cannot_reach_the_pith();
    test_thin_axes_carry_no_relief();
    test_relief_is_deep_enough_to_see();
    test_sample_rate_resolves_the_feature();
    test_determinism_and_seed_sensitivity();
    test_family_follows_radius_not_chance();
    test_conifer_and_broadleaf_differ();
}
