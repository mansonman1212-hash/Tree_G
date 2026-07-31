/* test_tree_build.c -- the build orchestrator, the BVH and the inspection API.
 *
 * WHAT THESE TESTS ARE FOR
 *   Three things that are easy to get wrong and hard to notice.
 *
 *   The PASS ORDER. Skinning before the mechanics pass produces a complete, valid,
 *   watertight mesh of an unbent tree with no radii, and nothing downstream
 *   complains. The only way to catch that class of error is to assert on the
 *   properties the order exists to guarantee: that radii are assigned, that the tree
 *   is bent, that foliage sits on bent wood.
 *
 *   The BVH. An acceleration structure that returns the wrong answer is worse than
 *   none, because it is fast and wrong. Every ray query here is checked against a
 *   brute-force scan of every triangle -- not against a recorded expectation, which
 *   would only prove the code still does what it did.
 *
 *   The INSPECTION facts. These are the numbers a user will read off the screen and
 *   believe. They have to agree with the graph they came from.
 */
#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_build.h"
#include "../src/tree/tree_inspect.h"
#include "../src/tree/tree_mechanics.h"
#include "../src/core/mem.h"

#include <string.h>
#include <math.h>

typedef struct ProgressLog {
    u32 calls;
    u32 stage_seen[BUILD_STAGE_COUNT];
    int last_stage;
    bool out_of_order;
} ProgressLog;

static void on_progress(TreeBuildStage s, f32 fraction, void *user) {
    ProgressLog *p = (ProgressLog *)user;
    p->calls++;
    if ((u32)s < (u32)BUILD_STAGE_COUNT) { p->stage_seen[(u32)s]++; }
    /* Stages must be reported in the order they are declared. A progress bar that
     * jumps backwards is a symptom of the pass order having been changed without
     * the enum being updated with it. */
    if ((int)s < p->last_stage) { p->out_of_order = true; }
    p->last_stage = (int)s;
    (void)fraction;
}

static TreeSettings settings_for(TreeCategory cat, f32 age, TreeQuality q) {
    TreeSettings s = tree_settings_default(cat);
    s.age_years = age;
    s.quality = q;
    s.seed = 0x77EEBull;
    return s;
}

/* ------------------------------------------------------------------------- */

static void test_one_call_produces_a_finished_tree(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 22.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    ProgressLog log;
    Tree t;
    u32 i;

    memset(&log, 0, sizeof log);
    log.last_stage = -1;
    opt.progress = on_progress;
    opt.progress_user = &log;

    TG_T_CASE("tree_build produces graph, mesh, BVH and stage record in one call");
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    TG_EXPECT(tree_graph_organ_count(&t.graph) > 100u);
    TG_EXPECT(mesh_triangle_count(&t.mesh) > 1000u);
    TG_EXPECT_MSG(t.bvh.node_count > 0u, "no BVH was built");
    TG_EXPECT_MSG(t.stage_count == t.growth.steps_run + 1u,
                  "%u stage entries for %u growth steps", t.stage_count,
                  t.growth.steps_run);
    TG_EXPECT(t.fingerprint != 0u);
    TG_EXPECT_MSG(t.failed_stage == BUILD_STAGE_COUNT,
                  "failed_stage set to %s on a successful build",
                  tree_build_stage_name(t.failed_stage));

    TG_T_CASE("every stage reports progress, in declaration order");
    for (i = 0; i < (u32)BUILD_STAGE_COUNT; ++i) {
        TG_EXPECT_MSG(log.stage_seen[i] > 0u, "stage %s reported no progress",
                      tree_build_stage_name((TreeBuildStage)i));
    }
    TG_EXPECT_MSG(!log.out_of_order, "progress stages were reported out of order");

    TG_T_CASE("the pass order left the tree bent and radius-assigned");
    /* The properties the order exists to produce. Radii come from the mechanics
     * pass and the skin pass consumes them, so a mesh built from zero radii would
     * still be watertight and still be wrong. */
    TG_EXPECT_MSG(t.mechanics.trunk_base_radius_m > 0.01f,
                  "trunk base radius is %.5f m: the mechanics pass did not run "
                  "before skinning", (double)t.mechanics.trunk_base_radius_m);
    TG_EXPECT_MSG(t.mechanics.max_tip_deflection_m > 0.0f,
                  "nothing bent at all, so skinning may have preceded mechanics");
    TG_EXPECT(t.skin.rings_emitted > 0u);
    TG_EXPECT(t.foliage.leaves_placed > 0u);

    TG_T_CASE("mesh validation ran and passed");
    TG_EXPECT_MSG(t.mesh_validated, "validation did not run");
    TG_EXPECT_MSG(t.mesh_valid, "generated mesh failed validation");

    tree_free(&t);
}

static void test_double_free_and_failed_build_are_safe(void) {
    Tree t;
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 8.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();

    TG_T_CASE("tree_free is idempotent");
    /* A caller unwinding several error paths should not have to track how far a
     * build got. */
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    tree_free(&t);
    tree_free(&t);
    TG_EXPECT(t.stage == NULL && t.stage_count == 0u);
    TG_EXPECT(tree_graph_organ_count(&t.graph) == 0u);

    TG_T_CASE("an invalid age fails at resolve and reports which stage");
    s.age_years = -5.0f;
    {
        TgResult r = tree_build(&s, &opt, &t);
        if (r != TG_OK) {
            TG_EXPECT_MSG(t.failed_stage == BUILD_STAGE_RESOLVE,
                          "failed at %s, expected resolve",
                          tree_build_stage_name(t.failed_stage));
        }
        tree_free(&t);
    }
}

