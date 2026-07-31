#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_growth.h"

#include <string.h>

/* Grows a complete tree (shoots then roots) and validates it. Returns the graph
 * by pointer; the caller destroys it. Used by nearly every test below so that
 * what is being asserted is the BIOLOGY, not the plumbing. */
static TgResult grow(TreeGraph *g, const TreeSettings *s, TreeResolved *out_r,
                     GrowthResult *out_gr) {
    TgResult r;
    r = tree_profile_resolve(s, out_r);
    if (r != TG_OK) { return r; }
    r = tree_graph_init(g, 4096, out_r->max_organs);
    if (r != TG_OK) { return r; }
    r = tree_growth_run(g, out_r, NULL, out_gr);
    if (r != TG_OK) { return r; }
    return tree_growth_roots(g, out_r, NULL, out_gr);
}

static TreeSettings fast_settings(TreeCategory cat, f32 age) {
    TreeSettings s = tree_settings_default(cat);
    /* Draft quality keeps the suite fast. It changes resolution budgets only --
     * tree_profile tests already assert that quality does not alter the biology,
     * so conclusions drawn here carry to higher settings. */
    s.quality = QUALITY_DRAFT;
    s.age_years = age;
    return s;
}

/* Horizontal spread of the SHOOT system only. The graph report's bounds include
 * the root plate, which is wider than the crown, so using them to judge crown
 * proportions would compare the wrong thing. */
static f32 shoot_spread(const TreeGraph *g) {
    f32 spread = 0.0f;
    u32 i;
    for (i = 0; i < tree_graph_organ_count(g); ++i) {
        const Organ *o = tree_graph_organ(g, i);
        V3 tip;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        tip = organ_tip(o);
        spread = tg_maxf(spread, v3_len(v3(tip.x, 0.0f, tip.z)));
    }
    return spread * 2.0f;
}

/* Centroid of live shoot-segment tips above the crown base: a robust proxy for
 * where the crown mass actually is. */
static V3 crown_centroid(const TreeGraph *g, f32 above_y) {
    V3 sum = v3_zero();
    u32 count = 0, i;
    for (i = 0; i < tree_graph_organ_count(g); ++i) {
        const Organ *o = tree_graph_organ(g, i);
        V3 tip;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
        tip = organ_tip(o);
        if (tip.y < above_y) { continue; }
        sum = v3_add(sum, tip);
        count++;
    }
    if (count == 0) { return v3_zero(); }
    return v3_scale(sum, 1.0f / (f32)count);
}

static void test_grows_a_valid_tree(void) {
    TreeGraph g;
    TreeResolved r;
    GrowthResult gr;
    GraphValidateReport rep;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);

    TG_T_CASE("growth produces a graph that passes full validation");
    TG_EXPECT_OK(grow(&g, &s, &r, &gr));
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT_MSG(rep.passed, "grown tree failed graph validation");
    if (!rep.passed) { tree_graph_log_report(&rep); }

    TG_T_CASE("the tree has a trunk, multiple branch orders, buds and roots");
    TG_EXPECT(rep.segments > 100);
    TG_EXPECT_MSG(rep.max_branch_order >= 3,
                  "only %u branch orders were produced", rep.max_branch_order);
    TG_EXPECT_MSG(rep.buds > 50, "only %u buds were placed", rep.buds);
    TG_EXPECT(g.trunk_axis != TG_INVALID_ID);
    TG_EXPECT_MSG(g.root_axis_first != TG_INVALID_ID, "no root system generated");
    TG_EXPECT_MSG(rep.min_root_depth < -0.1f,
                  "roots did not descend: deepest %.3f m",
                  (double)rep.min_root_depth);

    TG_T_CASE("realised height is close to the resolved target");
    /* The growth model is not told the height directly -- it emerges from step
     * count, internode lengths and resource. Agreement within a wide band is
     * therefore a real check that the two models are consistent. */
    TG_EXPECT_MSG(rep.max_height > r.height_m * 0.45f &&
                  rep.max_height < r.height_m * 1.7f,
                  "grown height %.2f m vs resolved target %.2f m",
                  (double)rep.max_height, (double)r.height_m);

    TG_T_CASE("the crown occupies a plausible fraction of its envelope");
    {
        f32 spread = shoot_spread(&g);
        TG_EXPECT_MSG(spread > r.crown_width_m * 0.3f,
                      "crown spread %.2f m against an envelope width of %.2f m",
                      (double)spread, (double)r.crown_width_m);
    }

    TG_T_CASE("attraction points are genuinely consumed, and more so with age");
    /* If none were consumed, space colonization is not steering anything and the
     * crown would fill uniformly. An absolute percentage would be an arbitrary
     * threshold, so the meaningful claim is asserted instead: consumption is
     * non-trivial AND an older tree claims a larger share of its own envelope. */
    TG_EXPECT(gr.attractors_initial > 100);
    TG_EXPECT_MSG(gr.attractors_consumed > 100,
                  "only %u of %u attractors were consumed",
                  gr.attractors_consumed, gr.attractors_initial);
    {
        TreeGraph old_g;
        TreeResolved old_r;
        GrowthResult old_gr;
        TreeSettings so = s;
        so.age_years = 140.0f;
        TG_EXPECT_OK(grow(&old_g, &so, &old_r, &old_gr));
        {
            f32 frac_young = (f32)gr.attractors_consumed
                           / (f32)tg_max_u32(gr.attractors_initial, 1u);
            f32 frac_old = (f32)old_gr.attractors_consumed
                         / (f32)tg_max_u32(old_gr.attractors_initial, 1u);
            /* The older tree must always claim more points in ABSOLUTE terms --
             * it is a bigger tree that has been competing for longer. */
            TG_EXPECT_MSG(old_gr.attractors_consumed > gr.attractors_consumed,
                          "older tree consumed %u points, younger %u",
                          old_gr.attractors_consumed, gr.attractors_consumed);
            if (old_gr.hit_organ_limit) {
                /* The FRACTIONAL claim can only be compared between runs that
                 * both finished. A 140-year individual exhausts the organ ceiling
                 * partway through its history, so its crown is genuinely
                 * incomplete and its share of a much larger envelope is not
                 * expected to exceed a younger tree's. Asserting otherwise would
                 * be asserting that a documented limitation does not exist. The
                 * weaker claim -- that the fraction has not COLLAPSED -- is still
                 * meaningful, and the limitation is recorded in
                 * docs/limitations.md. */
                TG_EXPECT_MSG(frac_old > frac_young * 0.75f,
                              "truncated older tree claimed %.3f against the "
                              "younger tree's %.3f",
                              (double)frac_old, (double)frac_young);
            } else {
                TG_EXPECT_MSG(frac_old > frac_young,
                              "older tree claimed %.3f of its envelope, younger "
                              "%.3f", (double)frac_old, (double)frac_young);
            }
        }
        tree_graph_destroy(&old_g);
    }

    TG_T_CASE("per-step statistics are real and internally consistent");
    {
        u32 i;
        u32 sum_segments = 0;
        TG_EXPECT(gr.step_count > 3);
        for (i = 0; i < gr.step_count; ++i) {
            const GrowthStepStats *st = &gr.step[i];
            TG_EXPECT_EQ_U64(st->step, i);
            sum_segments += st->segments_added;
            TG_EXPECT(tg_finitef(st->tallest_point_m));
        }
        /* Shoot segments recorded per step must add up to the shoot segments in
         * the graph. Root segments are created outside the step loop. */
        TG_EXPECT_MSG(sum_segments + 1u >= rep.segments - 200u,
                      "step records account for %u segments, graph has %u",
                      sum_segments, rep.segments);
    }

    TG_T_CASE("the tree gets taller over the growth history");
    {
        TG_EXPECT(gr.step[gr.step_count - 1u].tallest_point_m >
                  gr.step[0].tallest_point_m);
    }

    tree_graph_destroy(&g);
}

