#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_mechanics.h"
#include "../src/tree/tree_growth.h"

#include <string.h>

/* Full pipeline: resolve, grow shoots, grow roots, run mechanics. */
static TgResult build(TreeGraph *g, const TreeSettings *s, TreeResolved *out_r,
                      MechanicsResult *out_m) {
    TgResult r;
    GrowthResult gr;
    r = tree_profile_resolve(s, out_r);
    if (r != TG_OK) { return r; }
    r = tree_graph_init(g, 4096, out_r->max_organs);
    if (r != TG_OK) { return r; }
    r = tree_growth_run(g, out_r, NULL, &gr);
    if (r != TG_OK) { return r; }
    r = tree_growth_roots(g, out_r, NULL, &gr);
    if (r != TG_OK) { return r; }
    return tree_mechanics_run(g, out_r, out_m);
}

static TreeSettings fast_settings(TreeCategory cat, f32 age) {
    TreeSettings s = tree_settings_default(cat);
    s.quality = QUALITY_DRAFT;
    s.age_years = age;
    return s;
}

static void test_radii_are_assigned_and_consistent(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    GraphValidateReport rep;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 70.0f);
    u32 i;
    u32 segs = 0, zero_radius = 0, tip_wider = 0, child_wider = 0;

    TG_T_CASE("mechanics assigns a positive radius to every segment");
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ *o = tree_graph_organ(&g, i);
        const Organ *par;
        if (!organ_type_is_segment((OrganType)o->type)) {
            /* Non-segments carry no radius at all. */
            TG_EXPECT_MSG(o->radius_base == 0.0f && o->radius_tip == 0.0f,
                          "organ %u of type %s has a radius", i,
                          organ_type_name((OrganType)o->type));
            continue;
        }
        segs++;
        if (!(o->radius_base > 0.0f) || !(o->radius_tip > 0.0f)) { zero_radius++; }
        if (o->radius_tip > o->radius_base * 1.0001f) { tip_wider++; }
        if (o->parent == TG_INVALID_ID) { continue; }
        par = tree_graph_organ(&g, o->parent);
        if (!organ_type_is_segment((OrganType)par->type)) { continue; }
        if (o->radius_base > par->radius_base * 1.0001f) { child_wider++; }
    }
    TG_EXPECT(segs > 100);
    TG_EXPECT_EQ_U64(zero_radius, 0);

    TG_T_CASE("no segment widens distally");
    TG_EXPECT_EQ_U64(tip_wider, 0);

    TG_T_CASE("no child is thicker than its parent");
    /* The directive's explicit rule. It is not automatic: the basal flare, the
     * root override and the tip-radius floor are all applied after the pipe model
     * and can break it locally, which is why mechanics enforces it acropetally. */
    TG_EXPECT_EQ_U64(child_wider, 0);

    TG_T_CASE("the graph still validates after mechanics");
    /* This is the check that catches stale frames: bending changes every
     * direction, so frames propagated during growth are no longer perpendicular
     * unless mechanics rebuilt them. */
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT_MSG(rep.passed, "graph invalid after mechanics");
    if (!rep.passed) { tree_graph_log_report(&rep); }

    TG_T_CASE("trunk base radius lands on the resolved target");
    /* The pipe model fixes the SHAPE of the radius distribution; the calibration
     * constant fixes its scale. If these disagree, the radial model and the
     * size/age model are two unrelated descriptions of the same tree. The basal
     * flare is applied on top, so the realised value is deliberately larger. */
    TG_EXPECT(m.trunk_base_radius_target > 0.0f);
    {
        f32 ratio = m.trunk_base_radius_m / m.trunk_base_radius_target;
        f32 expected_flare = 1.0f + r.profile->basal_flare_factor;
        TG_EXPECT_MSG(ratio > 0.95f && ratio < expected_flare * 1.10f,
                      "realised trunk base radius %.4f m is %.2fx the target "
                      "%.4f m (flare alone should give ~%.2fx)",
                      (double)m.trunk_base_radius_m, (double)ratio,
                      (double)m.trunk_base_radius_target, (double)expected_flare);
    }

    TG_T_CASE("the thinnest segments sit at the profile's tip radius");
    TG_EXPECT_NEAR(m.min_radius_m, r.profile->tip_radius_m,
                   (f64)r.profile->tip_radius_m * 0.5);

    TG_T_CASE("radius decreases with branch order");
    {
        f32 sum[6];
        u32 cnt[6];
        u32 o;
        memset(sum, 0, sizeof sum);
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *org = tree_graph_organ(&g, i);
            if (!organ_type_is_segment((OrganType)org->type)) { continue; }
            if (org->type == ORGAN_ROOT_SEGMENT) { continue; }
            o = tg_min_u32(org->branch_order, 5);
            sum[o] += org->radius_base;
            cnt[o]++;
        }
        {
            f32 prev = 1e9f;
            for (o = 0; o < 6; ++o) {
                f32 mean;
                if (cnt[o] < 4) { continue; }
                mean = sum[o] / (f32)cnt[o];
                TG_EXPECT_MSG(mean < prev,
                              "order %u mean radius %.5f m is not below the "
                              "previous order's %.5f m", o, (double)mean,
                              (double)prev);
                prev = mean;
            }
        }
    }

    tree_graph_destroy(&g);
}