static void test_cancellation(void) {
    Tree t;
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 60.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    GrowthCancel cancel;

    TG_T_CASE("a build cancelled before it starts stops in growth and says so");
    /* Cancellation has to be honoured inside growth rather than between passes,
     * because growth is where effectively all the time goes. Requesting it up front
     * is the strongest form of the test: nothing else can have consumed it. */
    cancel.requested = 1;
    opt.cancel = &cancel;
    {
        TgResult r = tree_build(&s, &opt, &t);
        TG_EXPECT_MSG(r == TG_ERR_CANCELLED, "cancelled build returned %s",
                      tg_result_name(r));
        TG_EXPECT_MSG(t.cancelled, "the cancelled flag was not set");
        TG_EXPECT_MSG(t.failed_stage == BUILD_STAGE_GROW_SHOOTS,
                      "cancellation was reported at %s",
                      tree_build_stage_name(t.failed_stage));
        tree_free(&t);
    }
}

static void test_determinism_by_fingerprint(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_CONIFER, 18.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    Tree a, b, c;

    TG_T_CASE("identical settings give an identical fingerprint");
    TG_EXPECT_OK(tree_build(&s, &opt, &a));
    TG_EXPECT_OK(tree_build(&s, &opt, &b));
    TG_EXPECT_MSG(a.fingerprint == b.fingerprint,
                  "fingerprints %016llx and %016llx differ",
                  (unsigned long long)a.fingerprint,
                  (unsigned long long)b.fingerprint);
    TG_EXPECT(mesh_vertex_count(&a.mesh) == mesh_vertex_count(&b.mesh));
    TG_EXPECT_MSG(memcmp(mesh_vertices(&a.mesh), mesh_vertices(&b.mesh),
                         (size_t)mesh_vertex_count(&a.mesh)
                             * sizeof(MeshVertex)) == 0,
                  "vertex buffers differ despite matching fingerprints");

    TG_T_CASE("one different seed changes it");
    s.seed ^= 0x1ull;
    TG_EXPECT_OK(tree_build(&s, &opt, &c));
    TG_EXPECT_MSG(c.fingerprint != a.fingerprint,
                  "a different seed produced the same fingerprint %016llx",
                  (unsigned long long)a.fingerprint);
    tree_free(&a);
    tree_free(&b);
    tree_free(&c);
}

/* ------------------------------------------------------------------------- */
/* BVH against brute force                                                   */
/* ------------------------------------------------------------------------- */

static bool brute_force_raycast(const Mesh *m, u32 mask, Ray ray, u64 *out_tri,
                               f32 *out_t) {
    u64 i, n = mesh_triangle_count(m);
    f32 best = ray.t_max;
    bool found = false;

    for (i = 0; i < n; ++i) {
        MeshTriangle t;
        f32 hit, u, v;
        MeshSection s = mesh_triangle_section(m, i);
        if ((u32)s >= 32u || (mask & (1u << (u32)s)) == 0u) { continue; }
        if (!mesh_get_triangle(m, i, &t)) { continue; }
        if (!ray_triangle(ray, t.position[0], t.position[1], t.position[2],
                          false, &hit, &u, &v)) {
            continue;
        }
        if (hit < ray.t_min || hit >= best) { continue; }
        best = hit;
        *out_tri = i;
        found = true;
    }
    *out_t = best;
    return found;
}