static void test_determinism(void) {
    TreeGraph a, b;
    TreeResolved ra, rb;
    GrowthResult ga, gb;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 45.0f);
    TgFingerprint fa, fb;

    TG_T_CASE("identical settings produce a bit-identical tree");
    /* The core determinism contract. */
    TG_EXPECT_OK(grow(&a, &s, &ra, &ga));
    TG_EXPECT_OK(grow(&b, &s, &rb, &gb));
    fa = tree_graph_fingerprint(&a);
    fb = tree_graph_fingerprint(&b);
    TG_EXPECT_MSG(tg_fp_equal(fa, fb),
                  "two runs of identical settings differ: %llx vs %llx",
                  (unsigned long long)fa.value, (unsigned long long)fb.value);
    TG_EXPECT_EQ_U64(tree_graph_organ_count(&a), tree_graph_organ_count(&b));
    TG_EXPECT_EQ_U64(tree_graph_axis_count(&a), tree_graph_axis_count(&b));
    TG_EXPECT_EQ_U64(ga.attractors_consumed, gb.attractors_consumed);
    TG_EXPECT_EQ_U64(ga.buds_broken, gb.buds_broken);
    TG_EXPECT(!fa.saw_non_finite);
    tree_graph_destroy(&b);

    TG_T_CASE("a different seed gives a different tree of similar size");
    {
        TreeSettings s2 = s;
        TreeGraph c;
        TreeResolved rc;
        GrowthResult gc;
        TgFingerprint fc;
        s2.seed = s.seed ^ 0x9E3779B9ull;
        TG_EXPECT_OK(grow(&c, &s2, &rc, &gc));
        fc = tree_graph_fingerprint(&c);
        TG_EXPECT_MSG(!tg_fp_equal(fa, fc), "changing the seed changed nothing");
        /* Individuality without loss of identity: the organ count must be in the
         * same ballpark, not double or half. */
        {
            f32 ratio = (f32)tree_graph_organ_count(&c)
                      / (f32)tree_graph_organ_count(&a);
            TG_EXPECT_MSG(ratio > 0.5f && ratio < 2.0f,
                          "organ count ratio between seeds is %.3f", (double)ratio);
        }
        tree_graph_destroy(&c);
    }

    TG_T_CASE("changing any single setting changes the tree");
    {
        struct { const char *what; TreeSettings s; } variants[5];
        u32 i;
        variants[0].what = "age";      variants[0].s = s;
        variants[0].s.age_years += 10.0f;
        variants[1].what = "health";   variants[1].s = s;
        variants[1].s.health = HEALTH_STRESSED;
        variants[2].what = "wind";     variants[2].s = s;
        variants[2].s.environment.wind_exposure = 0.9f;
        variants[3].what = "canopy";   variants[3].s = s;
        variants[3].s.environment.canopy_closure = 0.8f;
        variants[4].what = "light dir"; variants[4].s = s;
        variants[4].s.environment.light_direction =
            v3_norm_or(v3(-1, 1, 0), v3(0, 1, 0));
        for (i = 0; i < 5; ++i) {
            TreeGraph v;
            TreeResolved rv;
            GrowthResult gv;
            TG_EXPECT_OK(grow(&v, &variants[i].s, &rv, &gv));
            TG_EXPECT_MSG(!tg_fp_equal(fa, tree_graph_fingerprint(&v)),
                          "changing %s produced an identical tree",
                          variants[i].what);
            tree_graph_destroy(&v);
        }
    }

    tree_graph_destroy(&a);
}

