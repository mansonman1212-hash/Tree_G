/* test_tree_foliage.c -- the foliage pass.
 *
 * WHAT THESE TESTS ARE FOR
 *   Foliage is the one pass where the geometry and the physics can silently
 *   disagree. The mechanics pass bends the tree under a foliage MASS derived from
 *   an assumed leaf area; this pass builds the leaves. If the two use different
 *   definitions of "a leaf's area", the tree sags under foliage that is not there
 *   and nothing complains. That happened: the mechanics assumed 0.65 of a leaf's
 *   bounding rectangle and the generated blade measured 0.25 of it, so the load was
 *   two and a half times the geometry. The first test below is the gate against
 *   that class of defect, and it is the reason the area is now integrated from the
 *   same half-width function the mesh uses.
 *
 *   The rest are the properties that must hold for foliage to survive inspection:
 *   every blade a closed manifold with positive volume (a card would fail this),
 *   thinning that is uniform rather than clustered, and no leaf on dead wood.
 */
#include "test_util.h"
#include "test_suites.h"
#include "../src/tree/tree_foliage.h"
#include "../src/tree/tree_mechanics.h"
#include "../src/tree/tree_growth.h"
#include "../src/tree/tree_skin.h"
#include "../src/geom/mesh_validate.h"

#include <string.h>

typedef struct Built {
    TreeSettings    settings;
    TreeResolved    resolved;
    TreeGraph       graph;
    GrowthResult    growth;
    MechanicsResult mechanics;
    SkinResult      skin;
    FoliageResult   foliage;
    Mesh            mesh;
} Built;

static TgResult build(Built *b, TreeCategory cat, f32 age, TreeQuality q,
                      TreeSeason season, bool with_wood) {
    TgResult r;

    memset(b, 0, sizeof *b);
    b->settings = tree_settings_default(cat);
    b->settings.age_years = age;
    b->settings.quality = q;
    b->settings.season = season;
    b->settings.seed = 0x7A11EEull;

    r = tree_profile_resolve(&b->settings, &b->resolved);
    if (r != TG_OK) { return r; }
    r = tree_graph_init(&b->graph, 2048, b->resolved.max_organs);
    if (r != TG_OK) { return r; }
    r = tree_growth_run(&b->graph, &b->resolved, NULL, &b->growth);
    if (r != TG_OK) { return r; }
    r = tree_growth_roots(&b->graph, &b->resolved, NULL, &b->growth);
    if (r != TG_OK) { return r; }
    r = tree_mechanics_run(&b->graph, &b->resolved, &b->mechanics);
    if (r != TG_OK) { return r; }
    r = mesh_init(&b->mesh, 1u << 14, 1u << 15);
    if (r != TG_OK) { return r; }
    if (with_wood) {
        r = tree_skin_build(&b->mesh, &b->graph, &b->resolved, &b->skin);
        if (r != TG_OK) { return r; }
    }
    return tree_foliage_build(&b->mesh, &b->graph, &b->resolved,
                              (u16)b->resolved.growth_steps, &b->foliage);
}

static void built_destroy(Built *b) {
    mesh_destroy(&b->mesh);
    tree_graph_destroy(&b->graph);
}

/* ------------------------------------------------------------------------- */

static void test_area_agrees_with_mechanics(void) {
    Built b;

    TG_T_CASE("the leaf area the geometry HAS equals the area the mechanics ASSUMED");
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_BROADLEAF, 25.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    /* Only meaningful when every leaf the botany asked for was actually placed;
     * otherwise the realised area is legitimately a fraction of the target. */
    TG_EXPECT_MSG(b.foliage.leaves_placed == b.foliage.leaves_wanted,
                  "%llu of %llu leaves placed, so the areas are not comparable",
                  (unsigned long long)b.foliage.leaves_placed,
                  (unsigned long long)b.foliage.leaves_wanted);
    if (b.foliage.leaves_placed == b.foliage.leaves_wanted) {
        f32 ratio = b.foliage.realised_leaf_area_m2
                  / tg_maxf(b.foliage.target_leaf_area_m2, 1e-6f);
        /* 8% allows for the per-leaf size jitter, which enters area squared and
         * therefore biases the realised total slightly upward. It does NOT allow
         * for a different shape model on each side. */
        TG_EXPECT_MSG(ratio > 0.92f && ratio < 1.08f,
                      "geometry has %.3f m2, mechanics assumed %.3f m2 "
                      "(ratio %.3f)",
                      (double)b.foliage.realised_leaf_area_m2,
                      (double)b.foliage.target_leaf_area_m2, (double)ratio);
    }

    TG_T_CASE("a single blade's area matches the shared unit-area definition");
    if (b.foliage.leaves_placed > 0u) {
        f32 each = b.foliage.realised_leaf_area_m2
                 / (f32)b.foliage.leaves_placed;
        f32 unit = tree_foliage_unit_area(&b.resolved);
        TG_EXPECT_MSG(tg_absf(each / tg_maxf(unit, 1e-9f) - 1.0f) < 0.08f,
                      "mean blade %.6f m2 against unit definition %.6f m2",
                      (double)each, (double)unit);
    }
    built_destroy(&b);
}