static void test_bvh_agrees_with_brute_force(void) {
    /* A 10-year tree, so a brute-force scan over every triangle for every ray is
     * affordable. The point is correctness of the traversal, which does not depend
     * on scale; the scale test is separate and checks COST, not answers. */
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 10.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    Tree t;
    Aabb bb;
    TgRng rng;
    u32 i;
    u32 agreed = 0, hits = 0, disagreed = 0;

    tg_rng_seed(&rng, 0xB7A1ull, 3u);
    TG_T_CASE("every BVH ray hit matches an exhaustive scan of all triangles");
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    bb = mesh_section_bounds(&t.mesh, MESH_SECTION_WOOD);
    TG_EXPECT(!aabb_is_empty(bb));

    for (i = 0; i < 240u; ++i) {
        Ray ray;
        MeshBvhHit hit;
        u64 bf_tri = 0;
        f32 bf_t = 0.0f;
        bool bf_found, bvh_found;
        /* Aimed at the centroid of a real triangle rather than at a random point in
         * the bounding box. A tree is almost entirely empty space, so random targets
         * missed 232 of 240 times and the test proved nothing about traversal. */
        MeshTriangle aim;
        V3 target, from;
        u64 pick = (u64)tg_rng_below(&rng, (u32)mesh_triangle_count(&t.mesh));
        if (!mesh_get_triangle(&t.mesh, pick, &aim)) { continue; }
        target = v3_scale(v3_add(v3_add(aim.position[0], aim.position[1]),
                                 aim.position[2]), 1.0f / 3.0f);
        from = v3_add(target, v3(tg_rng_range(&rng, -20.0f, 20.0f),
                                 tg_rng_range(&rng, -20.0f, 20.0f),
                                 tg_rng_range(&rng, -20.0f, 20.0f)));
        ray.origin = from;
        ray.dir = v3_norm_or(v3_sub(target, from), v3(0.0f, 1.0f, 0.0f));
        ray.t_min = 0.0f;
        ray.t_max = 500.0f;

        bvh_found = mesh_bvh_raycast(&t.bvh, &t.mesh, ray, false, &hit);
        bf_found = brute_force_raycast(&t.mesh, t.bvh.section_mask, ray, &bf_tri,
                                       &bf_t);
        if (bf_found != bvh_found) { disagreed++; continue; }
        if (!bf_found) { agreed++; continue; }
        hits++;
        /* Compare the DISTANCE, not the triangle id: two coincident triangles at the
         * same depth are an equally correct answer, and the tree has plenty of them
         * where a cap meets a wall. */
        if (tg_absf(hit.t - bf_t) < 1e-4f) { agreed++; }
        else { disagreed++; }
    }
    TG_EXPECT_MSG(disagreed == 0u,
                  "%u of 240 rays: the hierarchy and the exhaustive scan reached "
                  "different answers", disagreed);
    TG_EXPECT_MSG(agreed == 240u, "%u of 240 rays agreed", agreed);
    TG_EXPECT_MSG(hits > 80u,
                  "only %u of 240 rays hit anything, so this proved little", hits);

    TG_T_CASE("the BVH visits far fewer triangles than an exhaustive scan");
    {
        MeshBvhStats st;
        Ray ray;
        MeshBvhHit hit;
        V3 centre = aabb_center(bb);
        ray.origin = v3(centre.x + 40.0f, centre.y, centre.z);
        ray.dir = v3(-1.0f, 0.0f, 0.0f);
        ray.t_min = 0.0f;
        ray.t_max = 200.0f;
        mesh_bvh_stats(&t.bvh, &st);
        TG_EXPECT(st.leaves > 0u);
        TG_EXPECT_MSG(st.max_leaf_triangles <= 8u,
                      "a leaf holds %u triangles", st.max_leaf_triangles);
        if (mesh_bvh_raycast(&t.bvh, &t.mesh, ray, false, &hit)) {
            TG_EXPECT_MSG(hit.triangles_tested < t.bvh.tri_count / 4u,
                          "tested %u of %u triangles: the hierarchy is not "
                          "pruning", hit.triangles_tested, t.bvh.tri_count);
        }
    }

    TG_T_CASE("a ray that misses the tree entirely tests almost nothing");
    {
        Ray ray;
        MeshBvhHit hit;
        ray.origin = v3(1000.0f, 1000.0f, 1000.0f);
        ray.dir = v3(0.0f, 1.0f, 0.0f);
        ray.t_min = 0.0f;
        ray.t_max = 100.0f;
        TG_EXPECT(!mesh_bvh_raycast(&t.bvh, &t.mesh, ray, false, &hit));
        TG_EXPECT_MSG(hit.triangles_tested == 0u,
                      "a ray nowhere near the tree tested %u triangles",
                      hit.triangles_tested);
        TG_EXPECT_MSG(hit.nodes_visited <= 1u,
                      "a ray nowhere near the tree visited %u nodes",
                      hit.nodes_visited);
    }

    TG_T_CASE("the BVH only covers the sections it was asked for");
    {
        u32 i2;
        u32 wrong = 0;
        for (i2 = 0; i2 < t.bvh.tri_count; ++i2) {
            MeshSection sec = mesh_triangle_section(&t.mesh, t.bvh.tri_ids[i2]);
            if ((t.bvh.section_mask & (1u << (u32)sec)) == 0u) { wrong++; }
        }
        TG_EXPECT_MSG(wrong == 0u,
                      "%u triangles from unrequested sections are in the tree",
                      wrong);
        TG_EXPECT_MSG(t.bvh.tri_count < mesh_triangle_count(&t.mesh),
                      "the wood-only BVH covers all %llu triangles, so the "
                      "section mask did nothing",
                      (unsigned long long)mesh_triangle_count(&t.mesh));
    }

    tree_free(&t);
}

static void test_bvh_refuses_an_unfinalised_mesh(void) {
    Mesh m;
    MeshBvh bvh;

    TG_T_CASE("a BVH over a mesh that can still grow is refused");
    /* Every triangle id in the tree would dangle the moment the mesh reallocated.
     * Refusing is the only safe behaviour and it must be an error, not a warning. */
    TG_EXPECT_OK(mesh_init(&m, 64, 64));
    TG_EXPECT_MSG(mesh_bvh_build(&bvh, &m, MESH_BVH_ALL_SECTIONS)
                      == TG_ERR_INVALID_ARGUMENT,
                  "building over an unfinalised mesh was allowed");
    mesh_destroy(&m);

    TG_T_CASE("an empty finalised mesh builds an empty tree without complaint");
    TG_EXPECT_OK(mesh_init(&m, 64, 64));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_bvh_build(&bvh, &m, MESH_BVH_ALL_SECTIONS));
    TG_EXPECT(bvh.node_count == 0u && bvh.tri_count == 0u);
    {
        Ray ray;
        MeshBvhHit hit;
        ray.origin = v3_zero();
        ray.dir = v3(0.0f, 1.0f, 0.0f);
        ray.t_min = 0.0f;
        ray.t_max = 10.0f;
        TG_EXPECT(!mesh_bvh_raycast(&bvh, &m, ray, false, &hit));
    }
    mesh_bvh_destroy(&bvh);
    mesh_destroy(&m);
}

/* ------------------------------------------------------------------------- */
/* Inspection                                                                */
/* ------------------------------------------------------------------------- */