static void test_basal_flare(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 90.0f);
    const Axis *trunk;
    u32 id;
    f32 ground_radius = 0.0f, above_radius = 0.0f;
    f32 flare_height;

    TG_T_CASE("the trunk flares toward ground level");
    /* Without a flare the trunk enters the ground like a pole, which the directive
     * names explicitly as a cause of the synthetic look. */
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    trunk = tree_graph_axis(&g, g.trunk_axis);
    TG_EXPECT(trunk != NULL && trunk->first_organ != TG_INVALID_ID);
    ground_radius = tree_graph_organ(&g, trunk->first_organ)->radius_base;
    flare_height = r.trunk_base_radius_m * r.profile->basal_flare_height_ratio;

    /* Walk up the trunk to a height well past the flare's decay length. */
    id = trunk->first_organ;
    while (id != TG_INVALID_ID) {
        const Organ *o = tree_graph_organ(&g, id);
        u32 child, next = TG_INVALID_ID;
        if (o->base.y > flare_height * 4.0f) {
            above_radius = o->radius_base;
            break;
        }
        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *c = tree_graph_organ(&g, child);
            if (c->axis == trunk->id &&
                organ_type_is_segment((OrganType)c->type)) {
                next = child;
                break;
            }
            child = c->next_sibling;
        }
        id = next;
    }

    TG_EXPECT(ground_radius > 0.0f);
    TG_EXPECT(above_radius > 0.0f);
    TG_EXPECT_MSG(ground_radius > above_radius * 1.10f,
                  "trunk base radius %.4f m is not meaningfully wider than "
                  "%.4f m above the flare", (double)ground_radius,
                  (double)above_radius);

    TG_T_CASE("the flare decays rather than persisting up the trunk");
    /* A flare that never decays is just a thicker tree. */
    {
        f32 unflared = r.trunk_base_radius_m;
        TG_EXPECT_MSG(above_radius < unflared * 1.25f,
                      "radius %.4f m above the flare is still close to the "
                      "flared base scale %.4f m", (double)above_radius,
                      (double)(unflared * (1.0f + r.profile->basal_flare_factor)));
    }

    tree_graph_destroy(&g);
}

static void test_pipe_model_area_relationship(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 70.0f);
    u32 i;
    u32 checked = 0;
    f32 worst = 0.0f;

    TG_T_CASE("radius follows the pipe model with a single calibration constant");
    /* Tests the model that is actually implemented: r = k * A^(1/delta) with ONE k
     * for the whole tree.
     *
     * An earlier version of this test tried to verify r_parent^delta = sum of
     * r_child^delta at "branch points" defined as segments with two or more
     * segment children -- and found ZERO of them, because in this graph a lateral
     * axis hangs off a BUD, so a segment never has two segment children. The test
     * was measuring nothing. Checking the underlying relation directly is both
     * stronger and immune to that topology detail.
     *
     * Segments where a documented override applies are excluded: the basal flare,
     * the root sizing rule, and the tip-radius floor are all deliberate departures
     * from the pure pipe model. */
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    {
        f32 inv_delta = 1.0f / r.profile->leonardo_exponent;
        f32 k = m.pipe_calibration_k;
        f32 floor_r = r.profile->tip_radius_m * 1.5f;
        f32 flare_top = r.trunk_base_radius_m
                      * r.profile->basal_flare_height_ratio * 6.0f;
        TG_EXPECT_MSG(k > 0.0f, "no pipe calibration constant was produced");
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            f32 predicted, err;
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
            if (o->type == ORGAN_TRUNK_SEGMENT && o->base.y < flare_top) {
                continue; /* flare region */
            }
            if (o->radius_base <= floor_r) { continue; }
            if (!(o->supported_leaf_area > 0.0f)) { continue; }
            predicted = k * powf(o->supported_leaf_area, inv_delta);
            err = tg_absf(o->radius_base - predicted)
                / tg_maxf(predicted, 1e-12f);
            if (err > worst) { worst = err; }
            checked++;
        }
        TG_EXPECT_MSG(checked > 200, "only %u segments were checkable", checked);
        TG_EXPECT_MSG(worst < 0.02f,
                      "worst deviation from r = k*A^(1/delta) is %.4f over %u "
                      "segments", (double)worst, checked);
    }

    TG_T_CASE("a larger Leonardo exponent thickens the interior of the crown");
    /* Verifies the exponent genuinely enters the model rather than being carried
     * as an unused parameter. With the trunk base pinned by calibration, raising
     * delta must redistribute thickness -- the mid-crown gets relatively thicker
     * because area^(1/delta) flattens. */
    {
        TreeGraph g2;
        TreeResolved r2;
        MechanicsResult m2;
        GrowthResult gr;
        TreeProfile fat;
        TreeSettings s2 = s;
        f32 mean1 = 0.0f, mean2 = 0.0f;
        u32 n1 = 0, n2 = 0;

        TG_EXPECT_OK(tree_profile_resolve(&s2, &r2));
        fat = *r2.profile;
        fat.leonardo_exponent = 3.4f;
        r2.profile = &fat;
        TG_EXPECT_OK(tree_graph_init(&g2, 4096, r2.max_organs));
        TG_EXPECT_OK(tree_growth_run(&g2, &r2, NULL, &gr));
        TG_EXPECT_OK(tree_growth_roots(&g2, &r2, NULL, &gr));
        TG_EXPECT_OK(tree_mechanics_run(&g2, &r2, &m2));

        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            if (o->type != ORGAN_BRANCH_SEGMENT) { continue; }
            mean1 += o->radius_base;
            n1++;
        }
        for (i = 0; i < tree_graph_organ_count(&g2); ++i) {
            const Organ *o = tree_graph_organ(&g2, i);
            if (o->type != ORGAN_BRANCH_SEGMENT) { continue; }
            mean2 += o->radius_base;
            n2++;
        }
        TG_EXPECT(n1 > 50 && n2 > 50);
        if (n1 > 50 && n2 > 50) {
            mean1 /= (f32)n1;
            mean2 /= (f32)n2;
            TG_EXPECT_MSG(mean2 > mean1,
                          "delta=3.4 gave mean branch radius %.5f m, delta=%.2f "
                          "gave %.5f m", (double)mean2,
                          (double)r.profile->leonardo_exponent, (double)mean1);
        }
        tree_graph_destroy(&g2);
    }

    tree_graph_destroy(&g);
}