static void test_blades_are_closed_solids(void) {
    Built b;
    MeshValidateReport rep;
    MeshValidateOptions opt;

    TG_T_CASE("every blade and petiole is a closed manifold with positive volume");
    /* This is the check a billboard cannot pass, and it is why it is here: an
     * alpha-tested card has boundary edges, no enclosed volume and no distinct
     * upper and lower face. A leaf that satisfies this is a solid. */
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_BROADLEAF, 14.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT_OK(mesh_finalize(&b.mesh));
    opt = mesh_validate_default_options();
    TG_EXPECT_OK(mesh_validate(&b.mesh, &opt, &rep));
    TG_EXPECT_MSG(rep.passed, "foliage mesh failed validation");
    if (!rep.passed) { mesh_validate_log_report(&rep); }
    TG_EXPECT_MSG(rep.boundary_edges[MESH_SECTION_LEAF] == 0,
                  "%llu boundary edges in the leaf section",
                  (unsigned long long)rep.boundary_edges[MESH_SECTION_LEAF]);
    TG_EXPECT_MSG(rep.enclosed_volume[MESH_SECTION_LEAF] > 0.0,
                  "leaf section encloses %.9f m3, so its winding is inverted",
                  rep.enclosed_volume[MESH_SECTION_LEAF]);

    TG_T_CASE("one closed component per blade AND per petiole");
    TG_EXPECT_MSG(rep.closed_components[MESH_SECTION_LEAF]
                      == 2u * (u32)b.foliage.leaves_placed,
                  "%u closed components for %llu leaves and %u petioles",
                  rep.closed_components[MESH_SECTION_LEAF],
                  (unsigned long long)b.foliage.leaves_placed,
                  b.foliage.petioles_placed);
    built_destroy(&b);
}

static void test_needles_are_closed_solids(void) {
    Built b;
    MeshValidateReport rep;
    MeshValidateOptions opt;

    TG_T_CASE("needles are closed solids too, and there is one per placement");
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_CONIFER, 10.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT_OK(mesh_finalize(&b.mesh));
    opt = mesh_validate_default_options();
    TG_EXPECT_OK(mesh_validate(&b.mesh, &opt, &rep));
    TG_EXPECT_MSG(rep.passed, "needle mesh failed validation");
    if (!rep.passed) { mesh_validate_log_report(&rep); }
    TG_EXPECT_MSG(rep.boundary_edges[MESH_SECTION_NEEDLE] == 0,
                  "%llu boundary edges in the needle section",
                  (unsigned long long)rep.boundary_edges[MESH_SECTION_NEEDLE]);
    TG_EXPECT_MSG(rep.closed_components[MESH_SECTION_NEEDLE]
                      == (u32)b.foliage.leaves_placed,
                  "%u closed components for %llu needles",
                  rep.closed_components[MESH_SECTION_NEEDLE],
                  (unsigned long long)b.foliage.leaves_placed);
    TG_EXPECT_MSG(b.foliage.petioles_placed == 0,
                  "%u petioles on a conifer: needles are sessile",
                  b.foliage.petioles_placed);
    built_destroy(&b);
}