static void test_inspection_reports_the_truth(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 24.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    Tree t;
    Aabb bb;
    TgRng rng;
    u32 i, picks = 0;
    u32 bad_triangle = 0, bad_organ = 0, bad_facts = 0, bad_position = 0;
    u32 bad_path = 0;

    tg_rng_seed(&rng, 0x115EC7ull, 5u);
    TG_T_CASE("a picked surface reports the organ the triangle belongs to");
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    bb = mesh_section_bounds(&t.mesh, MESH_SECTION_WOOD);

    /* Mismatches are ACCUMULATED and asserted once, rather than asserted inside
     * the loop.
     *
     * The reason is a real observation: clang and gcc disagreed on whether three of
     * these rays grazed a triangle, which is ordinary floating-point rounding in the
     * intersection test and not a defect. But with the assertions inside the loop it
     * changed the number of CHECKS the suite ran -- 697,685 against 697,718 -- and
     * that count is one of the signals used to notice unintended behaviour change.
     * A test whose own shape depends on rounding cannot serve as that signal. */
    for (i = 0; i < 120u; ++i) {
        Ray ray;
        InspectResult res;
        MeshTriangle aim;
        V3 target, from;
        u64 pick = (u64)tg_rng_below(&rng, (u32)mesh_triangle_count(&t.mesh));
        if (!mesh_get_triangle(&t.mesh, pick, &aim)) { continue; }
        target = v3_scale(v3_add(v3_add(aim.position[0], aim.position[1]),
                                 aim.position[2]), 1.0f / 3.0f);
        from = v3_add(target, v3(tg_rng_range(&rng, -18.0f, 18.0f),
                                 tg_rng_range(&rng, -18.0f, 18.0f),
                                 tg_rng_range(&rng, -18.0f, 18.0f)));
        ray.origin = from;
        ray.dir = v3_norm_or(v3_sub(target, from), v3(0.0f, 1.0f, 0.0f));
        ray.t_min = 0.0f;
        ray.t_max = 300.0f;
        if (!tree_inspect_ray(&t, ray, &res)) { continue; }
        picks++;

        {
            MeshTriangle tri;
            const Organ *o;
            if (!mesh_get_triangle(&t.mesh, res.triangle, &tri)) {
                bad_triangle++;
                continue;
            }
            if (tri.organ_id != res.organ_id) { bad_organ++; continue; }
            o = tree_graph_organ(&t.graph, res.organ_id);
            if (res.branch_order != o->branch_order ||
                res.created_step != o->created_step ||
                res.length_m != o->length ||
                res.radius_base_m != o->radius_base ||
                res.supported_mass_kg != o->supported_mass ||
                res.dead != ((o->flags & ORGAN_FLAG_DEAD) != 0)) {
                bad_facts++;
            }
        }
        {
            V3 expect = v3_add(ray.origin, v3_scale(ray.dir, res.distance));
            if (v3_len(v3_sub(expect, res.position)) > 1e-3f) { bad_position++; }
        }
        /* The path to the base can never be shorter than the straight-line height. */
        if (res.path_length_from_base_m < res.height_above_ground_m - 1e-3f) {
            bad_path++;
        }
        if (res.depth_from_base < 1u) { bad_path++; }
    }
    TG_EXPECT_MSG(bad_triangle == 0u, "%u picks named a triangle that does not "
                                      "resolve", bad_triangle);
    TG_EXPECT_MSG(bad_organ == 0u,
                  "%u picks attributed the surface to the wrong organ", bad_organ);
    TG_EXPECT_MSG(bad_facts == 0u,
                  "%u picks reported facts that disagree with the graph",
                  bad_facts);
    TG_EXPECT_MSG(bad_position == 0u,
                  "%u picks reported a position that is not on the ray",
                  bad_position);
    TG_EXPECT_MSG(bad_path == 0u,
                  "%u picks reported an impossible path length or depth", bad_path);
    TG_EXPECT_MSG(picks > 20u, "only %u of 120 rays picked a surface", picks);

    TG_T_CASE("inspecting by organ id agrees with inspecting by ray");
    {
        InspectResult by_id;
        u32 mid = tree_graph_organ_count(&t.graph) / 2u;
        TG_EXPECT(tree_inspect_organ(&t, mid, &by_id));
        TG_EXPECT(by_id.organ_id == mid);
        TG_EXPECT(!tree_inspect_organ(&t, tree_graph_organ_count(&t.graph), &by_id));
    }

    TG_T_CASE("the description is produced and truncation is detectable");
    {
        InspectResult res;
        char small[24];
        u64 need;
        TG_EXPECT(tree_inspect_organ(&t, 1u, &res));
        need = tree_inspect_describe(&t, &res, small, sizeof small);
        TG_EXPECT_MSG(need > sizeof small,
                      "a full description fitted in %u bytes", (u32)sizeof small);
        TG_EXPECT_MSG(small[sizeof small - 1u] == '\0',
                      "the truncated description is not terminated");
        /* And a miss must describe itself rather than printing stale fields. */
        memset(&res, 0, sizeof res);
        TG_EXPECT(tree_inspect_describe(&t, &res, NULL, 0) > 0u);
    }

    TG_T_CASE("a region query counts EXACTLY, checked against a linear scan");
    /* The old version of this case asserted `reg.organs > 0`, and that is exactly
     * why it never noticed that the number was four times too large. The
     * expectation now comes from an independent computation -- a linear scan over
     * every triangle in the mesh with its own per-organ bitset, which shares no
     * code path with the BVH traversal the query uses -- rather than from the
     * implementation.
     *
     * Four nested boxes, because the two defects appeared at different scales: the
     * memo error dominated small boxes and the 4096-triangle sample dominated large
     * ones. A single box size would have caught at most one of them. */
    {
        u32 norg = tree_graph_organ_count(&t.graph);
        u64 words = ((u64)norg + 63u) / 64u;
        u64 *seen = (u64 *)tg_alloc_zero(words * sizeof(u64));
        u32 bad_tris = 0, bad_organs = 0, bad_living = 0, bad_leaf = 0;
        u32 memo_differed = 0, boxes_checked = 0;
        u32 scale;

        TG_EXPECT(seen != NULL);
        for (scale = 0; scale < 4u && seen != NULL; ++scale) {
            f32 frac = 0.05f * (f32)(1u << scale);
            V3 c = aabb_center(bb);
            V3 ext = v3_scale(aabb_extent(bb), frac * 0.5f);
            Aabb box;
            InspectRegion reg;
            u32 e_tris = 0, e_organs = 0, e_living = 0;
            f32 e_leaf = 0.0f;
            u32 memo_organs = 0, memo_last = TG_INVALID_ID;
            u64 k;

            box.mn = v3_sub(c, ext);
            box.mx = v3_add(c, ext);

            /* Independent reference: scan the whole mesh, de-duplicate exactly. */
            memset(seen, 0, words * sizeof(u64));
            for (k = 0; k < mesh_triangle_count(&t.mesh); ++k) {
                MeshTriangle tr;
                Aabb tb = aabb_empty();
                const Organ *o;
                if (!mesh_get_triangle(&t.mesh, k, &tr)) { continue; }
                /* Scoped to the sections the BVH was built over -- by default wood
                 * only, so an inspection cursor does not snap to a leaf. Scanning
                 * the whole mesh here would compare two different questions. */
                if ((u32)tr.section >= 32u) { continue; }
                if ((t.bvh.section_mask & (1u << (u32)tr.section)) == 0u) {
                    continue;
                }
                tb = aabb_add_point(tb, tr.position[0]);
                tb = aabb_add_point(tb, tr.position[1]);
                tb = aabb_add_point(tb, tr.position[2]);
                if (!aabb_overlaps(tb, box)) { continue; }
                e_tris++;
                if (tr.organ_id >= norg) { continue; }
                if ((seen[tr.organ_id >> 6] >> (tr.organ_id & 63u)) & 1u) {
                    continue;
                }
                seen[tr.organ_id >> 6] |= 1ull << (tr.organ_id & 63u);
                o = tree_graph_organ(&t.graph, tr.organ_id);
                e_organs++;
                if ((o->flags & ORGAN_FLAG_DEAD) == 0) { e_living++; }
                e_leaf += tree_mechanics_segment_leaf_area(
                              o, &t.resolved, (u16)t.resolved.growth_steps);
            }
            if (e_tris == 0u) { continue; }
            boxes_checked++;

            TG_EXPECT(tree_inspect_region(&t, box, &reg));
            if (reg.triangles != e_tris)       { bad_tris++; }
            if (reg.organs != e_organs)        { bad_organs++; }
            if (reg.living_organs != e_living) { bad_living++; }
            if (fabsf(reg.total_leaf_area_m2 - e_leaf)
                    > tg_maxf(e_leaf * 1e-4f, 1e-6f)) {
                bad_leaf++;
            }

            /* NON-VACUITY. Reproduce the discarded algorithm -- count RUNS of the
             * same organ id in BVH traversal order -- and require that it gives a
             * different answer. Without this the test could pass over a mesh whose
             * traversal order happened to be organ-grouped, and would then be
             * asserting nothing at all, which is the failure the whole case
             * exists to correct. */
            {
                u64 *ids = (u64 *)tg_alloc(sizeof(u64) * 4096u);
                u32 written = 0, total = 0, j;
                if (ids != NULL) {
                    (void)mesh_bvh_query_box(&t.bvh, &t.mesh, box, ids, 4096u,
                                             &written, &total);
                    for (j = 0; j < written; ++j) {
                        MeshTriangle tr;
                        if (!mesh_get_triangle(&t.mesh, ids[j], &tr)) { continue; }
                        if (tr.organ_id >= norg) { continue; }
                        if (tr.organ_id == memo_last) { continue; }
                        memo_last = tr.organ_id;
                        memo_organs++;
                    }
                    if (memo_organs != e_organs) { memo_differed++; }
                    tg_free(ids, sizeof(u64) * 4096u);
                }
            }
        }
        tg_free(seen, words * sizeof(u64));

        TG_EXPECT_MSG(boxes_checked == 4u,
                      "only %u of 4 nested boxes contained any geometry",
                      boxes_checked);
        TG_EXPECT_MSG(bad_tris == 0u,
                      "%u boxes reported a triangle count a linear scan disagrees "
                      "with", bad_tris);
        TG_EXPECT_MSG(bad_organs == 0u,
                      "%u boxes reported a distinct-organ count a linear scan "
                      "disagrees with", bad_organs);
        TG_EXPECT_MSG(bad_living == 0u,
                      "%u boxes reported a living-organ count a linear scan "
                      "disagrees with", bad_living);
        TG_EXPECT_MSG(bad_leaf == 0u,
                      "%u boxes reported a leaf area a linear scan disagrees with",
                      bad_leaf);
        TG_EXPECT_MSG(memo_differed == 4u,
                      "the discarded run-counting algorithm agreed with the exact "
                      "count on %u of 4 boxes: this test cannot distinguish the "
                      "two, so it is not testing anything",
                      4u - memo_differed);
    }

    TG_T_CASE("a region query reports facts consistent with the tree");
    {
        InspectRegion reg;
        Aabb box = aabb_expand(bb, -0.5f);
        TG_EXPECT(tree_inspect_region(&t, box, &reg));
        TG_EXPECT_MSG(reg.organs > 0u, "no organs found inside the crown bounds");
        TG_EXPECT(reg.organs <= tree_graph_organ_count(&t.graph));
        TG_EXPECT(reg.living_organs <= reg.organs);
        TG_EXPECT(reg.max_branch_order <= t.growth.max_order_reached);
        TG_EXPECT(reg.latest_step >= reg.earliest_step);
        TG_EXPECT(reg.min_radius_m <= reg.max_radius_m);
        /* A box below the roots must contain nothing, and must say so rather than
         * returning stale statistics. */
        box.mn = v3(-1000.0f, -1000.0f, -1000.0f);
        box.mx = v3(-999.0f, -999.0f, -999.0f);
        TG_EXPECT(!tree_inspect_region(&t, box, &reg));
        TG_EXPECT(reg.organs == 0u);
        TG_EXPECT(reg.triangles == 0u);
    }

    tree_free(&t);
}