static void test_mass_is_plausible(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 90.0f);

    TG_T_CASE("wood and foliage mass are positive and finite");
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    TG_EXPECT(m.total_wood_mass_kg > 0.0f && tg_finitef(m.total_wood_mass_kg));
    TG_EXPECT(m.total_foliage_mass_kg > 0.0f);
    TG_EXPECT(m.total_leaf_area_m2 > 0.0f);
    TG_EXPECT(m.foliage_bearing_segments > 0);

    TG_T_CASE("wood mass is in the right order of magnitude for the trunk volume");
    /* Checked against an independent estimate: a cylinder of the trunk's base
     * radius and the tree's height, times wood density, is a generous upper
     * bound on the whole tree's wood mass, and a small fraction of it is a lower
     * bound. This catches a units error or a factor-of-1000 slip, which is what
     * an order-of-magnitude check is for. */
    {
        f32 rb = m.trunk_base_radius_m;
        f32 cyl = TG_PI_F * rb * rb * r.height_m * r.profile->wood_density_kgm3;
        TG_EXPECT_MSG(m.total_wood_mass_kg < cyl * 2.5f,
                      "wood mass %.1f kg exceeds 2.5x the bounding cylinder "
                      "%.1f kg", (double)m.total_wood_mass_kg, (double)cyl);
        TG_EXPECT_MSG(m.total_wood_mass_kg > cyl * 0.02f,
                      "wood mass %.1f kg is under 2%% of the bounding cylinder "
                      "%.1f kg", (double)m.total_wood_mass_kg, (double)cyl);
    }

    TG_T_CASE("supported mass increases toward the base");
    {
        const Axis *trunk = tree_graph_axis(&g, g.trunk_axis);
        const Organ *base = tree_graph_organ(&g, trunk->first_organ);
        const Organ *tip = tree_graph_organ(&g, trunk->last_organ);
        TG_EXPECT_MSG(base->supported_mass > tip->supported_mass,
                      "trunk base supports %.2f kg, trunk tip %.2f kg",
                      (double)base->supported_mass, (double)tip->supported_mass);
        /* Everything is distal to the trunk base, so it must carry the total. */
        TG_EXPECT_NEAR(base->supported_mass,
                       m.total_wood_mass_kg + m.total_foliage_mass_kg,
                       (f64)(m.total_wood_mass_kg) * 0.02 + 1.0);
    }

    TG_T_CASE("an evergreen carries far more foliage per unit leaf-bearing shoot");
    /* Needle retention across several age classes is the mechanism, and it is why
     * a conifer's branches sag under a load a deciduous tree never carries. */
    {
        TreeGraph gc;
        TreeResolved rc;
        MechanicsResult mc;
        TreeSettings sc = fast_settings(TREE_CATEGORY_CONIFER, 90.0f);
        TG_EXPECT_OK(build(&gc, &sc, &rc, &mc));
        TG_EXPECT(!rc.profile->deciduous);
        TG_EXPECT_MSG(mc.total_foliage_mass_kg > m.total_foliage_mass_kg,
                      "conifer foliage mass %.1f kg is not above broadleaf %.1f kg",
                      (double)mc.total_foliage_mass_kg,
                      (double)m.total_foliage_mass_kg);
        tree_graph_destroy(&gc);
    }

    TG_T_CASE("a deciduous tree in winter has no foliage mass and still works");
    {
        TreeGraph gw;
        TreeResolved rw;
        MechanicsResult mw;
        GraphValidateReport rep;
        TreeSettings sw = fast_settings(TREE_CATEGORY_BROADLEAF, 90.0f);
        sw.season = SEASON_WINTER;
        TG_EXPECT_OK(build(&gw, &sw, &rw, &mw));
        TG_EXPECT_NEAR(mw.total_foliage_mass_kg, 0.0f, 0.0);
        TG_EXPECT_NEAR(mw.total_leaf_area_m2, 0.0f, 0.0);
        /* The pipe model has nothing to work from, so the fallback taper must
         * still produce a valid, invariant-respecting tree rather than leaving
         * every radius at the tip value. */
        TG_EXPECT(mw.max_radius_m > rw.profile->tip_radius_m * 10.0f);
        TG_EXPECT_OK(tree_graph_validate(&gw, 0.0f, &rep));
        TG_EXPECT_MSG(rep.passed, "winter tree failed validation");
        tree_graph_destroy(&gw);
    }

    tree_graph_destroy(&g);
}