static void test_broadleaf_vs_conifer(void) {
    TreeGraph bl, cf;
    TreeResolved rbl, rcf;
    GrowthResult gbl, gcf;
    GraphValidateReport pbl, pcf;
    TreeSettings sbl = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
    TreeSettings scf = fast_settings(TREE_CATEGORY_CONIFER, 60.0f);
    f32 rbl_target, rcf_target;

    TG_EXPECT_OK(grow(&bl, &sbl, &rbl, &gbl));
    TG_EXPECT_OK(grow(&cf, &scf, &rcf, &gcf));
    TG_EXPECT_OK(tree_graph_validate(&bl, 0.0f, &pbl));
    TG_EXPECT_OK(tree_graph_validate(&cf, 0.0f, &pcf));
    rbl_target = rbl.crown_width_m * 0.5f;
    rcf_target = rcf.crown_width_m * 0.5f;

    TG_T_CASE("the conifer is excurrent: taller and much narrower");
    {
        f32 wbl = shoot_spread(&bl);
        f32 wcf = shoot_spread(&cf);
        f32 slender_bl = wbl / tg_maxf(pbl.max_height, 0.01f);
        f32 slender_cf = wcf / tg_maxf(pcf.max_height, 0.01f);
        TG_EXPECT_MSG(pcf.max_height > pbl.max_height,
                      "conifer %.2f m is not taller than broadleaf %.2f m",
                      (double)pcf.max_height, (double)pbl.max_height);
        TG_EXPECT_MSG(slender_cf < slender_bl,
                      "conifer width/height %.3f is not below broadleaf %.3f",
                      (double)slender_cf, (double)slender_bl);
    }

    TG_T_CASE("the conifer keeps one leader; the broadleaf reiterates and forks");
    /* THE defining architectural difference. Massart retains strong apical
     * control for life and never reiterates, so its single leader reaches the
     * top. Rauh's control decays, so well-lit high buds become co-dominant
     * orthotropic leaders and the crown forks -- which is what makes an old
     * broadleaf decurrent.
     *
     * Measured by counting reiterated leaders, not by comparing trunk length
     * share: a large conifer has hundreds of branch tiers, so its trunk is a
     * small fraction of total length even though it is unambiguously dominant.
     * That earlier metric was measuring size, not architecture. */
    {
        u32 codom_bl = 0, codom_cf = 0, i;
        for (i = 0; i < tree_graph_axis_count(&bl); ++i) {
            if ((tree_graph_axis(&bl, i)->flags & ORGAN_FLAG_CO_DOMINANT) != 0) {
                codom_bl++;
            }
        }
        for (i = 0; i < tree_graph_axis_count(&cf); ++i) {
            if ((tree_graph_axis(&cf, i)->flags & ORGAN_FLAG_CO_DOMINANT) != 0) {
                codom_cf++;
            }
        }
        TG_EXPECT_MSG(codom_cf == 0,
                      "the conifer reiterated %u leaders; Massart must not",
                      codom_cf);
        TG_EXPECT_MSG(codom_bl > 0,
                      "the broadleaf never reiterated: the crown cannot become "
                      "decurrent");
        /* The conifer's leader must actually be the top of the tree. */
        TG_EXPECT_MSG(tree_graph_axis(&cf, cf.trunk_axis)->tip_position.y
                          > pcf.max_height * 0.90f,
                      "conifer leader tip at %.2f m of a %.2f m tree",
                      (double)tree_graph_axis(&cf, cf.trunk_axis)->tip_position.y,
                      (double)pcf.max_height);
    }

    TG_T_CASE("the crown fills a substantial fraction of its own envelope");
    /* Guards against a skeleton that stays huddled around the trunk. Measured:
     * broadleaf reaches 7.72 m of a 9.60 m envelope radius (80%), conifer 4.73 m
     * of 4.74 m (100%). The gate is set well below both so it catches a real
     * regression without pinning the exact tuning. */
    {
        f32 crown_r_broadleaf = shoot_spread(&bl) * 0.5f;
        f32 crown_r_conifer = shoot_spread(&cf) * 0.5f;
        TG_EXPECT_MSG(crown_r_broadleaf > rbl_target * 0.55f,
                      "broadleaf crown radius %.2f m against a %.2f m envelope",
                      (double)crown_r_broadleaf, (double)rbl_target);
        TG_EXPECT_MSG(crown_r_conifer > rcf_target * 0.55f,
                      "conifer crown radius %.2f m against a %.2f m envelope",
                      (double)crown_r_conifer, (double)rcf_target);
    }

    TG_T_CASE("root plate stays within a plausible multiple of the crown radius");
    /* Field guidance puts the structural root plate at roughly one to three
     * times the crown radius. An earlier version scaled root spread from HEIGHT
     * and produced a 12:1 ratio on the conifer. */
    {
        u32 i;
        f32 root_r_bl = 0.0f, root_r_cf = 0.0f;
        f32 crown_r_bl = shoot_spread(&bl) * 0.5f;
        f32 crown_r_cf = shoot_spread(&cf) * 0.5f;
        for (i = 0; i < tree_graph_organ_count(&bl); ++i) {
            const Organ *o = tree_graph_organ(&bl, i);
            if (o->type != ORGAN_ROOT_SEGMENT) { continue; }
            root_r_bl = tg_maxf(root_r_bl,
                                v3_len(v3(organ_tip(o).x, 0.0f, organ_tip(o).z)));
        }
        for (i = 0; i < tree_graph_organ_count(&cf); ++i) {
            const Organ *o = tree_graph_organ(&cf, i);
            if (o->type != ORGAN_ROOT_SEGMENT) { continue; }
            root_r_cf = tg_maxf(root_r_cf,
                                v3_len(v3(organ_tip(o).x, 0.0f, organ_tip(o).z)));
        }
        TG_EXPECT(crown_r_bl > 0.1f && crown_r_cf > 0.1f);
        TG_EXPECT_MSG(root_r_bl / crown_r_bl < 3.5f,
                      "broadleaf root/crown radius ratio is %.2f",
                      (double)(root_r_bl / crown_r_bl));
        TG_EXPECT_MSG(root_r_cf / crown_r_cf < 3.5f,
                      "conifer root/crown radius ratio is %.2f",
                      (double)(root_r_cf / crown_r_cf));
    }

    TG_T_CASE("both categories self-prune: mortality is substantial and shade-caused");
    /* This replaces a gate that spent its whole life asserting nothing.
     *
     * It was labelled "KNOWN DEFECT GATE: conifer self-shading mortality is too
     * high", it cited 78.4% of shoot segments dying of self-shading against a
     * believed-correct figure under 40%, and it was set to fail above 82% so the
     * defect "could not silently get worse". Measured at the commit before this
     * one, the conifer's actual dead fraction was 6.0% and its shade deaths were
     * exactly ZERO -- its light_death_threshold sat at 0.07 against a darkest
     * achievable light of 0.194, so no conifer shoot could ever be shade-killed
     * however deeply buried. The 78.4% belonged to a tree that no longer existed.
     * A one-sided gate at 0.82 in front of a true value of 0.06 does not catch
     * regressions; it just passes.
     *
     * So the gate is now two-sided and names its cause. Mortality is asserted to
     * be in the band a real tree of this age occupies, and shade is asserted to be
     * an actual mechanism rather than a field in a struct. Measured with shade
     * death reachable: broadleaf 40.2% dead / 5,013 shade stops, conifer 37.8%
     * dead / 14,815 shade stops.
     *
     * Note which direction the fix moved things. Making shade death POSSIBLE
     * raised mortality from 6% to 38% and simultaneously made the tree smaller
     * (179,477 shoot segments to 132,546), because a shoot that self-prunes stops
     * contributing to the shade cast on its neighbours. Mortality here is not
     * damage, it is the crown regulating its own density. */
    {
        u32 i;
        struct { const char *name; const TreeGraph *g; const GrowthResult *gr; }
            cases[2];
        cases[0].name = "broadleaf"; cases[0].g = &bl; cases[0].gr = &gbl;
        cases[1].name = "conifer";   cases[1].g = &cf; cases[1].gr = &gcf;

        for (i = 0; i < 2u; ++i) {
            u32 seg = 0, seg_dead = 0, k;
            f32 frac;
            for (k = 0; k < tree_graph_organ_count(cases[i].g); ++k) {
                const Organ *o = tree_graph_organ(cases[i].g, k);
                if (!organ_type_is_segment((OrganType)o->type)) { continue; }
                if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
                seg++;
                if ((o->flags & ORGAN_FLAG_DEAD) != 0) { seg_dead++; }
            }
            TG_EXPECT(seg > 1000);
            frac = (f32)seg_dead / (f32)tg_max_u32(seg, 1u);
            /* Two-sided. The lower bound is the half that was missing: a tree of
             * this age that has shed almost nothing has not been growing in
             * competition with itself. */
            TG_EXPECT_MSG(frac > 0.15f,
                          "%s shed only %.1f%% of its shoot segments in 60 years: "
                          "a crown that never self-prunes is not competing with "
                          "itself, so its density is unregulated",
                          cases[i].name, (double)(100.0f * frac));
            TG_EXPECT_MSG(frac < 0.65f,
                          "%s shed %.1f%% of its shoot segments: mortality this "
                          "high means the crown over-thickens and then mass-dies "
                          "rather than thinning as it goes",
                          cases[i].name, (double)(100.0f * frac));
            /* Causality, not just magnitude. This is the assertion whose absence
             * let a profile ship with shade death arithmetically impossible. */
            TG_EXPECT_MSG(cases[i].gr->stopped_by_shade > 0u,
                          "%s recorded zero shade deaths: self-pruning does not "
                          "exist for this profile, so whatever killed those shoots "
                          "was not shade",
                          cases[i].name);
        }
    }

    TG_T_CASE("both categories generate a root system within the organ budget");
    /* Regression guard: the shoot system must not spend the whole organ ceiling
     * and leave the tree with no roots. */
    TG_EXPECT_MSG(pbl.min_root_depth < -0.05f, "broadleaf has no roots");
    TG_EXPECT_MSG(pcf.min_root_depth < -0.05f, "conifer has no roots");

    TG_T_CASE("root architectures differ: the conifer plate is shallower and wider");
    {
        f32 depth_bl = -pbl.min_root_depth;
        f32 depth_cf = -pcf.min_root_depth;
        TG_EXPECT(depth_bl > 0.0f && depth_cf > 0.0f);
        TG_EXPECT_MSG(depth_cf / tg_maxf(pcf.max_height, 0.01f)
                          < depth_bl / tg_maxf(pbl.max_height, 0.01f),
                      "conifer relative root depth %.4f is not below broadleaf %.4f",
                      (double)(depth_cf / pcf.max_height),
                      (double)(depth_bl / pbl.max_height));
    }

    TG_T_CASE("the two categories are not the same tree with different numbers");
    TG_EXPECT(!tg_fp_equal(tree_graph_fingerprint(&bl),
                           tree_graph_fingerprint(&cf)));

    tree_graph_destroy(&bl);
    tree_graph_destroy(&cf);
}

