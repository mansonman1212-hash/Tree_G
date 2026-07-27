#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_profile.h"

#include <string.h>

static void test_builtin_profiles_are_coherent(void) {
    u32 cat, i;

    TG_T_CASE("every built-in profile passes its own coherence check");
    for (cat = 0; cat < TREE_CATEGORY_COUNT; ++cat) {
        u32 n = tree_profile_count((TreeCategory)cat);
        TG_EXPECT_MSG(n > 0, "category %s has no profiles",
                      tree_category_name((TreeCategory)cat));
        for (i = 0; i < n; ++i) {
            const TreeProfile *p = tree_profile_get((TreeCategory)cat, i);
            TG_EXPECT(p != NULL);
            if (p == NULL) { continue; }
            TG_EXPECT_MSG(tree_profile_validate(p) == TG_OK,
                          "profile '%s' failed validation", p->name);
            TG_EXPECT_EQ_U64(p->category, cat);
        }
    }

    TG_T_CASE("out-of-range profile lookups return NULL");
    TG_EXPECT(tree_profile_get(TREE_CATEGORY_BROADLEAF, 999) == NULL);
    TG_EXPECT(tree_profile_get(TREE_CATEGORY_COUNT, 0) == NULL);
    TG_EXPECT_EQ_U64(tree_profile_count(TREE_CATEGORY_COUNT), 0);

    TG_T_CASE("every enum has a name and no name reads 'invalid'");
    {
        u32 k;
        for (k = 0; k < TREE_CATEGORY_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_category_name((TreeCategory)k), "invalid") != 0);
        }
        for (k = 0; k < TREE_ARCH_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_architecture_name((TreeArchitecture)k),
                             "invalid") != 0);
        }
        for (k = 0; k < BARK_FAMILY_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_bark_family_name((BarkFamily)k), "invalid") != 0);
        }
        for (k = 0; k < ROOT_ARCH_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_root_architecture_name((RootArchitecture)k),
                             "invalid") != 0);
        }
        for (k = 0; k < LEAF_KIND_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_leaf_kind_name((LeafKind)k), "invalid") != 0);
        }
        for (k = 0; k < TREE_SEASON_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_season_name((TreeSeason)k), "invalid") != 0);
        }
        for (k = 0; k < TREE_HEALTH_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_health_name((TreeHealth)k), "invalid") != 0);
        }
        for (k = 0; k < TREE_QUALITY_COUNT; ++k) {
            TG_EXPECT(strcmp(tree_quality_name((TreeQuality)k), "invalid") != 0);
        }
    }
}

/* The forbidden combinations from the project directive. Each is constructed
 * deliberately and must be REJECTED. Without these, the coherence checker could
 * be vacuous. */