static void test_deflection_behaviour(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 90.0f);
    u32 i;

    TG_T_CASE("deflection occurs and is bounded");
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    TG_EXPECT_MSG(m.max_segment_rotation_rad > 1e-5f,
                  "no segment bent at all: the mechanics pass is inert");
    TG_EXPECT(m.max_segment_rotation_rad <= 0.20f + 1e-6f);
    TG_EXPECT(tg_finitef(m.max_tip_deflection_m));

    TG_T_CASE("the trunk stays essentially vertical");
    /* Gravity acting on a vertical member produces no bending moment, and the
     * implementation gets this for free because the rotation axis vanishes. If a
     * trunk ever sags, the moment arm is being computed wrongly. */
    {
        const Axis *trunk = tree_graph_axis(&g, g.trunk_axis);
        u32 id = trunk->first_organ;
        f32 worst = 0.0f;
        while (id != TG_INVALID_ID) {
            const Organ *o = tree_graph_organ(&g, id);
            u32 child, next = TG_INVALID_ID;
            f32 from_vertical = v3_angle_between(o->direction, v3(0, 1, 0));
            if (from_vertical > worst) { worst = from_vertical; }
            for (child = o->first_child; child != TG_INVALID_ID;) {
                const Organ *c = tree_graph_organ(&g, child);
                if (c->axis == trunk->id &&
                    organ_type_is_segment((OrganType)c->type)) {
                    next = child;
                    break;
                }
                child = c->next_sibling;
            }
            id = next;
        }
        /* Generous: the trunk also carries the profile's lean and its own
         * phototropic wander. The point is that it is not sagging over. */
        TG_EXPECT_MSG(worst < 0.75f,
                      "trunk deviates %.3f rad from vertical", (double)worst);
    }

    TG_T_CASE("deflection moves every tip DOWNWARD relative to an unbent tree");
    /* The sign check, and the only honest way to make it.
     *
     * Comparing a bent segment's direction against its PARENT's is meaningless,
     * because gravitropism already makes later segments of an axis point more
     * upward than earlier ones -- an earlier version of this test did exactly
     * that and reported 863 "upward" deflections in a correctly sagging tree.
     *
     * The right comparison is against the SAME tree with sag retention set to
     * zero. Growth does not read sag_retention, so the two graphs are structurally
     * identical and organ ids correspond exactly; every difference is deflection
     * and nothing else. A sign error would show as tips rising. */
    {
        TreeGraph unbent;
        TreeResolved ru;
        MechanicsResult mu;
        GrowthResult gr;
        TreeProfile rigid;
        TreeSettings su = s;
        TG_EXPECT_OK(tree_profile_resolve(&su, &ru));
        rigid = *ru.profile;
        rigid.sag_retention = 0.0f;
        ru.profile = &rigid;
        TG_EXPECT_OK(tree_graph_init(&unbent, 4096, ru.max_organs));
        TG_EXPECT_OK(tree_growth_run(&unbent, &ru, NULL, &gr));
        TG_EXPECT_OK(tree_growth_roots(&unbent, &ru, NULL, &gr));
        TG_EXPECT_OK(tree_mechanics_run(&unbent, &ru, &mu));

        TG_EXPECT_MSG(tree_graph_organ_count(&unbent) ==
                          tree_graph_organ_count(&g),
                      "the two graphs are not structurally identical (%u vs %u "
                      "organs), so the comparison would be meaningless",
                      tree_graph_organ_count(&unbent),
                      tree_graph_organ_count(&g));

        if (tree_graph_organ_count(&unbent) == tree_graph_organ_count(&g)) {
            u32 risen = 0, fallen = 0;
            f32 worst_rise = 0.0f, biggest_drop = 0.0f;
            for (i = 0; i < tree_graph_organ_count(&g); ++i) {
                const Organ *a = tree_graph_organ(&g, i);
                const Organ *b = tree_graph_organ(&unbent, i);
                f32 dy;
                if (!organ_type_is_segment((OrganType)a->type)) { continue; }
                if (a->type == ORGAN_ROOT_SEGMENT) { continue; }
                dy = organ_tip(a).y - organ_tip(b).y;
                if (dy > 1e-4f) {
                    risen++;
                    if (dy > worst_rise) { worst_rise = dy; }
                } else if (dy < -1e-4f) {
                    fallen++;
                    if (-dy > biggest_drop) { biggest_drop = -dy; }
                }
            }
            TG_EXPECT_MSG(fallen > 50,
                          "only %u tips moved down: the pass is barely doing "
                          "anything", fallen);
            /* A few tips rising is CORRECT, not a defect. A branch that points
             * back toward the trunk sits on the proximal side of its parent's
             * bending pivot, so a correct downward rotation of the parent lifts
             * it -- rigid-body geometry, nothing to do with the sign. What must
             * hold is that falling dominates overwhelmingly and that the largest
             * rise is small beside the largest drop. The exact sign is asserted
             * separately and precisely by the own_bend_upward audit below. */
            TG_EXPECT_MSG(risen * 8u < fallen,
                          "%u tips rose against %u falling: too many to be "
                          "back-pointing branches", risen, fallen);
            TG_EXPECT_MSG(worst_rise < biggest_drop * 0.2f,
                          "largest rise %.4f m against largest drop %.4f m",
                          (double)worst_rise, (double)biggest_drop);
            TG_EXPECT(biggest_drop > 1e-3f);
        }
        tree_graph_destroy(&unbent);
    }

    TG_T_CASE("no segment's own bend raises its own tip: the exact sign check");
    /* The unambiguous form of the sign test, free of the rigid-propagation
     * confound. Measured inside the pass against each segment's direction after
     * the inherited rotation and before its own. */
    TG_EXPECT_MSG(m.own_bend_upward == 0,
                  "%u segments bent upward under gravity", m.own_bend_upward);

    TG_T_CASE("axes remain connected after bending");
    /* Rotating a segment without re-anchoring its children opens a gap at every
     * joint, which is precisely the junction crack the mesh validator exists to
     * find. Checked here at the graph level so it is caught before meshing. */
    {
        u32 broken = 0;
        f32 worst = 0.0f;
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            const Organ *par;
            f32 gap;
            if (!organ_type_is_segment((OrganType)o->type)) { continue; }
            if (o->parent == TG_INVALID_ID) { continue; }
            par = tree_graph_organ(&g, o->parent);
            if (par->axis != o->axis) { continue; }
            if (!organ_type_is_segment((OrganType)par->type)) { continue; }
            gap = v3_dist(o->base, organ_tip(par));
            if (gap > worst) { worst = gap; }
            if (gap > 1e-3f) { broken++; }
        }
        TG_EXPECT_MSG(broken == 0,
                      "%u joints opened a gap after bending, worst %.6f m",
                      broken, (double)worst);
    }

    TG_T_CASE("every frame is still orthonormal after bending");
    /* The stale-frame check, stated directly rather than only via the validator. */
    {
        u32 bad = 0;
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            if (!tg_nearf(v3_len(o->frame_ref), 1.0f, 1e-3f) ||
                tg_absf(v3_dot(o->frame_ref, o->direction)) > 1e-3f) {
                bad++;
            }
        }
        TG_EXPECT_MSG(bad == 0, "%u organs have a stale or sheared frame", bad);
    }

    TG_T_CASE("a stiffer profile deflects less than a compliant one");
    /* Verifies the modulus actually enters the calculation. */
    {
        TreeGraph gs;
        TreeResolved rs;
        MechanicsResult ms;
        GrowthResult gr;
        TreeProfile stiff;
        TreeSettings ss = s;
        TG_EXPECT_OK(tree_profile_resolve(&ss, &rs));
        stiff = *rs.profile;
        stiff.wood_modulus_pa *= 50.0f;
        rs.profile = &stiff;
        TG_EXPECT_OK(tree_graph_init(&gs, 4096, rs.max_organs));
        TG_EXPECT_OK(tree_growth_run(&gs, &rs, NULL, &gr));
        TG_EXPECT_OK(tree_growth_roots(&gs, &rs, NULL, &gr));
        TG_EXPECT_OK(tree_mechanics_run(&gs, &rs, &ms));
        /* NOT max_segment_rotation_rad. That statistic SATURATES: the stability
         * clamp caps any single segment at 0.2 rad, and on a mature tree both the
         * stiff and the compliant run have at least one segment against the cap, so
         * the comparison read 0.20000 < 0.20000 and failed while the modulus was
         * working perfectly. A test has to measure something that can still move.
         *
         * Two that can: how MANY segments reach the cap, and how far the tips
         * actually travel. Fifty times stiffer wood must reduce both. */
        TG_EXPECT_MSG(ms.clamped_rotations < m.clamped_rotations,
                      "50x stiffer wood still clamped %u segments against %u",
                      ms.clamped_rotations, m.clamped_rotations);
        TG_EXPECT_MSG(ms.max_tip_deflection_m < m.max_tip_deflection_m,
                      "50x stiffer wood deflected %.4f m vs %.4f m",
                      (double)ms.max_tip_deflection_m,
                      (double)m.max_tip_deflection_m);
        tree_graph_destroy(&gs);
    }

    TG_T_CASE("zero sag retention produces no permanent bend");
    /* The retention parameter must genuinely control how much deflection the tree
     * grows into, not merely scale a number nothing reads. */
    {
        TreeGraph gz;
        TreeResolved rz;
        MechanicsResult mz;
        GrowthResult gr;
        TreeProfile rigid;
        TreeSettings sz = s;
        TG_EXPECT_OK(tree_profile_resolve(&sz, &rz));
        rigid = *rz.profile;
        rigid.sag_retention = 0.0f;
        rz.profile = &rigid;
        TG_EXPECT_OK(tree_graph_init(&gz, 4096, rz.max_organs));
        TG_EXPECT_OK(tree_growth_run(&gz, &rz, NULL, &gr));
        TG_EXPECT_OK(tree_growth_roots(&gz, &rz, NULL, &gr));
        TG_EXPECT_OK(tree_mechanics_run(&gz, &rz, &mz));
        TG_EXPECT_NEAR(mz.max_segment_rotation_rad, 0.0f, 1e-9);
        /* Not exactly zero: the pass still normalises an identity quaternion and
         * re-derives every base from its parent's tip, so a few micrometres of
         * float round-off accumulate along a long axis. 0.1 mm is far below any
         * feature the engine generates and orders of magnitude below the ~0.5 m
         * deflection a retaining profile produces. */
        TG_EXPECT_NEAR(mz.max_tip_deflection_m, 0.0f, 1e-4);
        TG_EXPECT_NEAR(mz.max_eccentricity, 0.0f, 1e-9);
        tree_graph_destroy(&gz);
    }

    tree_graph_destroy(&g);
}