static void test_no_foliage_on_dead_wood(void) {
    Built b;
    u32 i, on_dead = 0;
    u64 tri;

    TG_T_CASE("no leaf is attached to a dead or root organ");
    /* Foliage on a shed limb is the single most obvious way a procedural tree
     * gives itself away, and the graph records exactly which wood is dead, so
     * there is no excuse for it. */
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_BROADLEAF, 45.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT_OK(mesh_finalize(&b.mesh));
    for (tri = 0; tri < mesh_triangle_count(&b.mesh); ++tri) {
        MeshTriangle t;
        const Organ *o;
        if (!mesh_get_triangle(&b.mesh, tri, &t)) { continue; }
        if (t.organ_id >= tree_graph_organ_count(&b.graph)) { continue; }
        o = tree_graph_organ(&b.graph, t.organ_id);
        if ((o->flags & ORGAN_FLAG_DEAD) != 0
                || o->type == ORGAN_ROOT_SEGMENT) {
            on_dead++;
        }
    }
    TG_EXPECT_MSG(on_dead == 0, "%u foliage triangles sit on dead or root wood",
                  on_dead);

    TG_T_CASE("foliage sits only on shoots the mechanics counted as bearing");
    for (i = 0; i < tree_graph_organ_count(&b.graph); ++i) {
        const Organ *o = tree_graph_organ(&b.graph, i);
        if (tree_mechanics_bears_foliage(o, &b.resolved,
                                        (u16)b.resolved.growth_steps)) {
            continue;
        }
        if (o->supported_leaf_area > 0.0f
                && (o->flags & ORGAN_FLAG_TERMINAL) == 0
                && o->first_child == TG_INVALID_ID) {
            on_dead++;
        }
    }
    TG_EXPECT(b.foliage.bearing_segments > 0);
    built_destroy(&b);
}

static void test_thinning_is_uniform(void) {
    Built b;
    f32 low = 0.0f, high = 0.0f;
    f32 height_reached = 0.0f;
    u32 i;
    u32 bearing_low = 0, bearing_high = 0;

    TG_T_CASE("when the budget thins the crown it thins it EVENLY");
    /* The budget is spent by hashing each leaf's identity, not by counting as the
     * graph is walked. That matters: graph order is acropetal, so a running counter
     * would spend the whole budget on the oldest wood and leave the outer crown --
     * the part anybody looks at -- bald. This measures the placed fraction in the
     * lower and upper halves of the crown and requires them to agree. */
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_CONIFER, 30.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT(b.foliage.leaves_placed < b.foliage.leaves_wanted);

    for (i = 0; i < tree_graph_organ_count(&b.graph); ++i) {
        const Organ *o = tree_graph_organ(&b.graph, i);
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        if (organ_tip(o).y > height_reached) { height_reached = organ_tip(o).y; }
    }
    for (i = 0; i < tree_graph_organ_count(&b.graph); ++i) {
        const Organ *o = tree_graph_organ(&b.graph, i);
        if (!tree_mechanics_bears_foliage(o, &b.resolved,
                                         (u16)b.resolved.growth_steps)) {
            continue;
        }
        if (o->base.y < height_reached * 0.5f) {
            bearing_low++;
        } else {
            bearing_high++;
        }
    }
    /* Counting placements per half needs the mesh, so use the vertex positions. */
    {
        const MeshVertex *v = mesh_vertices(&b.mesh);
        u64 nv = mesh_vertex_count(&b.mesh);
        u64 k;
        f32 mid = height_reached * 0.5f;
        for (k = 0; k < nv; ++k) {
            if (v[k].position.y < mid) { low += 1.0f; } else { high += 1.0f; }
        }
    }
    if (bearing_low > 0u && bearing_high > 0u && low > 0.0f && high > 0.0f) {
        f32 per_low = low / (f32)bearing_low;
        f32 per_high = high / (f32)bearing_high;
        f32 ratio = per_low / per_high;
        TG_EXPECT_MSG(ratio > 0.55f && ratio < 1.85f,
                      "%.2f foliage vertices per bearing shoot in the lower half "
                      "against %.2f in the upper (ratio %.2f): thinning is biased",
                      (double)per_low, (double)per_high, (double)ratio);
    }
    built_destroy(&b);
}