static void test_forbidden_combinations_are_rejected(void) {
    const TreeProfile *base;
    TreeProfile bad;

    tg_test_logs_mute();

    TG_T_CASE("broad leaves on a conifer are rejected");
    base = tree_profile_get(TREE_CATEGORY_CONIFER, 0);
    TG_EXPECT(base != NULL);
    bad = *base;
    bad.leaf_kind = LEAF_SIMPLE_LOBED;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("needles on a broadleaf are rejected");
    base = tree_profile_get(TREE_CATEGORY_BROADLEAF, 0);
    bad = *base;
    bad.leaf_kind = LEAF_NEEDLE_SINGLE;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("angiosperm with compression wood is rejected");
    /* Reaction wood forms on opposite sides in the two groups; getting the sign
     * wrong makes every branch cross-section systematically wrong. */
    bad = *base;
    bad.reaction_wood = REACTION_COMPRESSION_LOWER;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("gymnosperm with tension wood is rejected");
    bad = *tree_profile_get(TREE_CATEGORY_CONIFER, 0);
    bad.reaction_wood = REACTION_TENSION_UPPER;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("a mature bark family as juvenile bark is rejected");
    /* A tree cannot be born with deeply furrowed bark. */
    bad = *base;
    bad.bark_juvenile = BARK_DEEP_FURROWED_RIDGED;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("leaflets on a simple leaf are rejected");
    /* Treating leaflets as independent leaves is explicitly forbidden, so the
     * data model must not be able to express it. */
    bad = *base;
    bad.leaflets_per_leaf = 7;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("a compound leaf with too few leaflets is rejected");
    bad = *base;
    bad.leaf_kind = LEAF_PINNATE_COMPOUND;
    bad.leaflets_per_leaf = 1;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("fascicle needle counts are enforced in both directions");
    bad = *tree_profile_get(TREE_CATEGORY_CONIFER, 0);
    bad.leaf_kind = LEAF_NEEDLE_FASCICLE;
    bad.needles_per_fascicle = 1;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);
    bad = *tree_profile_get(TREE_CATEGORY_CONIFER, 0);
    bad.needles_per_fascicle = 3;  /* set on non-fascicle foliage */
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("Massart architecture without branch tiers is rejected");
    bad = *tree_profile_get(TREE_CATEGORY_CONIFER, 0);
    bad.whorl_count = 1;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("branch orders that are mere scaled copies are rejected");
    /* internode length must genuinely decrease with order; equal values mean the
     * orders are self-similar copies, which the directive forbids. */
    bad = *base;
    bad.internode_length_m[2] = bad.internode_length_m[1];
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("a set angle becoming more vertical with order is rejected");
    bad = *base;
    bad.plagiotropic_set_angle_deg[3] = 10.0f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("gravitropism increasing with order is rejected");
    bad = *base;
    bad.gravitropism[2] = 0.99f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("a non-vertical trunk set angle is rejected");
    bad = *base;
    bad.plagiotropic_set_angle_deg[0] = 20.0f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("an implausible Leonardo exponent is rejected");
    bad = *base;
    bad.leonardo_exponent = 1.0f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);
    bad.leonardo_exponent = 9.0f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("reaction eccentricity at or beyond half the radius is rejected");
    bad = *base;
    bad.reaction_eccentricity = 0.7f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("an influence radius below the kill radius is rejected");
    bad = *base;
    bad.kill_radius_m = bad.influence_radius_m * 2.0f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("a root flare narrower than the trunk is rejected");
    bad = *base;
    bad.root_flare_ratio = 0.8f;
    TG_EXPECT_ERR(tree_profile_validate(&bad), TG_ERR_VALIDATION_FAILED);

    TG_T_CASE("NULL is rejected");
    TG_EXPECT_ERR(tree_profile_validate(NULL), TG_ERR_INVALID_ARGUMENT);

    tg_test_logs_unmute();
}

static void test_resolve_basic(void) {
    TreeSettings s = tree_settings_default(TREE_CATEGORY_BROADLEAF);
    TreeResolved r;

    TG_T_CASE("resolve produces finite, positive, self-consistent values");
    TG_EXPECT_OK(tree_profile_resolve(&s, &r));
    TG_EXPECT(r.profile != NULL);
    TG_EXPECT(r.height_m > 0.0f && tg_finitef(r.height_m));
    TG_EXPECT(r.crown_width_m > 0.0f);
    TG_EXPECT(r.trunk_base_radius_m > 0.0f);
    TG_EXPECT(r.crown_base_height_m > 0.0f);
    TG_EXPECT(r.crown_base_height_m < r.height_m);
    TG_EXPECT(r.crown_widest_height_m >= r.crown_base_height_m);
    TG_EXPECT(r.crown_widest_height_m <= r.height_m);
    TG_EXPECT(r.growth_steps >= 3);
    TG_EXPECT(r.max_organs > 0);
    TG_EXPECT_NEAR(v3_len(r.crown_offset_dir), 1.0f, 1e-4);
    TG_EXPECT_NEAR(v3_len(r.lean_direction), 1.0f, 1e-4);
    TG_EXPECT(r.bark_maturity >= 0.0f && r.bark_maturity <= 1.0f);
    TG_EXPECT(r.deadwood_fraction >= 0.0f && r.deadwood_fraction <= 1.0f);
    TG_EXPECT(r.crown_asymmetry >= 0.0f && r.crown_asymmetry <= 1.0f);

    TG_T_CASE("resolve is pure: the same settings give identical results");
    {
        TreeResolved r2;
        TG_EXPECT_OK(tree_profile_resolve(&s, &r2));
        TG_EXPECT_EQ_U64(memcmp(&r, &r2, sizeof r), 0);
    }

    TG_T_CASE("resolve rejects bad input rather than producing garbage");
    tg_test_logs_mute();
    {
        TreeSettings bad = s;
        TreeResolved out;
        volatile f32 zero = 0.0f;
        bad.age_years = -5.0f;
        TG_EXPECT_ERR(tree_profile_resolve(&bad, &out), TG_ERR_INVALID_ARGUMENT);
        bad = s;
        bad.age_years = 0.0f / zero;
        TG_EXPECT_ERR(tree_profile_resolve(&bad, &out), TG_ERR_INVALID_ARGUMENT);
        bad = s;
        bad.profile_index = 42;
        TG_EXPECT_ERR(tree_profile_resolve(&bad, &out), TG_ERR_NOT_FOUND);
        bad = s;
        bad.environment.light_direction = v3(1.0f / zero, 0, 0);
        TG_EXPECT_ERR(tree_profile_resolve(&bad, &out), TG_ERR_INVALID_ARGUMENT);
        TG_EXPECT_ERR(tree_profile_resolve(NULL, &out), TG_ERR_INVALID_ARGUMENT);
    }
    tg_test_logs_unmute();

    TG_T_CASE("an absurd age is clamped, not rejected: a UI slip must still build");
    {
        TreeSettings extreme = s;
        TreeResolved out;
        extreme.age_years = 1.0e9f;
        TG_EXPECT_OK(tree_profile_resolve(&extreme, &out));
        TG_EXPECT(tg_finitef(out.height_m));
        TG_EXPECT(out.height_m < r.profile->mature_height_m * 2.0f);
        TG_EXPECT(out.growth_steps <= 400);
    }
}