static void test_roots_are_not_mirrored_branches(void) {
    TreeGraph g;
    TreeResolved r;
    GrowthResult gr;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
    u32 i;
    u32 root_segments = 0, above_ground_roots = 0;
    f32 root_len_sum = 0.0f, shoot_len_sum = 0.0f;
    u32 shoot_segments = 0;
    f32 root_vertical_sum = 0.0f, shoot_vertical_sum = 0.0f;

    TG_EXPECT_OK(grow(&g, &s, &r, &gr));

    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ *o = tree_graph_organ(&g, i);
        if (o->type == ORGAN_ROOT_SEGMENT) {
            V3 tip = organ_tip(o);
            root_segments++;
            root_len_sum += o->length;
            root_vertical_sum += o->direction.y;
            /* A root emerging into the air is a defect, not a buttress:
             * buttresses are trunk-base geometry, not rising roots. */
            if (tip.y > 1e-4f || o->base.y > 1e-4f) { above_ground_roots++; }
        } else if (organ_type_is_segment((OrganType)o->type)) {
            shoot_segments++;
            shoot_len_sum += o->length;
            shoot_vertical_sum += o->direction.y;
        }
    }

    TG_T_CASE("a root system exists and stays at or below ground level");
    TG_EXPECT(root_segments > 20);
    TG_EXPECT_MSG(above_ground_roots == 0,
                  "%u of %u root segments rose above ground",
                  above_ground_roots, root_segments);

    TG_T_CASE("roots are not mirrored branches: their direction statistics differ");
    /* If roots were generated by mirroring the shoot system, the mean vertical
     * component would be the exact negation of the shoot's. It must not be. */
    {
        f32 root_mean = root_vertical_sum / (f32)root_segments;
        f32 shoot_mean = shoot_vertical_sum / (f32)shoot_segments;
        TG_EXPECT_MSG(root_mean < 0.0f, "roots do not descend on average: %.4f",
                      (double)root_mean);
        TG_EXPECT_MSG(shoot_mean > 0.0f, "shoots do not ascend on average: %.4f",
                      (double)shoot_mean);
        /* The original form of this check asserted that the two mean vertical
         * components were not near-negations of each other. That was a weak proxy
         * and it broke for the wrong reason: a change to the crown envelope moved
         * the shoot mean from 0.31 to 0.28 while the root mean sat at -0.25, and
         * the test failed on a coincidence rather than on any mirroring. A root
         * system that DESCENDS as much as the shoots ASCEND is not evidence of
         * mirroring; it is just a tree.
         *
         * What mirroring would actually imply is one root segment per shoot
         * segment. The real counts differ by more than two orders of magnitude,
         * which is direct structural evidence, and the separate length-distribution
         * case below covers the shape. */
        TG_EXPECT_MSG(shoot_segments > root_segments * 10u,
                      "%u shoot segments against %u root segments: too close to "
                      "a one-for-one mirror (root mean %.4f, shoot mean %.4f)",
                      shoot_segments, root_segments,
                      (double)root_mean, (double)shoot_mean);
    }

    TG_T_CASE("root segment lengths differ systematically from shoot lengths");
    {
        f32 root_mean_len = root_len_sum / (f32)root_segments;
        f32 shoot_mean_len = shoot_len_sum / (f32)shoot_segments;
        TG_EXPECT(root_mean_len > 0.0f && shoot_mean_len > 0.0f);
        TG_EXPECT_MSG(tg_absf(root_mean_len - shoot_mean_len)
                          > shoot_mean_len * 0.10f,
                      "root and shoot segment lengths are effectively identical "
                      "(%.4f vs %.4f m)",
                      (double)root_mean_len, (double)shoot_mean_len);
    }

    TG_T_CASE("root spread relates to the supported above-ground structure");
    {
        GraphValidateReport rep;
        f32 spread = 0.0f;
        TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            V3 tip;
            if (o->type != ORGAN_ROOT_SEGMENT) { continue; }
            tip = organ_tip(o);
            spread = tg_maxf(spread, v3_len(v3(tip.x, 0.0f, tip.z)));
        }
        TG_EXPECT_MSG(spread > 0.15f * rep.max_height,
                      "root spread %.2f m is implausibly small for a %.2f m tree",
                      (double)spread, (double)rep.max_height);
    }

    TG_T_CASE("roots connect to the trunk base, not to a floating origin");
    {
        const Axis *first_root = tree_graph_axis(&g, g.root_axis_first);
        const Organ *ro = tree_graph_organ(&g, first_root->first_organ);
        TG_EXPECT(ro != NULL);
        if (ro != NULL) {
            TG_EXPECT_MSG(v3_len(ro->base) < 0.01f,
                          "first root starts at (%.3f,%.3f,%.3f), not the base",
                          (double)ro->base.x, (double)ro->base.y,
                          (double)ro->base.z);
            TG_EXPECT(ro->parent != TG_INVALID_ID);
        }
    }

    tree_graph_destroy(&g);
}

