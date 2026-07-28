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

#include <string.h>

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

    TG_T_CASE("a region query summarises what is inside a box");
    {
        InspectRegion reg;
        Aabb box = aabb_expand(bb, -0.5f);
        TG_EXPECT(tree_inspect_region(&t, box, &reg));
        TG_EXPECT_MSG(reg.organs > 0u, "no organs found inside the crown bounds");
        TG_EXPECT(reg.max_branch_order <= t.growth.max_order_reached);
        TG_EXPECT(reg.latest_step >= reg.earliest_step);
        /* A box below the roots must contain nothing, and must say so rather than
         * returning stale statistics. */
        box.mn = v3(-1000.0f, -1000.0f, -1000.0f);
        box.mx = v3(-999.0f, -999.0f, -999.0f);
        TG_EXPECT(!tree_inspect_region(&t, box, &reg));
        TG_EXPECT(reg.organs == 0u);
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

void test_suite_tree_build(void) {
    test_one_call_produces_a_finished_tree();
    test_double_free_and_failed_build_are_safe();
    test_cancellation();
    test_determinism_by_fingerprint();
    test_bvh_agrees_with_brute_force();
    test_bvh_refuses_an_unfinalised_mesh();
    test_inspection_reports_the_truth();
    test_inspection_needs_the_bvh();
    test_construction_record();
}