/* The core requirement of the directive's randomisation section: parameters must
 * be CORRELATED, not drawn independently. Each check below asserts a specific
 * causal relationship. */
static void test_correlations(void) {
    TreeSettings base = tree_settings_default(TREE_CATEGORY_BROADLEAF);
    TreeResolved young, old, exposed, sheltered, forest, open;

    TG_T_CASE("age drives height, trunk radius, bark maturity together");
    {
        TreeSettings a = base, b = base;
        a.age_years = 8.0f;
        b.age_years = 200.0f;
        TG_EXPECT_OK(tree_profile_resolve(&a, &young));
        TG_EXPECT_OK(tree_profile_resolve(&b, &old));
        TG_EXPECT_MSG(old.height_m > young.height_m, "older tree is not taller");
        TG_EXPECT_MSG(old.trunk_base_radius_m > young.trunk_base_radius_m,
                      "older tree is not thicker");
        TG_EXPECT_MSG(old.bark_maturity > young.bark_maturity,
                      "older tree does not have more mature bark");
        TG_EXPECT_MSG(old.deadwood_fraction > young.deadwood_fraction,
                      "older tree has no more deadwood");
        TG_EXPECT_MSG(old.crown_width_m > young.crown_width_m,
                      "older tree has no wider crown");
        TG_T_CASE("a young tree is proportionally narrower than an old one");
        TG_EXPECT_MSG(young.crown_width_m / young.height_m
                          < old.crown_width_m / old.height_m,
                      "juvenile proportions not narrower: %.3f vs %.3f",
                      (double)(young.crown_width_m / young.height_m),
                      (double)(old.crown_width_m / old.height_m));
        TG_T_CASE("apical control decays with age (Rauh becomes decurrent)");
        TG_EXPECT_MSG(old.apical_control < young.apical_control,
                      "apical control did not decay: %.4f vs %.4f",
                      (double)old.apical_control, (double)young.apical_control);
        TG_EXPECT(old.apical_control >= old.profile->apical_control_min - 1e-6f);
    }

    TG_T_CASE("wind exposure shortens, thickens and asymmetrises together");
    {
        TreeSettings a = base, b = base;
        a.environment = tree_environment_exposed_ridge();
        b.environment = tree_environment_open_grown();
        TG_EXPECT_OK(tree_profile_resolve(&a, &exposed));
        TG_EXPECT_OK(tree_profile_resolve(&b, &sheltered));
        TG_EXPECT_MSG(exposed.height_m < sheltered.height_m,
                      "exposed tree is not shorter: %.2f vs %.2f",
                      (double)exposed.height_m, (double)sheltered.height_m);
        TG_EXPECT_MSG(exposed.basal_thickening > sheltered.basal_thickening,
                      "exposed tree is not basally thickened");
        TG_EXPECT_MSG(exposed.crown_asymmetry > sheltered.crown_asymmetry,
                      "exposed tree is not more asymmetric");
        TG_EXPECT_MSG(exposed.lean_angle_rad > sheltered.lean_angle_rad,
                      "exposed tree does not lean more");
        TG_EXPECT_MSG(exposed.crown_width_m / exposed.height_m
                          < sheltered.crown_width_m / sheltered.height_m,
                      "exposed tree is not relatively narrower");
    }

    TG_T_CASE("canopy closure lifts the crown and kills lower branches");
    {
        TreeSettings a = base, b = base;
        a.environment = tree_environment_forest();
        b.environment = tree_environment_open_grown();
        TG_EXPECT_OK(tree_profile_resolve(&a, &forest));
        TG_EXPECT_OK(tree_profile_resolve(&b, &open));
        TG_EXPECT_MSG(forest.crown_base_height_m / forest.height_m
                          > open.crown_base_height_m / open.height_m,
                      "forest tree crown is not lifted");
        TG_EXPECT_MSG(forest.deadwood_fraction > open.deadwood_fraction,
                      "forest tree has no more shade-killed shoots");
        TG_EXPECT_MSG(forest.crown_width_m / forest.height_m
                          < open.crown_width_m / open.height_m,
                      "forest tree is not relatively narrower than open-grown");
        TG_T_CASE("shade produces larger leaves (shade-leaf morphology)");
        TG_EXPECT_MSG(forest.leaf_scale > open.leaf_scale,
                      "shade leaves are not larger");
    }

    TG_T_CASE("crown displacement follows light and wind, not random noise");
    {
        TreeSettings a = base;
        TreeResolved ra, rb;
        a.environment = tree_environment_open_grown();
        a.environment.light_anisotropy = 0.9f;
        a.environment.light_direction = v3_norm_or(v3(1, 1, 0), v3(0, 1, 0));
        a.environment.wind_exposure = 0.0f;
        TG_EXPECT_OK(tree_profile_resolve(&a, &ra));
        /* Displacement must be toward the light in the horizontal plane. */
        TG_EXPECT_MSG(ra.crown_offset_dir.x > 0.7f,
                      "crown not displaced toward the light: dir (%.3f,%.3f,%.3f)",
                      (double)ra.crown_offset_dir.x, (double)ra.crown_offset_dir.y,
                      (double)ra.crown_offset_dir.z);
        /* Reverse the light and the displacement must reverse too. */
        a.environment.light_direction = v3_norm_or(v3(-1, 1, 0), v3(0, 1, 0));
        TG_EXPECT_OK(tree_profile_resolve(&a, &rb));
        TG_EXPECT(rb.crown_offset_dir.x < -0.7f);
    }

    TG_T_CASE("health drives deadwood, vigour and foliage coherently");
    {
        TreeSettings a = base, b = base;
        TreeResolved vig, anc;
        a.health = HEALTH_VIGOROUS;
        b.health = HEALTH_ANCIENT;
        TG_EXPECT_OK(tree_profile_resolve(&a, &vig));
        TG_EXPECT_OK(tree_profile_resolve(&b, &anc));
        TG_EXPECT(anc.deadwood_fraction > vig.deadwood_fraction);
        TG_EXPECT(anc.height_m < vig.height_m);
        TG_EXPECT(anc.foliage_density < vig.foliage_density);
        TG_T_CASE("a healthy tree is not made to look diseased");
        TG_EXPECT_MSG(vig.deadwood_fraction < 0.15f,
                      "a vigorous open-grown tree has %.3f deadwood",
                      (double)vig.deadwood_fraction);
    }

    TG_T_CASE("soil resistance trades root depth for root spread");
    {
        TreeSettings a = base, b = base;
        TreeResolved hard, soft;
        a.environment.soil_resistance = 0.95f;
        b.environment.soil_resistance = 0.05f;
        TG_EXPECT_OK(tree_profile_resolve(&a, &hard));
        TG_EXPECT_OK(tree_profile_resolve(&b, &soft));
        TG_EXPECT_MSG(hard.root_depth_m < soft.root_depth_m,
                      "compacted soil did not reduce rooting depth");
        TG_EXPECT_MSG(hard.root_spread_m > soft.root_spread_m,
                      "compacted soil did not increase lateral spread");
    }

    TG_T_CASE("a deciduous profile in winter has no foliage");
    {
        TreeSettings a = base;
        TreeResolved winter;
        a.season = SEASON_WINTER;
        TG_EXPECT_OK(tree_profile_resolve(&a, &winter));
        TG_EXPECT(winter.profile->deciduous);
        TG_EXPECT_NEAR(winter.foliage_density, 0.0f, 0.0);
    }

    TG_T_CASE("an evergreen profile keeps foliage in winter");
    {
        TreeSettings a = tree_settings_default(TREE_CATEGORY_CONIFER);
        TreeResolved winter;
        a.season = SEASON_WINTER;
        TG_EXPECT_OK(tree_profile_resolve(&a, &winter));
        TG_EXPECT(!winter.profile->deciduous);
        TG_EXPECT(winter.foliage_density > 0.0f);
    }
}