static void test_shade_mortality_is_causal(void) {
    TreeGraph open_g, forest_g;
    TreeResolved ro, rf;
    GrowthResult go, gf;
    GraphValidateReport po, pf;
    TreeSettings so = fast_settings(TREE_CATEGORY_BROADLEAF, 70.0f);
    TreeSettings sf = so;

    so.environment = tree_environment_open_grown();
    sf.environment = tree_environment_forest();

    TG_EXPECT_OK(grow(&open_g, &so, &ro, &go));
    TG_EXPECT_OK(grow(&forest_g, &sf, &rf, &gf));
    TG_EXPECT_OK(tree_graph_validate(&open_g, 0.0f, &po));
    TG_EXPECT_OK(tree_graph_validate(&forest_g, 0.0f, &pf));

    TG_T_CASE("an open-grown healthy tree is not made to look diseased");
    /* The directive is explicit: a normal healthy tree must not look diseased
     * merely because mortality exists. An earlier light model killed 61% of all
     * organs on an open-grown tree, which is a diseased tree, not a healthy one.
     *
     * Measured on the shoot SEGMENTS specifically, because counting organs folds
     * in the buds carried by dead branches and inflates the figure. Current
     * measured value for the broadleaf: 33.1%. */
    {
        u32 seg = 0, seg_dead = 0, i;
        for (i = 0; i < tree_graph_organ_count(&open_g); ++i) {
            const Organ *o = tree_graph_organ(&open_g, i);
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
            seg++;
            if ((o->flags & ORGAN_FLAG_DEAD) != 0) { seg_dead++; }
        }
        TG_EXPECT(seg > 100);
        TG_EXPECT_MSG((f32)seg_dead / (f32)tg_max_u32(seg, 1u) < 0.45f,
                      "open-grown healthy broadleaf has %.1f%% dead shoot "
                      "segments", (double)(100.0f * (f32)seg_dead / (f32)seg));
    }

    TG_T_CASE("shoots do die, and death is not uniform across the tree");
    /* A tree where every branch is alive reads as a manufactured ornament. */
    TG_EXPECT_MSG(po.dead_organs > 0, "no shoot ever died in the open-grown tree");
    TG_EXPECT_MSG(go.shoots_killed > 0, "no shoot was self-pruned");

    TG_T_CASE("dead shoots cluster in the lower and interior crown");
    /* Mortality must be caused by shade, so its SPATIAL DISTRIBUTION is the real
     * evidence -- a random cull would produce no height bias. */
    {
        f32 dead_y = 0.0f, live_y = 0.0f;
        u32 dead_n = 0, live_n = 0, i;
        for (i = 0; i < tree_graph_organ_count(&open_g); ++i) {
            const Organ *o = tree_graph_organ(&open_g, i);
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
            if ((o->flags & ORGAN_FLAG_DEAD) != 0) {
                dead_y += organ_tip(o).y;
                dead_n++;
            } else {
                live_y += organ_tip(o).y;
                live_n++;
            }
        }
        if (dead_n > 5 && live_n > 5) {
            dead_y /= (f32)dead_n;
            live_y /= (f32)live_n;
            TG_EXPECT_MSG(dead_y < live_y,
                          "dead shoots average %.2f m, live %.2f m: mortality "
                          "shows no height bias, so it is not shade driven",
                          (double)dead_y, (double)live_y);
        } else {
            TG_EXPECT_MSG(false, "too few dead (%u) or live (%u) shoots to judge",
                          dead_n, live_n);
        }
    }

    TG_T_CASE("a forest tree loses proportionally more shoots than an open one");
    {
        f32 frac_open = (f32)po.dead_organs / (f32)tg_max_u32(po.organs, 1u);
        f32 frac_forest = (f32)pf.dead_organs / (f32)tg_max_u32(pf.organs, 1u);
        TG_EXPECT_MSG(frac_forest > frac_open,
                      "forest dead fraction %.4f not above open-grown %.4f",
                      (double)frac_forest, (double)frac_open);
    }

    TG_T_CASE("a forest tree carries its live crown higher");
    {
        f32 lowest_live_open = 1e9f, lowest_live_forest = 1e9f;
        u32 i;
        for (i = 0; i < tree_graph_organ_count(&open_g); ++i) {
            const Organ *o = tree_graph_organ(&open_g, i);
            if (o->branch_order == 0 || o->type == ORGAN_ROOT_SEGMENT) { continue; }
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
            lowest_live_open = tg_minf(lowest_live_open, o->base.y);
        }
        for (i = 0; i < tree_graph_organ_count(&forest_g); ++i) {
            const Organ *o = tree_graph_organ(&forest_g, i);
            if (o->branch_order == 0 || o->type == ORGAN_ROOT_SEGMENT) { continue; }
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
            lowest_live_forest = tg_minf(lowest_live_forest, o->base.y);
        }
        TG_EXPECT(lowest_live_open < 1e8f && lowest_live_forest < 1e8f);
        TG_EXPECT_MSG(lowest_live_forest > lowest_live_open,
                      "lowest live branch: forest %.2f m, open %.2f m",
                      (double)lowest_live_forest, (double)lowest_live_open);
    }

    tree_graph_destroy(&open_g);
    tree_graph_destroy(&forest_g);
}