static void test_reaction_wood(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 90.0f);
    u32 i;
    u32 loaded_with_ecc = 0, vertical_with_ecc = 0;

    TG_T_CASE("eccentricity appears on loaded segments and not on unloaded ones");
    /* Reaction wood is a response to mechanical load, so it must correlate with
     * the bending the same pass computed -- otherwise it is decoration. */
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ *o = tree_graph_organ(&g, i);
        if (!organ_type_is_segment((OrganType)o->type)) {
            TG_EXPECT(o->eccentricity == 0.0f);
            continue;
        }
        if (o->curvature > 1e-4f && o->eccentricity > 0.0f) {
            loaded_with_ecc++;
        }
        if (o->curvature <= 0.0f && o->eccentricity > 0.0f) {
            vertical_with_ecc++;
        }
    }
    TG_EXPECT_MSG(loaded_with_ecc > 20,
                  "only %u loaded segments recorded eccentricity",
                  loaded_with_ecc);
    TG_EXPECT_MSG(vertical_with_ecc == 0,
                  "%u unbent segments recorded eccentricity", vertical_with_ecc);

    TG_T_CASE("eccentricity never reaches half the radius");
    /* At 0.5 the section's centre would leave the wood entirely. */
    TG_EXPECT(m.max_eccentricity < 0.5f);
    TG_EXPECT(m.max_eccentricity <= r.profile->reaction_eccentricity + 1e-6f);

    TG_T_CASE("both categories record eccentricity, with their own magnitudes");
    /* The SIDE differs by group (tension wood above in angiosperms, compression
     * wood below in gymnosperms) and is derived from the profile at meshing time,
     * so what is checked here is that each profile's magnitude is respected. */
    {
        TreeGraph gc;
        TreeResolved rc;
        MechanicsResult mc;
        TreeSettings sc = fast_settings(TREE_CATEGORY_CONIFER, 90.0f);
        TG_EXPECT_OK(build(&gc, &sc, &rc, &mc));
        TG_EXPECT(rc.profile->reaction_wood == REACTION_COMPRESSION_LOWER);
        TG_EXPECT(r.profile->reaction_wood == REACTION_TENSION_UPPER);
        TG_EXPECT(mc.max_eccentricity > 0.0f);
        TG_EXPECT(mc.max_eccentricity <= rc.profile->reaction_eccentricity + 1e-6f);
        tree_graph_destroy(&gc);
    }

    tree_graph_destroy(&g);
}