static void test_seed_individuality(void) {
    TreeSettings s = tree_settings_default(TREE_CATEGORY_BROADLEAF);
    TreeResolved a, b;
    u32 i;
    f32 min_h = 1e9f, max_h = -1e9f;

    TG_T_CASE("different seeds give different individuals");
    s.seed = 1;
    TG_EXPECT_OK(tree_profile_resolve(&s, &a));
    s.seed = 2;
    TG_EXPECT_OK(tree_profile_resolve(&s, &b));
    TG_EXPECT(a.height_m != b.height_m);
    TG_EXPECT(a.crown_width_m != b.crown_width_m);

    TG_T_CASE("individual variation stays inside a biologically sane band");
    /* Randomisation must create individuality WITHOUT destroying identity: a
     * seed must never produce a tree half or double the expected size. */
    for (i = 0; i < 400; ++i) {
        TreeResolved r;
        s.seed = 1000u + i;
        TG_EXPECT_OK(tree_profile_resolve(&s, &r));
        if (r.height_m < min_h) { min_h = r.height_m; }
        if (r.height_m > max_h) { max_h = r.height_m; }
        TG_EXPECT(r.height_m > 0.0f && tg_finitef(r.height_m));
        TG_EXPECT(r.crown_base_height_m < r.height_m);
    }
    TG_EXPECT_MSG(max_h / min_h < 1.6f,
                  "height varies by %.2fx across seeds (%.2f..%.2f m), which is "
                  "individuality at the cost of identity",
                  (double)(max_h / min_h), (double)min_h, (double)max_h);
    TG_EXPECT_MSG(max_h / min_h > 1.05f,
                  "height varies by only %.3fx: no individuality at all",
                  (double)(max_h / min_h));

    TG_T_CASE("the settings hash distinguishes every field");
    {
        TreeSettings x = tree_settings_default(TREE_CATEGORY_BROADLEAF);
        u64 h0 = tree_settings_hash(&x);
        TreeSettings y;
        y = x; y.seed ^= 1u;              TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.age_years += 1.0f;       TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.season = SEASON_WINTER;  TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.health = HEALTH_ANCIENT; TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.quality = QUALITY_HIGH;  TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.category = TREE_CATEGORY_CONIFER;
                                          TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x; y.environment.wind_exposure += 0.1f;
                                          TG_EXPECT(tree_settings_hash(&y) != h0);
        y = x;                            TG_EXPECT_EQ_U64(tree_settings_hash(&y), h0);
    }
}