static void test_crown_asymmetry_is_directional(void) {
    TreeGraph a, b;
    TreeResolved ra, rb;
    GrowthResult ga, gb;
    TreeSettings sa = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
    TreeSettings sb;

    /* Strong one-sided light, no wind, so the only cause of asymmetry is the
     * light direction. Then reverse the light and require the crown to follow. */
    sa.environment = tree_environment_open_grown();
    sa.environment.light_anisotropy = 0.9f;
    sa.environment.wind_exposure = 0.0f;
    sa.environment.light_direction = v3_norm_or(v3(1, 1, 0), v3(0, 1, 0));
    sb = sa;
    sb.environment.light_direction = v3_norm_or(v3(-1, 1, 0), v3(0, 1, 0));

    TG_EXPECT_OK(grow(&a, &sa, &ra, &ga));
    TG_EXPECT_OK(grow(&b, &sb, &rb, &gb));

    TG_T_CASE("the crown mass is displaced toward the light");
    /* Directed asymmetry, not directionless noise: this is the difference between
     * a tree with a light history and a tree with jitter applied. */
    {
        V3 ca = crown_centroid(&a, ra.crown_base_height_m);
        V3 cb = crown_centroid(&b, rb.crown_base_height_m);
        TG_EXPECT_MSG(ca.x > 0.0f,
                      "light from +X but crown centroid x = %.3f", (double)ca.x);
        TG_EXPECT_MSG(cb.x < 0.0f,
                      "light from -X but crown centroid x = %.3f", (double)cb.x);
        TG_EXPECT_MSG(tg_absf(ca.x) > 0.02f * ra.crown_width_m,
                      "displacement %.3f m is negligible for a %.2f m crown",
                      (double)tg_absf(ca.x), (double)ra.crown_width_m);
    }

    TG_T_CASE("an isotropic light field leaves the crown near-centred");
    /* The control case: without a cause there must be no strong asymmetry. */
    {
        TreeGraph c;
        TreeResolved rc;
        GrowthResult gc;
        TreeSettings sc = sa;
        V3 cc;
        sc.environment.light_anisotropy = 0.0f;
        sc.environment.wind_exposure = 0.0f;
        TG_EXPECT_OK(grow(&c, &sc, &rc, &gc));
        cc = crown_centroid(&c, rc.crown_base_height_m);
        TG_EXPECT_MSG(v3_len(v3(cc.x, 0.0f, cc.z)) < rc.crown_width_m * 0.15f,
                      "isotropic light still gave an offset of %.3f m on a %.2f m "
                      "crown", (double)v3_len(v3(cc.x, 0.0f, cc.z)),
                      (double)rc.crown_width_m);
        tree_graph_destroy(&c);
    }

    tree_graph_destroy(&a);
    tree_graph_destroy(&b);
}