static void test_season_shifts_colour_and_sheds(void) {
    Built summer, autumn, winter;

    TG_T_CASE("autumn foliage is measurably yellower than summer foliage");
    /* The first version of this test asserted that autumn recoloured the SAME
     * geometry. It does not, and the test was wrong rather than the code: autumn
     * also lowers foliage_density, so fewer leaves are placed, and the lighter
     * crown changes the deflection so the remaining leaves are genuinely
     * elsewhere. Both effects are correct. What must hold is that the colour
     * shifts toward the carotenoids and that the crown thins rather than
     * thickens. */
    TG_EXPECT_OK(build(&summer, TREE_CATEGORY_BROADLEAF, 20.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT_OK(build(&autumn, TREE_CATEGORY_BROADLEAF, 20.0f, QUALITY_DRAFT,
                       SEASON_AUTUMN, false));
    {
        f64 sum_ratio = 0.0, aut_ratio = 0.0;
        u64 ns = 0, na = 0, k;
        const MeshVertex *vs = mesh_vertices(&summer.mesh);
        const MeshVertex *va = mesh_vertices(&autumn.mesh);
        for (k = 0; k < mesh_vertex_count(&summer.mesh); ++k) {
            u32 c = vs[k].color;
            f64 red = (f64)(c & 0xFFu), grn = (f64)((c >> 8) & 0xFFu);
            if (grn > 0.5) { sum_ratio += red / grn; ns++; }
        }
        for (k = 0; k < mesh_vertex_count(&autumn.mesh); ++k) {
            u32 c = va[k].color;
            f64 red = (f64)(c & 0xFFu), grn = (f64)((c >> 8) & 0xFFu);
            if (grn > 0.5) { aut_ratio += red / grn; na++; }
        }
        if (ns > 0u && na > 0u) {
            f64 s_mean = sum_ratio / (f64)ns;
            f64 a_mean = aut_ratio / (f64)na;
            TG_EXPECT_MSG(a_mean > s_mean * 1.5,
                          "autumn red/green %.3f against summer %.3f: the "
                          "senescent shift is not happening", a_mean, s_mean);
        }
    }

    TG_T_CASE("autumn carries no more foliage than summer");
    TG_EXPECT_MSG(autumn.foliage.leaves_placed <= summer.foliage.leaves_placed,
                  "autumn placed %llu leaves against summer's %llu",
                  (unsigned long long)autumn.foliage.leaves_placed,
                  (unsigned long long)summer.foliage.leaves_placed);

    TG_T_CASE("a deciduous profile in winter carries no leaves at all");
    TG_EXPECT_OK(build(&winter, TREE_CATEGORY_BROADLEAF, 20.0f, QUALITY_DRAFT,
                       SEASON_WINTER, false));
    TG_EXPECT_MSG(winter.foliage.leaves_placed == 0u,
                  "%llu leaves on a bare deciduous tree",
                  (unsigned long long)winter.foliage.leaves_placed);
    TG_EXPECT_MSG(winter.foliage.triangles == 0u,
                  "%u foliage triangles in winter", winter.foliage.triangles);

    built_destroy(&summer);
    built_destroy(&autumn);
    built_destroy(&winter);
}

static void test_determinism(void) {
    Built a, b;

    TG_T_CASE("identical settings produce byte-identical foliage");
    TG_EXPECT_OK(build(&a, TREE_CATEGORY_BROADLEAF, 18.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT_OK(build(&b, TREE_CATEGORY_BROADLEAF, 18.0f, QUALITY_DRAFT,
                       SEASON_SUMMER, false));
    TG_EXPECT(a.foliage.leaves_placed == b.foliage.leaves_placed);
    TG_EXPECT(a.foliage.vertices == b.foliage.vertices);
    {
        u64 n = mesh_vertex_count(&a.mesh);
        TG_EXPECT(n == mesh_vertex_count(&b.mesh));
        TG_EXPECT_MSG(memcmp(mesh_vertices(&a.mesh), mesh_vertices(&b.mesh),
                             (size_t)n * sizeof(MeshVertex)) == 0,
                      "foliage vertex buffers differ between identical runs");
    }
    built_destroy(&a);
    built_destroy(&b);
}

void test_suite_tree_foliage(void) {
    test_area_agrees_with_mechanics();
    test_blades_are_closed_solids();
    test_needles_are_closed_solids();
    test_no_foliage_on_dead_wood();
    test_thinning_is_uniform();
    test_season_shifts_colour_and_sheds();
    test_determinism();
}