static void test_quality_budgets(void) {
    TreeSettings s = tree_settings_default(TREE_CATEGORY_BROADLEAF);
    TreeResolved draft, reference;

    TG_T_CASE("quality levels are strictly ordered in every budget");
    /* Quality must not silently reduce one thing while increasing another. */
    s.quality = QUALITY_DRAFT;
    TG_EXPECT_OK(tree_profile_resolve(&s, &draft));
    s.quality = QUALITY_REFERENCE;
    TG_EXPECT_OK(tree_profile_resolve(&s, &reference));
    TG_EXPECT(reference.cross_section_segments_min > draft.cross_section_segments_min);
    TG_EXPECT(reference.cross_section_segments_max > draft.cross_section_segments_max);
    TG_EXPECT(reference.max_leaves > draft.max_leaves);
    TG_EXPECT(reference.leaf_triangle_budget > draft.leaf_triangle_budget);
    TG_EXPECT(reference.bark_feature_size_m < draft.bark_feature_size_m);
    TG_EXPECT(reference.max_organs > draft.max_organs);

    TG_T_CASE("quality does not change the tree's biology, only its resolution");
    /* If quality altered the biology, two quality levels would be different
     * trees rather than the same tree at different fidelity. */
    TG_EXPECT_NEAR(draft.height_m, reference.height_m, 0.0);
    TG_EXPECT_NEAR(draft.crown_width_m, reference.crown_width_m, 0.0);
    TG_EXPECT_NEAR(draft.trunk_base_radius_m, reference.trunk_base_radius_m, 0.0);
    TG_EXPECT_NEAR(draft.deadwood_fraction, reference.deadwood_fraction, 0.0);
    TG_EXPECT_EQ_U64(draft.growth_steps, reference.growth_steps);

    TG_T_CASE("segment counts stay within the mandate against low-poly output");
    /* A minimum of 24 cross-section segments at reference quality means a trunk
     * silhouette with no visible facets at inspection distance. */
    TG_EXPECT(reference.cross_section_segments_min >= 24);
}