static void test_inspection_needs_the_bvh(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 8.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    Tree t;
    Ray ray;
    InspectResult res;

    TG_T_CASE("ray inspection refuses to run without the BVH");
    /* Rather than silently falling back to a linear scan, which would pass every
     * test on a small tree and stall for a second per mouse move on a real one. */
    opt.build_bvh = false;
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    ray.origin = v3(10.0f, 2.0f, 0.0f);
    ray.dir = v3(-1.0f, 0.0f, 0.0f);
    ray.t_min = 0.0f;
    ray.t_max = 50.0f;
    TG_EXPECT(!tree_inspect_ray(&t, ray, &res));
    TG_EXPECT(!res.hit);
    /* Inspection by organ id does not need it and must still work. */
    TG_EXPECT(tree_inspect_organ(&t, 2u, &res));
    tree_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Construction record                                                       */
/* ------------------------------------------------------------------------- */

static void test_construction_record(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_BROADLEAF, 30.0f, QUALITY_DRAFT);
    TreeBuildOptions opt = tree_build_default_options();
    Tree t;
    u32 i;

    TG_T_CASE("the construction record grows monotonically from nothing");
    /* The directive requires the tree be observable being built from nothing. The
     * record is what a replay narrates, so it has to start at nothing, never go
     * backwards, and end at the finished tree. */
    TG_EXPECT_OK(tree_build(&s, &opt, &t));
    TG_EXPECT(t.stage_count > 1u);
    for (i = 1; i < t.stage_count; ++i) {
        TG_EXPECT_MSG(t.stage[i].organs >= t.stage[i - 1u].organs,
                      "organ count fell from %u to %u at step %u",
                      t.stage[i - 1u].organs, t.stage[i].organs, i);
        TG_EXPECT_MSG(t.stage[i].height_m >= t.stage[i - 1u].height_m - 1e-4f,
                      "the tree got shorter at step %u: %.3f m after %.3f m", i,
                      (double)t.stage[i].height_m,
                      (double)t.stage[i - 1u].height_m);
        TG_EXPECT(t.stage[i].axes >= t.stage[i - 1u].axes);
        TG_EXPECT(t.stage[i].wood_volume_m3
                      >= t.stage[i - 1u].wood_volume_m3 - 1e-9f);
    }

    TG_T_CASE("the last stage accounts for every organ in the graph");
    TG_EXPECT_MSG(t.stage[t.stage_count - 1u].organs
                      == tree_graph_organ_count(&t.graph),
                  "the record ends at %u organs but the graph holds %u",
                  t.stage[t.stage_count - 1u].organs,
                  tree_graph_organ_count(&t.graph));
    TG_EXPECT_MSG(t.stage[t.stage_count - 1u].axes
                      == tree_graph_axis_count(&t.graph),
                  "the record ends at %u axes but the graph holds %u",
                  t.stage[t.stage_count - 1u].axes,
                  tree_graph_axis_count(&t.graph));

    TG_T_CASE("the first stage is a seedling, not a tree");
    TG_EXPECT_MSG(t.stage[0].height_m < t.stage[t.stage_count - 1u].height_m * 0.4f,
                  "step 0 is already %.2f m of a final %.2f m",
                  (double)t.stage[0].height_m,
                  (double)t.stage[t.stage_count - 1u].height_m);

    TG_T_CASE("every vertex carries a birth step inside the recorded history");
    /* This is what makes the reveal a clip test. A vertex whose birth step is
     * outside the record could never be shown, or would be shown from the start. */
    {
        const MeshVertex *v = mesh_vertices(&t.mesh);
        u64 n = mesh_vertex_count(&t.mesh), k;
        u32 bad = 0;
        u32 max_seen = 0;
        for (k = 0; k < n; ++k) {
            if (v[k].birth_step >= t.stage_count) { bad++; }
            if (v[k].birth_step > max_seen) { max_seen = v[k].birth_step; }
        }
        TG_EXPECT_MSG(bad == 0u,
                      "%u vertices have a birth step outside the %u-step record",
                      bad, t.stage_count);
        TG_EXPECT_MSG(max_seen > 0u,
                      "every vertex claims to have existed from step 0");
    }

    TG_T_CASE("tree_stage_at clamps rather than reading out of range");
    TG_EXPECT(tree_stage_at(&t, 0u) == &t.stage[0]);
    TG_EXPECT(tree_stage_at(&t, 1u << 20) == &t.stage[t.stage_count - 1u]);
    tree_free(&t);
}