static void test_determinism_and_immutability(void) {
    TreeGraph a, b;
    TreeResolved ra, rb;
    MechanicsResult ma, mb;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 60.0f);
    TgFingerprint fa, fb;

    TG_T_CASE("mechanics is deterministic: identical settings, identical geometry");
    TG_EXPECT_OK(build(&a, &s, &ra, &ma));
    TG_EXPECT_OK(build(&b, &s, &rb, &mb));
    fa = tree_graph_fingerprint(&a);
    fb = tree_graph_fingerprint(&b);
    TG_EXPECT_MSG(tg_fp_equal(fa, fb),
                  "two identical pipelines differ: %llx vs %llx",
                  (unsigned long long)fa.value, (unsigned long long)fb.value);
    TG_EXPECT(!fa.saw_non_finite);
    TG_EXPECT_EQ_U64(memcmp(&ma, &mb, sizeof ma), 0);

    TG_T_CASE("mechanics changes the geometry: it is not a no-op");
    /* Guards against a pass that reports numbers without touching the tree. */
    {
        TreeGraph c;
        TreeResolved rc;
        GrowthResult gr;
        TgFingerprint before;
        TG_EXPECT_OK(tree_profile_resolve(&s, &rc));
        TG_EXPECT_OK(tree_graph_init(&c, 4096, rc.max_organs));
        TG_EXPECT_OK(tree_growth_run(&c, &rc, NULL, &gr));
        TG_EXPECT_OK(tree_growth_roots(&c, &rc, NULL, &gr));
        before = tree_graph_fingerprint(&c);
        TG_EXPECT_OK(tree_mechanics_run(&c, &rc, NULL));
        TG_EXPECT(!tg_fp_equal(before, tree_graph_fingerprint(&c)));
        tree_graph_destroy(&c);
    }

    TG_T_CASE("mechanics refuses a finalized or empty graph");
    tg_test_logs_mute();
    tree_graph_finalize(&b);
    TG_EXPECT_ERR(tree_mechanics_run(&b, &rb, &mb), TG_ERR_INVALID_STATE);
    {
        TreeGraph empty;
        TG_EXPECT_OK(tree_graph_init(&empty, 8, 100));
        TG_EXPECT_ERR(tree_mechanics_run(&empty, &ra, &ma), TG_ERR_INVALID_STATE);
        TG_EXPECT_ERR(tree_mechanics_run(&empty, NULL, &ma),
                      TG_ERR_INVALID_ARGUMENT);
        tree_graph_destroy(&empty);
    }
    tg_test_logs_unmute();

    tree_graph_destroy(&a);
    tree_graph_destroy(&b);
}