static void test_crown_envelope(void) {
    TreeSettings s = tree_settings_default(TREE_CATEGORY_BROADLEAF);
    TreeResolved rb, rc;
    u32 i;

    TG_EXPECT_OK(tree_profile_resolve(&s, &rb));
    s = tree_settings_default(TREE_CATEGORY_CONIFER);
    TG_EXPECT_OK(tree_profile_resolve(&s, &rc));

    TG_T_CASE("the envelope is zero outside the live crown and positive inside");
    TG_EXPECT_NEAR(tree_resolved_envelope_radius(&rb, 0.0f), 0.0f, 0.0);
    TG_EXPECT_NEAR(tree_resolved_envelope_radius(&rb,
                       rb.crown_base_height_m - 0.01f), 0.0f, 0.0);
    TG_EXPECT_NEAR(tree_resolved_envelope_radius(&rb, rb.height_m + 1.0f), 0.0f, 0.0);
    TG_EXPECT(tree_resolved_envelope_radius(&rb,
                  (rb.crown_base_height_m + rb.height_m) * 0.5f) > 0.0f);

    TG_T_CASE("the envelope never exceeds half the crown width");
    for (i = 0; i <= 200; ++i) {
        f32 h = (f32)i / 200.0f * (rb.height_m + 2.0f);
        f32 rad = tree_resolved_envelope_radius(&rb, h);
        TG_EXPECT(tg_finitef(rad) && rad >= 0.0f);
        TG_EXPECT(rad <= rb.crown_width_m * 0.5f + 1e-4f);
    }

    TG_T_CASE("a conifer envelope is widest near its base, a broadleaf mid-crown");
    /* This single test distinguishes an excurrent cone from a decurrent dome,
     * which is the most visible difference between the two categories. */
    {
        f32 best_h_b = 0.0f, best_r_b = -1.0f;
        f32 best_h_c = 0.0f, best_r_c = -1.0f;
        for (i = 0; i <= 400; ++i) {
            f32 tb = (f32)i / 400.0f;
            f32 hb = tg_lerpf(rb.crown_base_height_m, rb.height_m, tb);
            f32 hc = tg_lerpf(rc.crown_base_height_m, rc.height_m, tb);
            f32 vb = tree_resolved_envelope_radius(&rb, hb);
            f32 vc = tree_resolved_envelope_radius(&rc, hc);
            if (vb > best_r_b) { best_r_b = vb; best_h_b = tb; }
            if (vc > best_r_c) { best_r_c = vc; best_h_c = tb; }
        }
        TG_EXPECT_MSG(best_h_c < 0.25f,
                      "conifer widest at %.3f of the crown, expected near the base",
                      (double)best_h_c);
        TG_EXPECT_MSG(best_h_b > 0.30f && best_h_b < 0.75f,
                      "broadleaf widest at %.3f of the crown, expected mid-crown",
                      (double)best_h_b);
    }

    TG_T_CASE("a conifer is taller and much narrower than a broadleaf of the same age");
    TG_EXPECT(rc.height_m > rb.height_m);
    TG_EXPECT(rc.crown_width_m / rc.height_m < rb.crown_width_m / rb.height_m * 0.6f);

    TG_T_CASE("per-order helpers clamp instead of reading past the array");
    for (i = 0; i < 40; ++i) {
        TG_EXPECT(tg_finitef(tree_resolved_branch_angle(&rb, i)));
        TG_EXPECT(tg_finitef(tree_resolved_internode_length(&rb, i)));
        TG_EXPECT(tg_finitef(tree_resolved_gravitropism(&rb, i)));
        TG_EXPECT(tg_finitef(tree_resolved_set_angle(&rb, i)));
        TG_EXPECT(tree_resolved_internode_length(&rb, i) > 0.0f);
    }
}

void test_suite_tree_profile(void) {
    test_builtin_profiles_are_coherent();
    test_forbidden_combinations_are_rejected();
    test_resolve_basic();
    test_correlations();
    test_seed_individuality();
    test_quality_budgets();
    test_crown_envelope();
}