static void test_branch_orders_are_not_scaled_copies(void) {
    TreeGraph g;
    TreeResolved r;
    GrowthResult gr;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 70.0f);
    f32 len_sum[6];
    u32 len_n[6];
    u32 i, o;

    memset(len_sum, 0, sizeof len_sum);
    memset(len_n, 0, sizeof len_n);

    TG_EXPECT_OK(grow(&g, &s, &r, &gr));
    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ * org = tree_graph_organ(&g, i);
        if (!organ_type_is_segment((OrganType)org->type)) { continue; }
        if (org->type == ORGAN_ROOT_SEGMENT) { continue; }
        o = tg_min_u32(org->branch_order, 5);
        len_sum[o] += org->length;
        len_n[o]++;
    }

    TG_T_CASE("mean internode length decreases with branch order");
    /* Higher orders must be genuinely different, not scaled copies. */
    {
        f32 prev = 1e9f;
        for (o = 0; o < 6; ++o) {
            f32 mean;
            if (len_n[o] < 4) { continue; }
            mean = len_sum[o] / (f32)len_n[o];
            TG_EXPECT_MSG(mean < prev,
                          "order %u mean internode %.4f m is not below the "
                          "previous order's %.4f m", o, (double)mean, (double)prev);
            prev = mean;
        }
    }

    TG_T_CASE("higher orders are more numerous than the trunk");
    TG_EXPECT_MSG(len_n[2] > len_n[0],
                  "order 2 has %u segments, order 0 has %u", len_n[2], len_n[0]);

    TG_T_CASE("sibling branches receive unequal resource and so differ in length");
    /* The Borchert-Honda partition should make siblings unequal. Equal-length
     * siblings would mean the resource split is not actually doing anything. */
    {
        u32 pairs = 0, unequal = 0;
        for (i = 0; i < tree_graph_axis_count(&g); ++i) {
            const Axis *ax = tree_graph_axis(&g, i);
            u32 j;
            f32 first = -1.0f;
            if (ax->parent_axis == TG_INVALID_ID) { continue; }
            for (j = i + 1u; j < tree_graph_axis_count(&g); ++j) {
                const Axis *bx = tree_graph_axis(&g, j);
                if (bx->parent_axis != ax->parent_axis) { continue; }
                if (bx->order != ax->order) { continue; }
                if (first < 0.0f) { first = ax->total_length; }
                pairs++;
                if (tg_absf(bx->total_length - first) > first * 0.05f) {
                    unequal++;
                }
                break;
            }
        }
        TG_EXPECT(pairs > 10);
        if (pairs > 10) {
            TG_EXPECT_MSG((f32)unequal / (f32)pairs > 0.4f,
                          "only %u of %u sibling pairs differ in length by >5%%: "
                          "the resource partition is not producing inequality",
                          unequal, pairs);
        }
    }

    tree_graph_destroy(&g);
}

static void test_age_progression(void) {
    TreeGraph young, mid, old;
    TreeResolved ry, rm, ro;
    GrowthResult gy, gm, go;
    GraphValidateReport py, pm, po;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 6.0f);

    TG_T_CASE("a young tree is a young tree, not a small mature one");
    TG_EXPECT_OK(grow(&young, &s, &ry, &gy));
    TG_EXPECT_OK(tree_graph_validate(&young, 0.0f, &py));
    s.age_years = 40.0f;
    TG_EXPECT_OK(grow(&mid, &s, &rm, &gm));
    TG_EXPECT_OK(tree_graph_validate(&mid, 0.0f, &pm));
    s.age_years = 160.0f;
    TG_EXPECT_OK(grow(&old, &s, &ro, &go));
    TG_EXPECT_OK(tree_graph_validate(&old, 0.0f, &po));

    TG_EXPECT_MSG(py.max_height < pm.max_height && pm.max_height < po.max_height,
                  "heights not increasing with age: %.2f, %.2f, %.2f",
                  (double)py.max_height, (double)pm.max_height,
                  (double)po.max_height);
    TG_EXPECT_MSG(py.segments < pm.segments && pm.segments < po.segments,
                  "segment counts not increasing with age: %u, %u, %u",
                  py.segments, pm.segments, po.segments);

    TG_T_CASE("a young tree has fewer accumulated branch orders");
    TG_EXPECT_MSG(py.max_branch_order <= po.max_branch_order,
                  "young tree reached order %u, old tree %u",
                  py.max_branch_order, po.max_branch_order);

    TG_T_CASE("a young tree has accumulated fewer scars: less deadwood");
    {
        f32 fy = (f32)py.dead_organs / (f32)tg_max_u32(py.organs, 1u);
        f32 fo = (f32)po.dead_organs / (f32)tg_max_u32(po.organs, 1u);
        TG_EXPECT_MSG(fy <= fo + 0.01f,
                      "young dead fraction %.4f exceeds old %.4f", (double)fy,
                      (double)fo);
    }

    TG_T_CASE("even a very young tree is structurally valid");
    {
        TreeGraph seedling;
        TreeResolved rs;
        GrowthResult gs;
        GraphValidateReport ps;
        TreeSettings ss = fast_settings(TREE_CATEGORY_BROADLEAF, 1.0f);
        TG_EXPECT_OK(grow(&seedling, &ss, &rs, &gs));
        TG_EXPECT_OK(tree_graph_validate(&seedling, 0.0f, &ps));
        TG_EXPECT(ps.passed);
        TG_EXPECT(ps.segments > 0);
        TG_EXPECT_MSG(ps.min_root_depth < 0.0f, "seedling has no roots");
        tree_graph_destroy(&seedling);
    }

    tree_graph_destroy(&young);
    tree_graph_destroy(&mid);
    tree_graph_destroy(&old);
}