static void test_age_scaling(void) {
    TreeGraph y, o;
    TreeResolved ry, ro;
    MechanicsResult my, mo;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 10.0f);

    TG_T_CASE("an older tree is thicker, heavier and sags more");
    TG_EXPECT_OK(build(&y, &s, &ry, &my));
    s.age_years = 180.0f;
    TG_EXPECT_OK(build(&o, &s, &ro, &mo));
    TG_EXPECT_MSG(mo.trunk_base_radius_m > my.trunk_base_radius_m,
                  "older trunk radius %.4f is not above younger %.4f",
                  (double)mo.trunk_base_radius_m, (double)my.trunk_base_radius_m);
    TG_EXPECT(mo.total_wood_mass_kg > my.total_wood_mass_kg);
    TG_EXPECT(mo.total_leaf_area_m2 > my.total_leaf_area_m2);
    TG_EXPECT_MSG(mo.max_tip_deflection_m > my.max_tip_deflection_m,
                  "older tree deflects %.4f m, younger %.4f m",
                  (double)mo.max_tip_deflection_m,
                  (double)my.max_tip_deflection_m);

    TG_T_CASE("physiological age is recorded, oldest at the base");
    /* Note what is NOT asserted: that the trunk's last segment is the youngest
     * tissue in the tree. It usually is not. The leader stops extending once it
     * reaches the target height, so its final segment can be decades old while
     * shoots elsewhere are still growing -- an earlier version of this test
     * assumed otherwise and failed on a perfectly correct tree. */
    {
        const Axis *trunk = tree_graph_axis(&o, o.trunk_axis);
        const Organ *base = tree_graph_organ(&o, trunk->first_organ);
        f32 youngest = 1e9f, oldest = -1.0f;
        u32 i;
        for (i = 0; i < tree_graph_organ_count(&o); ++i) {
            const Organ *org = tree_graph_organ(&o, i);
            if (!organ_type_is_segment((OrganType)org->type)) { continue; }
            youngest = tg_minf(youngest, org->physiological_age);
            oldest = tg_maxf(oldest, org->physiological_age);
        }
        TG_EXPECT_MSG(base->physiological_age >= oldest - 1e-4f,
                      "the trunk base (%.2f yr) is not the oldest tissue "
                      "(%.2f yr)", (double)base->physiological_age,
                      (double)oldest);
        TG_EXPECT_NEAR(youngest, 0.0f, 1e-6);
        TG_EXPECT_MSG(oldest > 10.0f,
                      "oldest tissue is only %.2f yr on a 180-year tree",
                      (double)oldest);
    }

    TG_T_CASE("a seedling also gets valid radii and mechanics");
    {
        TreeGraph tiny;
        TreeResolved rt;
        MechanicsResult mt;
        GraphValidateReport rep;
        TreeSettings st = fast_settings(TREE_CATEGORY_BROADLEAF, 1.0f);
        TG_EXPECT_OK(build(&tiny, &st, &rt, &mt));
        TG_EXPECT(mt.trunk_base_radius_m > 0.0f);
        TG_EXPECT(mt.max_radius_m >= mt.min_radius_m);
        TG_EXPECT_OK(tree_graph_validate(&tiny, 0.0f, &rep));
        TG_EXPECT(rep.passed);
        tree_graph_destroy(&tiny);
    }

    tree_graph_destroy(&y);
    tree_graph_destroy(&o);
}