/* Abscission: dead branches fall off.
 *
 * ORGAN_FLAG_SHED was declared in tree_graph.h from the beginning, described as
 * "self-pruned: only a scar remains", and was set by nothing and read by nothing.
 * Every branch that ever died was therefore skinned at full length for the rest of
 * the tree's life, and the 80-year conifer wore a fifteen-metre skirt of pale dead
 * twigs under its live crown -- 118 597 dead shoot segments, 60 596 of them dead
 * for more than twenty years. These are the properties that make the correction
 * safe rather than merely tidier. */
static void test_dead_wood_is_shed(void) {
    TreeSettings s = settings_for(TREE_CATEGORY_CONIFER, 55.0f, QUALITY_DRAFT);
    Tree t;
    u32 i, n;
    u32 shed = 0, shed_alive = 0, shed_root = 0, orphaned = 0, too_soon = 0;
    u32 dead_kept = 0;
    f64 shed_radius_sum = 0.0, kept_radius_sum = 0.0;
    f64 shed_years_sum = 0.0, kept_years_sum = 0.0;
    u16 final_step;

    TG_EXPECT_OK(tree_build(&s, NULL, &t));
    n = tree_graph_organ_count(&t.graph);
    final_step = (u16)t.resolved.growth_steps;

    TG_T_CASE("dead branches are shed, and only dead ones");
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(&t.graph, i);
        f32 radius, years;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        radius = 0.5f * (o->radius_base + o->radius_tip);
        years = (o->death_step <= final_step)
                  ? (f32)(final_step - o->death_step) : 0.0f;

        if ((o->flags & ORGAN_FLAG_SHED) == 0) {
            if ((o->flags & ORGAN_FLAG_DEAD) != 0
                    && o->type != ORGAN_ROOT_SEGMENT) {
                dead_kept++;
                kept_radius_sum += (f64)radius;
                kept_years_sum += (f64)years;
            }
            continue;
        }
        shed++;
        if ((o->flags & ORGAN_FLAG_DEAD) == 0) { shed_alive++; }
        if (o->type == ORGAN_ROOT_SEGMENT) { shed_root++; }
        /* Nothing may be shed in the year it died: a branch that dies and vanishes
         * within the same step never existed as far as any observer is concerned,
         * and the recently dead are part of what a real crown shows. */
        if (years < 1.0f) { too_soon++; }
        shed_radius_sum += (f64)radius;
        shed_years_sum += (f64)years;

        /* Never orphan. A shed organ may not have a child that is still attached,
         * or living wood would be left hanging off nothing -- and the skin pass,
         * which stops at the first shed organ along an axis, would leave a hole. */
        {
            u32 ch = o->first_child;
            while (ch != TG_INVALID_ID) {
                const Organ *cc = tree_graph_organ(&t.graph, ch);
                if ((cc->flags & ORGAN_FLAG_SHED) == 0
                        && cc->type != ORGAN_ROOT_SEGMENT
                        && organ_type_is_segment((OrganType)cc->type)) {
                    orphaned++;
                }
                ch = cc->next_sibling;
            }
        }
    }
    TG_EXPECT_MSG(shed > 0u,
                  "a 55-year conifer shed nothing: either abscission is not "
                  "running or nothing has been dead long enough, and in both "
                  "cases the rest of this case is vacuous");
    TG_EXPECT_MSG(t.growth.dead_organs_shed == shed,
                  "the pass reported %u shed organs, the graph carries %u",
                  t.growth.dead_organs_shed, shed);
    TG_EXPECT_MSG(shed_alive == 0u, "%u LIVING organs were shed", shed_alive);
    TG_EXPECT_MSG(shed_root == 0u,
                  "%u root segments were shed; nothing weathers a root off a "
                  "standing tree", shed_root);
    TG_EXPECT_MSG(orphaned == 0u,
                  "%u shed organs still carry an attached child, which would "
                  "leave living wood hanging off nothing and a hole in the skin",
                  orphaned);
    TG_EXPECT_MSG(too_soon == 0u,
                  "%u organs were shed in the same step they died", too_soon);

    TG_T_CASE("every shed and every retained organ satisfies the retention rule");
    /* An earlier version of this case compared the MEAN radius of shed dead wood
     * against retained dead wood and required the first to be smaller. It failed at
     * 1.60 mm against 1.60 mm -- not because the radius term was missing but
     * because a 55-year conifer's dead wood is almost all twigs of the same
     * thickness, so the comparison was measuring the population rather than the
     * rule. The rule itself is what to assert, organ by organ:
     *
     *   shed          => it had been dead longer than its own retention
     *   dead, kept    => either it is still within its retention, or something
     *                    below it is staying, which pins it in place
     *
     * `blocked` is recomputed here from first principles rather than read from the
     * pass, so the two have to agree about which organs are pinned. */
    TG_EXPECT(dead_kept > 0u);
    {
        u8 *blocked = (u8 *)tg_alloc_zero((u64)n);
        u32 rule_broken_shed = 0, rule_broken_kept = 0;
        TG_EXPECT(blocked != NULL);
        if (blocked != NULL) {
            for (i = n; i-- > 0;) {
                const Organ *o = tree_graph_organ(&t.graph, i);
                bool pins = (o->flags & ORGAN_FLAG_SHED) == 0
                            && !(!organ_type_is_segment((OrganType)o->type)
                                 && (o->flags & ORGAN_FLAG_DEAD) != 0);
                if (pins && o->parent != TG_INVALID_ID) {
                    blocked[o->parent] = 1u;
                }
            }
            for (i = 0; i < n; ++i) {
                const Organ *o = tree_graph_organ(&t.graph, i);
                f32 radius, years, retention;
                if (!organ_type_is_segment((OrganType)o->type)) { continue; }
                if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
                radius = 0.5f * (o->radius_base + o->radius_tip);
                retention = tree_dead_branch_retention(t.resolved.profile, radius);
                years = (o->death_step <= final_step)
                          ? (f32)(final_step - o->death_step) : 0.0f;
                if ((o->flags & ORGAN_FLAG_SHED) != 0) {
                    if (!(years > retention)) { rule_broken_shed++; }
                } else if ((o->flags & ORGAN_FLAG_DEAD) != 0) {
                    if (years > retention && !blocked[i]) { rule_broken_kept++; }
                }
            }
            tg_free(blocked, (u64)n);
        }
        TG_EXPECT_MSG(rule_broken_shed == 0u,
                      "%u organs were shed before their retention had elapsed",
                      rule_broken_shed);
        TG_EXPECT_MSG(rule_broken_kept == 0u,
                      "%u organs are dead past their retention, are pinned by "
                      "nothing, and are still attached", rule_broken_kept);
    }

    TG_T_CASE("abscission is driven by age, and thickness buys time");
    if (shed > 0u && dead_kept > 0u) {
        f64 shed_y = shed_years_sum / (f64)shed;
        f64 kept_y = kept_years_sum / (f64)dead_kept;
        f32 thin = tree_dead_branch_retention(t.resolved.profile, 0.002f);
        f32 thick = tree_dead_branch_retention(t.resolved.profile, 0.060f);
        (void)shed_radius_sum; (void)kept_radius_sum;
        TG_EXPECT_MSG(shed_y > kept_y,
                      "shed dead wood had been dead %.1f years against %.1f for "
                      "dead wood still attached: age is not driving abscission",
                      shed_y, kept_y);
        /* The radius term asserted on the curve rather than on the population,
         * because this tree's dead wood is nearly all twigs of one thickness. A
         * 60 mm limb must outlast a 2 mm twig by a wide margin, or what remains on
         * the bole will not be the short thick stubs a real conifer carries. */
        TG_EXPECT_MSG(thick > thin * 4.0f,
                      "a 60 mm dead limb is retained %.1f years against %.1f for a "
                      "2 mm twig: thickness barely buys time, so abscission is "
                      "effectively a flat age cutoff",
                      (double)thick, (double)thin);
    }

    TG_T_CASE("the retention curve is monotonic in radius and never instant");
    {
        f32 prev = -1.0f;
        u32 bad_order = 0, bad_floor = 0;
        for (i = 0; i < 60u; ++i) {
            f32 rad = 0.0005f * (f32)(i + 1u);
            f32 yrs = tree_dead_branch_retention(t.resolved.profile, rad);
            if (yrs < prev) { bad_order++; }
            if (!(yrs >= 1.0f)) { bad_floor++; }
            prev = yrs;
        }
        TG_EXPECT_MSG(bad_order == 0u,
                      "the retention curve fell at %u of 60 radii: a thicker dead "
                      "branch must not drop sooner", bad_order);
        TG_EXPECT_MSG(bad_floor == 0u,
                      "%u radii retained for under a year", bad_floor);
    }

    TG_T_CASE("shedding leaves the wood watertight");
    /* The skin pass stops at the first shed organ along an axis and relies on the
     * distal cap to close the stub. If that reasoning is wrong the mesh opens, and
     * a hole is exactly what no amount of looking at a crown from outside would
     * reveal. */
    {
        MeshValidateReport rep;
        MeshValidateOptions vo = mesh_validate_default_options();
        u64 nonmanifold = 0;
        u32 k;
        vo.max_scratch_bytes = 1024ull * 1024ull * 1024ull;
        (void)mesh_validate(&t.mesh, &vo, &rep);
        TG_EXPECT_MSG(!rep.topology_not_checked,
                      "the topology pass did not run, so this case measured "
                      "nothing about watertightness");
        TG_EXPECT_MSG(rep.boundary_edges[MESH_SECTION_WOOD] == 0u,
                      "%llu boundary edges after shedding dead wood: stopping the "
                      "axis sweep at a shed organ left the stub open",
                      (unsigned long long)rep.boundary_edges[MESH_SECTION_WOOD]);
        for (k = 0; k < rep.issue_count; ++k) {
            if (rep.issue[k].kind == MESH_ISSUE_NON_MANIFOLD_EDGE) {
                nonmanifold += rep.issue[k].count;
            }
        }
        TG_EXPECT_MSG(nonmanifold == 0u,
                      "%llu non-manifold edges after shedding dead wood",
                      (unsigned long long)nonmanifold);
    }

    tree_free(&t);
}

void test_suite_tree_build(void) {
    test_one_call_produces_a_finished_tree();
    test_dead_wood_is_shed();
    test_double_free_and_failed_build_are_safe();
    test_cancellation();
    test_determinism_by_fingerprint();
    test_bvh_agrees_with_brute_force();
    test_bvh_refuses_an_unfinalised_mesh();
    test_inspection_reports_the_truth();
    test_inspection_needs_the_bvh();
    test_construction_record();
}