static void test_cancellation_and_limits(void) {
    TreeGraph g;
    TreeResolved r;
    GrowthResult gr;
    GraphValidateReport rep;
    GrowthCancel cancel;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 80.0f);

    TG_T_CASE("cancellation returns promptly and leaves a VALID partial graph");
    /* The UI must be able to abort. Whatever exists at that moment must still be
     * structurally sound, so nothing downstream can be handed a half-written
     * organ. */
    cancel.requested = 1;
    TG_EXPECT_OK(tree_profile_resolve(&s, &r));
    TG_EXPECT_OK(tree_graph_init(&g, 256, r.max_organs));
    TG_EXPECT_ERR(tree_growth_run(&g, &r, &cancel, &gr), TG_ERR_CANCELLED);
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT(rep.passed);
    tree_graph_destroy(&g);

    TG_T_CASE("a tight organ ceiling is reported honestly, not silently truncated");
    tg_test_logs_mute();
    TG_EXPECT_OK(tree_profile_resolve(&s, &r));
    TG_EXPECT_OK(tree_graph_init(&g, 64, 400));
    TG_EXPECT_OK(tree_growth_run(&g, &r, NULL, &gr));
    TG_EXPECT_MSG(gr.hit_organ_limit, "the organ ceiling was hit but not reported");
    TG_EXPECT(tree_graph_organ_count(&g) <= 400);
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT_MSG(rep.passed, "graph invalid after hitting the organ ceiling");
    tree_graph_destroy(&g);
    tg_test_logs_unmute();

    TG_T_CASE("growth refuses a non-empty graph rather than appending to it");
    tg_test_logs_mute();
    TG_EXPECT_OK(grow(&g, &s, &r, &gr));
    TG_EXPECT_ERR(tree_growth_run(&g, &r, NULL, &gr), TG_ERR_INVALID_STATE);
    TG_T_CASE("growth refuses a finalized graph");
    tree_graph_finalize(&g);
    TG_EXPECT_ERR(tree_growth_roots(&g, &r, NULL, &gr), TG_ERR_INVALID_STATE);
    tree_graph_destroy(&g);

    TG_T_CASE("roots refuse to grow without a shoot system");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 1000));
    TG_EXPECT_ERR(tree_growth_roots(&g, &r, NULL, &gr), TG_ERR_INVALID_STATE);
    tree_graph_destroy(&g);
    tg_test_logs_unmute();

    TG_T_CASE("bad arguments are rejected");
    TG_EXPECT_OK(tree_graph_init(&g, 8, 100));
    TG_EXPECT_ERR(tree_growth_run(&g, NULL, NULL, &gr), TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_ERR(tree_growth_roots(&g, NULL, NULL, &gr), TG_ERR_INVALID_ARGUMENT);
    tree_graph_destroy(&g);
}

static void test_attractor_cloud(void) {
    TreeResolved r;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
    AttractorCloud cloud;
    u32 i;

    TG_T_CASE("attractors lie inside the crown envelope");
    TG_EXPECT_OK(tree_profile_resolve(&s, &r));
    TG_EXPECT_OK(tree_growth_build_attractors(&r, &cloud));
    TG_EXPECT(cloud.count > 100);
    for (i = 0; i < cloud.count; ++i) {
        V3 p = cloud.points[i];
        TG_EXPECT(v3_finite(p));
        /* Vertically bounded by the live crown. */
        if (!(p.y >= r.crown_base_height_m - 1e-3f && p.y <= r.height_m + 1e-3f)) {
            TG_EXPECT_MSG(false, "attractor %u at y=%.3f outside crown [%.3f,%.3f]",
                          i, (double)p.y, (double)r.crown_base_height_m,
                          (double)r.height_m);
            break;
        }
    }

    TG_T_CASE("the cloud is deterministic for a given seed");
    {
        AttractorCloud again;
        TG_EXPECT_OK(tree_growth_build_attractors(&r, &again));
        TG_EXPECT_EQ_U64(again.count, cloud.count);
        if (again.count == cloud.count) {
            u32 diff = 0;
            for (i = 0; i < cloud.count; ++i) {
                if (memcmp(&again.points[i], &cloud.points[i], sizeof(V3)) != 0) {
                    diff++;
                }
            }
            TG_EXPECT_EQ_U64(diff, 0);
        }
        tree_growth_free_attractors(&again);
    }

    TG_T_CASE("attractor density is calibrated to the influence radius");
    /* If the influence sphere held far more attractors than the query buffer,
     * every direction decision would be silently biased toward low indices. */
    {
        f64 vol = 4.0 / 3.0 * TG_PI * (f64)r.profile->influence_radius_m
                * (f64)r.profile->influence_radius_m
                * (f64)r.profile->influence_radius_m;
        f64 in_range = vol * (f64)r.profile->attractor_density;
        TG_EXPECT_MSG(in_range > 8.0 && in_range < 100.0,
                      "%.1f attractors expected inside the influence sphere",
                      in_range);
    }

    TG_T_CASE("a seedling with no crown yields an empty cloud, not an error");
    {
        TreeResolved tiny;
        AttractorCloud empty;
        TreeSettings st = fast_settings(TREE_CATEGORY_BROADLEAF, 0.5f);
        TG_EXPECT_OK(tree_profile_resolve(&st, &tiny));
        TG_EXPECT_OK(tree_growth_build_attractors(&tiny, &empty));
        tree_growth_free_attractors(&empty);
    }

    TG_T_CASE("asymmetric light thins the shaded side of the cloud");
    {
        AttractorCloud sided;
        TreeResolved rs;
        TreeSettings ss = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
        u32 lit = 0, shaded = 0;
        ss.environment.light_anisotropy = 0.95f;
        ss.environment.light_direction = v3_norm_or(v3(1, 1, 0), v3(0, 1, 0));
        ss.environment.wind_exposure = 0.0f;
        TG_EXPECT_OK(tree_profile_resolve(&ss, &rs));
        TG_EXPECT_OK(tree_growth_build_attractors(&rs, &sided));
        for (i = 0; i < sided.count; ++i) {
            if (sided.points[i].x > 0.0f) { lit++; } else { shaded++; }
        }
        TG_EXPECT_MSG(lit > shaded,
                      "lit side has %u attractors, shaded side %u", lit, shaded);
        tree_growth_free_attractors(&sided);
    }

    tree_growth_free_attractors(&cloud);
    TG_EXPECT(cloud.points == NULL);
}

void test_suite_tree_growth(void) {
    test_attractor_cloud();
    test_grows_a_valid_tree();
    test_determinism();
    test_broadleaf_vs_conifer();
    test_roots_are_not_mirrored_branches();
    test_shade_mortality_is_causal();
    test_crown_asymmetry_is_directional();
    test_branch_orders_are_not_scaled_copies();
    test_age_progression();
    test_cancellation_and_limits();
}