static void test_roots_are_sized_independently(void) {
    TreeGraph g;
    TreeResolved r;
    MechanicsResult m;
    TreeSettings s = fast_settings(TREE_CATEGORY_BROADLEAF, 80.0f);
    u32 i;
    f32 max_root_r = 0.0f;
    u32 roots = 0, thinner_than_tip = 0;

    TG_T_CASE("roots get radii even though they bear no foliage");
    /* Roots carry no leaves, so a purely foliage-driven pipe model would leave
     * every root at the tip radius. They are sized from what they anchor instead. */
    TG_EXPECT_OK(build(&g, &s, &r, &m));
    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ *o = tree_graph_organ(&g, i);
        if (o->type != ORGAN_ROOT_SEGMENT) { continue; }
        roots++;
        if (o->radius_base > max_root_r) { max_root_r = o->radius_base; }
        if (o->radius_base <= r.profile->tip_radius_m * 1.001f) {
            thinner_than_tip++;
        }
        TG_EXPECT(o->supported_leaf_area == 0.0f ||
                  o->supported_leaf_area >= 0.0f);
    }
    TG_EXPECT(roots > 20);
    TG_EXPECT_MSG(max_root_r > r.profile->tip_radius_m * 20.0f,
                  "thickest root is only %.5f m", (double)max_root_r);
    TG_EXPECT_MSG(thinner_than_tip * 2u < roots,
                  "%u of %u root segments collapsed to the tip radius",
                  thinner_than_tip, roots);

    TG_T_CASE("major roots are comparable to, but thinner than, the trunk base");
    TG_EXPECT_MSG(max_root_r < m.trunk_base_radius_m,
                  "thickest root %.4f m is not thinner than the trunk base %.4f m",
                  (double)max_root_r, (double)m.trunk_base_radius_m);
    TG_EXPECT_MSG(max_root_r > m.trunk_base_radius_m * 0.10f,
                  "thickest root %.4f m is negligible against the trunk base "
                  "%.4f m", (double)max_root_r, (double)m.trunk_base_radius_m);

    TG_T_CASE("roots taper with distance from the stem");
    {
        f32 near_sum = 0.0f, far_sum = 0.0f;
        u32 near_n = 0, far_n = 0;
        for (i = 0; i < tree_graph_organ_count(&g); ++i) {
            const Organ *o = tree_graph_organ(&g, i);
            f32 d;
            if (o->type != ORGAN_ROOT_SEGMENT) { continue; }
            if (o->branch_order != 0) { continue; }
            d = v3_len(v3(o->base.x, 0.0f, o->base.z));
            if (d < r.root_spread_m * 0.25f) { near_sum += o->radius_base; near_n++; }
            else if (d > r.root_spread_m * 0.6f) { far_sum += o->radius_base; far_n++; }
        }
        if (near_n > 2 && far_n > 2) {
            TG_EXPECT_MSG(near_sum / (f32)near_n > far_sum / (f32)far_n,
                          "near-stem roots average %.4f m, distal %.4f m",
                          (double)(near_sum / (f32)near_n),
                          (double)(far_sum / (f32)far_n));
        } else {
            TG_EXPECT_MSG(false, "too few root samples: %u near, %u far",
                          near_n, far_n);
        }
    }

    tree_graph_destroy(&g);
}

void test_suite_tree_mechanics(void) {
    test_radii_are_assigned_and_consistent();
    test_basal_flare();
    test_pipe_model_area_relationship();
    test_mass_is_plausible();
    test_deflection_behaviour();
    test_reaction_wood();
    test_determinism_and_immutability();
    test_age_scaling();
    test_roots_are_sized_independently();
}
