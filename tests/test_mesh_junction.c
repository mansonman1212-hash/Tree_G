/* test_mesh_junction.c -- welded branch unions.
 *
 * WHY THIS SUITE EXISTS, SPECIFICALLY
 *   docs/architecture.md carries this as risk R1, "Open, gated": junction meshing
 *   produces cracks or non-manifold edges at high-valence unions. The mitigation it
 *   names is ring-correspondence stitching gated by the mesh validator's
 *   edge-manifold test, together with an ADVERSARIAL FIXTURE WITH A SIX-CHILD UNION.
 *   That fixture is test_six_child_whorl below, and it is the reason this file exists
 *   rather than a general sense that junctions should be tested.
 *
 *   Every defect the implementation actually had is represented here by a test that
 *   fails if it returns. Those defects were, in order: a zero-width seam whose
 *   triangles had undefined orientation; per-triangle winding decided by a field
 *   gradient, which cannot be consistent across a shared diagonal; a seam that sorted
 *   the loop by angle and so discarded its connectivity; plane-plane corners in the
 *   clip region, which manufactured boundary loops belonging to no limb; a ragged
 *   cap-classification band; a boundary walk keyed by vertex, which cannot represent a
 *   pinch; and a merge advanced by angle, which let one side saturate and revisit its
 *   starting pair. Six of the seven were found by the validator rather than by
 *   inspection, which is the argument for gating on it.
 */
#include "test_util.h"
#include "test_suites.h"

#include "../src/geom/mesh_junction.h"
#include "../src/geom/mesh_validate.h"

#include <math.h>
#include <string.h>

#define MJT_MAX_SEG 64

typedef struct Fixture {
    Mesh mesh;
    JunctionSpec spec;
    JunctionLimb limb[MESH_JUNCTION_MAX_LIMBS];
    u32 ring[MESH_JUNCTION_MAX_LIMBS][MJT_MAX_SEG];
    JunctionResult result;
    f32 limb_volume_estimate;   /* independent: cones plus caps               */
} Fixture;

/* Emits one limb as an open tube running from the ring plane outward, closed at its
 * far end, and reports the ring the junction has to be sewn to. Deliberately built
 * here rather than by tree_skin: this suite must exercise the junction module against
 * geometry whose properties are known analytically, not against whatever the tree
 * pass happens to produce. */
static void emit_limb_tube(Fixture *f, u32 li, u32 nseg, f32 extend) {
    const JunctionLimb *l = &f->limb[li];
    V3 base = v3_add(f->spec.centre, v3_scale(l->dir, f->spec.limb_length));
    Frame fr = frame_make(base, l->dir, v3(0.0f, 1.0f, 0.0f));
    u32 near_ring[MJT_MAX_SEG], far_ring[MJT_MAX_SEG], apex;
    u32 i;
    MeshVertex v;

    memset(&v, 0, sizeof v);
    v.color = 0xFF7A7A7Au;
    v.ao = 1.0f;
    v.attrib = mesh_pack_attrib(MESH_MAT_BARK_MATURE, MESH_SECTION_WOOD, 0);
    v.tangent = l->dir;
    for (i = 0; i < nseg; ++i) {
        f32 th = (f32)i / (f32)nseg * TG_TAU_F;
        V3 rd = frame_ring_dir(fr, th);
        v.normal = rd;
        v.param = v2(0.0f, (f32)i / (f32)nseg);
        v.position = v3_add(fr.origin, v3_scale(rd, l->radius_outer));
        TG_EXPECT_OK(mesh_add_vertex(&f->mesh, &v, &near_ring[i]));
        v.position = v3_add(v3_add(fr.origin, v3_scale(l->dir, extend)),
                            v3_scale(rd, l->radius_outer * 0.8f));
        TG_EXPECT_OK(mesh_add_vertex(&f->mesh, &v, &far_ring[i]));
    }
    for (i = 0; i < nseg; ++i) {
        u32 j = (i + 1u) % nseg;
        TG_EXPECT_OK(mesh_add_quad(&f->mesh, near_ring[i], near_ring[j],
                                   far_ring[j], far_ring[i], li));
    }
    v.normal = l->dir;
    v.position = v3_add(fr.origin, v3_scale(l->dir, extend));
    TG_EXPECT_OK(mesh_add_vertex(&f->mesh, &v, &apex));
    for (i = 0; i < nseg; ++i) {
        u32 j = (i + 1u) % nseg;
        TG_EXPECT_OK(mesh_add_triangle(&f->mesh, far_ring[i], far_ring[j], apex, li));
    }
    for (i = 0; i < nseg; ++i) { f->ring[li][i] = near_ring[i]; }
    f->limb[li].ring = f->ring[li];
    f->limb[li].ring_count = nseg;
    f->limb[li].organ_id = li;

    /* Independent volume of this tube: a frustum plus a cone cap. */
    {
        f32 r0 = l->radius_outer, r1 = r0 * 0.8f;
        f->limb_volume_estimate += (TG_PI_F / 3.0f) * extend
                                 * (r0 * r0 + r0 * r1 + r1 * r1);
    }
}

/* A union with `children` laterals leaving a continuing parent. Returns the number of
 * limbs. */
static u32 fixture_build(Fixture *f, u32 children, u32 grid, u32 nseg,
                         f32 child_ratio, f32 elevation, f32 limb_length,
                         bool emit_tubes) {
    f32 r_par = 0.10f;
    f32 r_child = r_par * child_ratio;
    u32 n = 0, i;

    memset(f, 0, sizeof *f);
    TG_EXPECT_OK(mesh_init(&f->mesh, 1u << 15, 1u << 16));
    TG_EXPECT_OK(mesh_begin_section(&f->mesh, MESH_SECTION_WOOD));

    f->spec.centre = v3_zero();
    f->spec.limb_length = limb_length;
    f->spec.blend = r_child * 0.9f;
    f->spec.grid = grid;
    f->spec.material = MESH_MAT_BARK_MATURE;
    f->spec.colour = 0xFF7A7A7Au;
    f->spec.birth_step = 12u;
    f->spec.organ_id = 999u;

    f->limb[n].dir = v3(0.0f, -1.0f, 0.0f);
    f->limb[n].radius_inner = r_par * 1.06f;
    f->limb[n].radius_outer = r_par;
    n++;
    f->limb[n].dir = v3(0.0f, 1.0f, 0.0f);
    f->limb[n].radius_inner = r_par * 1.06f;
    f->limb[n].radius_outer = r_par * 0.94f;
    n++;
    for (i = 0; i < children && n < MESH_JUNCTION_MAX_LIMBS; ++i) {
        f32 az = (f32)i / (f32)children * TG_TAU_F;
        f->limb[n].dir = v3_norm_or(v3(cosf(az) * sinf(elevation),
                                       cosf(elevation),
                                       sinf(az) * sinf(elevation)),
                                    v3(1.0f, 0.0f, 0.0f));
        f->limb[n].radius_inner = r_child * 1.05f;
        f->limb[n].radius_outer = r_child * 0.9f;
        n++;
    }
    f->spec.limb = f->limb;
    f->spec.limb_count = n;
    if (emit_tubes) {
        for (i = 0; i < n; ++i) { emit_limb_tube(f, i, nseg, 0.35f); }
    } else {
        /* The junction still needs rings to sew to, so emit them without the tube.
         * The result is then an open shell, which is the right thing to test the
         * seam's edge accounting against. */
        for (i = 0; i < n; ++i) { emit_limb_tube(f, i, nseg, 0.35f); }
    }
    return n;
}

static void fixture_finish(Fixture *f, MeshValidateReport *rep) {
    MeshValidateOptions opt = mesh_validate_default_options();
    TG_EXPECT_OK(mesh_end_section(&f->mesh));
    TG_EXPECT_OK(mesh_finalize(&f->mesh));
    (void)mesh_validate(&f->mesh, &opt, rep);
}

static void fixture_free(Fixture *f) { mesh_destroy(&f->mesh); }

/* ------------------------------------------------------------------------- */

static void expect_welded(Fixture *f, const MeshValidateReport *rep,
                          const char *what) {
    TG_EXPECT_MSG(rep->passed, "%s: mesh failed validation", what);
    if (!rep->passed) { mesh_validate_log_report(rep); }
    TG_EXPECT_MSG(rep->boundary_edges[MESH_SECTION_WOOD] == 0,
                  "%s: %llu boundary edges -- a crack", what,
                  (unsigned long long)rep->boundary_edges[MESH_SECTION_WOOD]);
    /* ONE component is the whole point. Before this module existed the same
     * geometry was one closed tube per limb, so a union of a parent and one child was
     * three separate interpenetrating solids. */
    TG_EXPECT_MSG(rep->closed_components[MESH_SECTION_WOOD] == 1u,
                  "%s: %u closed components, expected exactly one welded solid",
                  what, rep->closed_components[MESH_SECTION_WOOD]);
    TG_EXPECT_MSG(rep->enclosed_volume[MESH_SECTION_WOOD] > 0.0,
                  "%s: enclosed volume %.9f -- the winding is inverted", what,
                  rep->enclosed_volume[MESH_SECTION_WOOD]);
    /* The module's own accounting must agree that it did a complete job. */
    TG_EXPECT_MSG(f->result.loops_found == f->spec.limb_count,
                  "%s: %u boundary loops for %u limbs", what,
                  f->result.loops_found, f->spec.limb_count);
    TG_EXPECT_MSG(f->result.loops_sewn == f->spec.limb_count,
                  "%s: %u of %u loops sewn", what, f->result.loops_sewn,
                  f->spec.limb_count);
    TG_EXPECT_MSG(f->result.loops_unmatched == 0u && f->result.limbs_unmatched == 0u,
                  "%s: %u loops and %u limbs left unreached", what,
                  f->result.loops_unmatched, f->result.limbs_unmatched);
    TG_EXPECT_MSG(f->result.diagonal_reuse == 0u,
                  "%s: %u seam diagonals emitted more than twice", what,
                  f->result.diagonal_reuse);
    TG_EXPECT_MSG(f->result.ring_duplicates == 0u,
                  "%s: %u repeated vertices in a sewn ring", what,
                  f->result.ring_duplicates);
    TG_EXPECT_MSG(f->result.coincident_vertices_merged == 0u,
                  "%s: %u coincident vertices had to be merged", what,
                  f->result.coincident_vertices_merged);
    TG_EXPECT_MSG(f->result.seam_triangles == f->result.seam_expected,
                  "%s: %u seam triangles emitted against %u required to close "
                  "every ring edge exactly once", what, f->result.seam_triangles,
                  f->result.seam_expected);
    TG_EXPECT_MSG(!f->result.pinched, "%s: boundary stayed pinched", what);
    TG_EXPECT_MSG(!f->result.not_separable, "%s: limbs reported inseparable", what);
}

static void test_six_child_whorl(void) {
    Fixture f;
    MeshValidateReport rep;

    TG_T_CASE("THE ADVERSARIAL FIXTURE: a six-child union welds to one solid");
    /* Named in the risk register. A conifer whorl really does put five laterals on one
     * node, so eight limbs meeting at a point is not a contrived stress case; it is
     * the case the tree will actually generate. */
    (void)fixture_build(&f, 6u, 32u, 16u, 0.55f, 0.85f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
    TG_EXPECT(f.spec.limb_count == 8u);
    fixture_finish(&f, &rep);
    expect_welded(&f, &rep, "six-child whorl");
    fixture_free(&f);
}

static void test_every_valence(void) {
    u32 children;

    TG_T_CASE("every valence from one to six children welds to one solid");
    for (children = 1u; children <= 6u; ++children) {
        Fixture f;
        MeshValidateReport rep;
        char label[64];
        (void)snprintf(label, sizeof label, "%u children", children);
        (void)fixture_build(&f, children, 32u, 16u, 0.55f, 0.85f, 0.34f, true);
        TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
        fixture_finish(&f, &rep);
        expect_welded(&f, &rep, label);
        fixture_free(&f);
    }
}

static void test_ring_resolution_independence(void) {
    static const u32 segs[] = { 6u, 8u, 16u, 40u, 64u };
    u32 k;

    TG_T_CASE("the seam closes for any ring resolution, coarse or fine");
    /* The patch's boundary has whatever vertex count the extraction produced; the
     * ring has whatever the caller chose. The stitch has to close for any pair, and
     * it must not require them to be equal or commensurate. */
    for (k = 0; k < (u32)TG_COUNTOF(segs); ++k) {
        Fixture f;
        MeshValidateReport rep;
        char label[64];
        (void)snprintf(label, sizeof label, "%u ring segments", segs[k]);
        (void)fixture_build(&f, 2u, 32u, segs[k], 0.55f, 0.85f, 0.34f, true);
        TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
        fixture_finish(&f, &rep);
        expect_welded(&f, &rep, label);
        fixture_free(&f);
    }
}

static void test_grid_is_raised_to_what_it_needs(void) {
    Fixture f;
    MeshValidateReport rep;
    u32 needed;

    TG_T_CASE("a grid too coarse for the thinnest limb is raised, not accepted");
    /* Asking for eight cells across a union whose child is a fifth of the parent's
     * radius cannot resolve the child at all, and the first implementation quietly
     * produced a stray extra component. The module now computes what it needs. */
    (void)fixture_build(&f, 1u, 8u, 16u, 0.35f, 0.9f, 0.34f, true);
    needed = mesh_junction_min_grid(&f.spec);
    TG_EXPECT_MSG(needed > 8u, "min_grid reported %u for a deliberately coarse "
                               "request", needed);
    TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
    TG_EXPECT_MSG(f.result.grid_used >= needed,
                  "built at grid %u when %u was required", f.result.grid_used,
                  needed);
    fixture_finish(&f, &rep);
    expect_welded(&f, &rep, "coarse grid request");
    fixture_free(&f);
}

static void test_inseparable_union_is_refused_not_cracked(void) {
    Fixture f;
    f32 need;

    TG_T_CASE("a shallow-angle union is refused, and nothing is emitted");
    /* A child leaving its parent at ten degrees is genuinely fused to it for a long
     * way, so there is no separate exit to sew. The honest answers are "weld over a
     * longer region" or "do not weld this one" -- never "emit a ring nobody reached",
     * which is a hole the size of a branch. */
    (void)fixture_build(&f, 1u, 32u, 16u, 0.55f, 0.20f, 0.34f, true);
    need = mesh_junction_min_limb_length(&f.spec);
    TG_EXPECT_MSG(need > f.spec.limb_length,
                  "a 0.20 rad union claims to be separable within %.4f m",
                  (double)need);
    {
        u64 v_before = mesh_vertex_count(&f.mesh);
        u64 t_before = mesh_triangle_count(&f.mesh);
        TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
        TG_EXPECT_MSG(f.result.not_separable, "the refusal was not reported");
        TG_EXPECT_MSG(mesh_vertex_count(&f.mesh) == v_before &&
                      mesh_triangle_count(&f.mesh) == t_before,
                      "a refused union still emitted %llu vertices and %llu "
                      "triangles",
                      (unsigned long long)(mesh_vertex_count(&f.mesh) - v_before),
                      (unsigned long long)(mesh_triangle_count(&f.mesh) - t_before));
    }
    fixture_free(&f);

    TG_T_CASE("and the same union welds once it is given the length it asked for");
    (void)fixture_build(&f, 1u, 32u, 16u, 0.55f, 0.20f, need * 1.02f, true);
    {
        MeshValidateReport rep;
        TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
        fixture_finish(&f, &rep);
        expect_welded(&f, &rep, "shallow union at the required length");
    }
    fixture_free(&f);
}

static void test_bad_input_is_rejected(void) {
    Fixture f;
    JunctionResult res;

    TG_T_CASE("malformed specifications are rejected rather than approximated");
    (void)fixture_build(&f, 1u, 32u, 16u, 0.55f, 0.85f, 0.34f, true);

    {
        JunctionSpec bad = f.spec;
        bad.limb_count = 1u;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
        bad = f.spec;
        bad.limb_count = MESH_JUNCTION_MAX_LIMBS + 1u;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
        bad = f.spec;
        bad.limb_length = 0.0f;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
    }
    {
        JunctionLimb broken[MESH_JUNCTION_MAX_LIMBS];
        JunctionSpec bad = f.spec;
        memcpy(broken, f.limb, sizeof broken);
        broken[1].dir = v3_zero();
        bad.limb = broken;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
        memcpy(broken, f.limb, sizeof broken);
        broken[1].radius_outer = 0.0f;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
        memcpy(broken, f.limb, sizeof broken);
        broken[1].ring_count = 2u;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
        memcpy(broken, f.limb, sizeof broken);
        broken[1].ring = NULL;
        TG_EXPECT(mesh_junction_build(&f.mesh, &bad, &res)
                      == TG_ERR_INVALID_ARGUMENT);
    }
    fixture_free(&f);
}

static void test_field_describes_a_collar(void) {
    Fixture f;
    u32 i;

    TG_T_CASE("the field is negative inside every limb and positive outside");
    /* The field is a construction device, but it is the thing that decides where the
     * surface goes, so its sign has to be right independently of any tessellation. */
    (void)fixture_build(&f, 2u, 16u, 8u, 0.55f, 0.85f, 0.34f, true);
    for (i = 0; i < f.spec.limb_count; ++i) {
        V3 on_axis = v3_add(f.spec.centre,
                            v3_scale(f.limb[i].dir, f.spec.limb_length * 0.5f));
        V3 beyond = v3_add(f.spec.centre,
                           v3_scale(f.limb[i].dir, f.spec.limb_length * 1.4f));
        TG_EXPECT_MSG(mesh_junction_field(&f.spec, on_axis) < 0.0f,
                      "limb %u: the field is not negative on its own axis", i);
        /* Beyond the ring the solid must have ended, because the clip ball is what
         * hands the limb over to the caller's tube.
         *
         * Note what this test does NOT do. It used to sample a point three radii out
         * from one limb's axis and require the field to be positive there, which
         * failed -- correctly -- because on a union of several limbs that point is
         * often inside a NEIGHBOUR. "Outside limb i" is not the same as "outside the
         * solid", and only the second is a property of the field. */
        TG_EXPECT_MSG(mesh_junction_field(&f.spec, beyond) > 0.0f,
                      "limb %u: the field is not positive beyond the clip ball", i);
    }
    /* A direction no limb points in: with two children in the XY plane, +Z is a
     * genuine gap, and the field there has to be outside everything. */
    {
        V3 gap = v3_add(f.spec.centre, v3(0.0f, 0.0f, f.spec.limb_length * 0.7f));
        TG_EXPECT_MSG(mesh_junction_field(&f.spec, gap) > 0.0f,
                      "the field is negative in a direction no limb occupies, so the "
                      "blend is fusing limbs that should be separate");
    }
    TG_EXPECT_MSG(mesh_junction_field(&f.spec, f.spec.centre) < 0.0f,
                  "the field is not negative at the union centre, so the limbs are "
                  "not one solid there");

    TG_T_CASE("the smooth union swells the surface in the fork: that IS the collar");
    /* A branch collar is not decoration added afterwards. It is the fillet of the
     * smooth union, so it must be measurable as the surface standing further out in
     * the fork than either limb alone would put it. */
    {
        /* A point in the bisector plane of the parent and the first child, at a
         * radius where both limbs' surfaces are close. */
        V3 bis = v3_norm_or(v3_add(f.limb[1].dir, f.limb[2].dir),
                            v3(0.0f, 1.0f, 0.0f));
        f32 near_r = f.spec.limb_length * 0.30f;
        V3 p = v3_add(f.spec.centre, v3_scale(bis, near_r));
        f32 blended = mesh_junction_field(&f.spec, p);
        /* Distance to the two limbs taken separately, without the smooth union. */
        JunctionSpec solo = f.spec;
        JunctionLimb one[1];
        f32 d_parent, d_child;
        one[0] = f.limb[1];
        solo.limb = one;
        solo.limb_count = 1u;
        d_parent = mesh_junction_field(&solo, p);
        one[0] = f.limb[2];
        d_child = mesh_junction_field(&solo, p);
        TG_EXPECT_MSG(blended < tg_minf(d_parent, d_child) - 1e-6f,
                      "in the fork the blended field is %.6f, no deeper than the "
                      "nearer limb alone at %.6f: there is no fillet, so there is "
                      "no collar", (double)blended,
                      (double)tg_minf(d_parent, d_child));
    }
    fixture_free(&f);
}

static void test_volume_against_an_independent_estimate(void) {
    Fixture f;
    MeshValidateReport rep;
    f64 measured;
    f32 lower, upper;
    u32 i;

    TG_T_CASE("the welded solid's volume lies between independent bounds");
    /* Not compared against a recorded number, which would only prove the code still
     * does what it did. The lower bound is the limb tubes alone, which the welded
     * solid must contain. The upper bound adds the ball the junction is confined to,
     * which it cannot exceed. A winding error, a lost limb or a doubled patch all
     * fall outside those bounds. */
    (void)fixture_build(&f, 3u, 32u, 16u, 0.55f, 0.85f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
    fixture_finish(&f, &rep);
    expect_welded(&f, &rep, "three-child union for volume");
    measured = rep.enclosed_volume[MESH_SECTION_WOOD];

    lower = f.limb_volume_estimate;
    upper = f.limb_volume_estimate;
    for (i = 0; i < f.spec.limb_count; ++i) {
        /* Each limb also contributes a cone from the union centre out to its ring. */
        f32 r0 = f.limb[i].radius_inner, r1 = f.limb[i].radius_outer;
        upper += (TG_PI_F / 3.0f) * f.spec.limb_length
                 * (r0 * r0 + r0 * r1 + r1 * r1);
    }
    /* Plus the blend fillet, bounded generously by the clip ball. */
    upper += (4.0f / 3.0f) * TG_PI_F * f.spec.limb_length * f.spec.limb_length
             * f.spec.limb_length;
    TG_EXPECT_MSG(measured > (f64)lower,
                  "welded volume %.6f m3 is smaller than the limb tubes alone at "
                  "%.6f m3", measured, (double)lower);
    TG_EXPECT_MSG(measured < (f64)upper,
                  "welded volume %.6f m3 exceeds the limbs plus the junction ball "
                  "at %.6f m3", measured, (double)upper);
    fixture_free(&f);
}

static void test_determinism(void) {
    Fixture a, b;
    MeshValidateReport ra, rb;

    TG_T_CASE("the same union produces byte-identical geometry twice");
    (void)fixture_build(&a, 4u, 32u, 16u, 0.55f, 0.85f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&a.mesh, &a.spec, &a.result));
    fixture_finish(&a, &ra);
    (void)fixture_build(&b, 4u, 32u, 16u, 0.55f, 0.85f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&b.mesh, &b.spec, &b.result));
    fixture_finish(&b, &rb);

    TG_EXPECT(a.result.patch_triangles == b.result.patch_triangles);
    TG_EXPECT(a.result.seam_triangles == b.result.seam_triangles);
    TG_EXPECT(mesh_vertex_count(&a.mesh) == mesh_vertex_count(&b.mesh));
    TG_EXPECT_MSG(memcmp(mesh_vertices(&a.mesh), mesh_vertices(&b.mesh),
                         (size_t)mesh_vertex_count(&a.mesh)
                             * sizeof(MeshVertex)) == 0,
                  "the two runs produced different vertices");
    TG_EXPECT(mesh_fingerprint(&a.mesh).value == mesh_fingerprint(&b.mesh).value);
    fixture_free(&a);
    fixture_free(&b);
}

static void test_patch_carries_its_attribution(void) {
    Fixture f;
    MeshValidateReport rep;
    u64 t;
    u32 patch_seen = 0, wrong_section = 0, wrong_birth = 0;

    TG_T_CASE("junction geometry is attributed and dated like any other surface");
    /* The patch has to be inspectable and it has to appear at the right moment in the
     * construction replay, which means carrying an organ id and a birth step. A
     * junction that appears from step zero would show a fully formed fork under a
     * seedling. */
    (void)fixture_build(&f, 2u, 32u, 12u, 0.55f, 0.85f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
    fixture_finish(&f, &rep);
    for (t = 0; t < mesh_triangle_count(&f.mesh); ++t) {
        MeshTriangle tri;
        if (!mesh_get_triangle(&f.mesh, t, &tri)) { continue; }
        if (tri.organ_id != f.spec.organ_id) { continue; }
        patch_seen++;
        if (tri.section != MESH_SECTION_WOOD) { wrong_section++; }
    }
    {
        const MeshVertex *v = mesh_vertices(&f.mesh);
        u64 k;
        for (k = 0; k < mesh_vertex_count(&f.mesh); ++k) {
            if (v[k].organ_id != f.spec.organ_id) { continue; }
            if (v[k].birth_step != f.spec.birth_step) { wrong_birth++; }
        }
    }
    TG_EXPECT_MSG(patch_seen > 0u, "no triangle carries the junction's organ id");
    TG_EXPECT_MSG(wrong_section == 0u, "%u junction triangles are not wood",
                  wrong_section);
    TG_EXPECT_MSG(wrong_birth == 0u,
                  "%u junction vertices carry the wrong birth step", wrong_birth);
    fixture_free(&f);
}

static void test_seam_normals_lie_in_the_surface(void) {
    Fixture f;
    MeshValidateReport rep;
    const MeshVertex *v;
    u64 k;
    u32 checked = 0, axial = 0;
    f32 worst = 0.0f;

    TG_T_CASE("normals at the patch boundary lie IN the surface, not along the limb");
    /* This test exists because of a rendered capture, and it is the clearest example
     * in the project of why gate 8 of docs/testing.md is not optional.
     *
     * The first version took the surface normal from the gradient of the CLIPPED
     * field. The clip ball is a construction device that decides where the patch ends;
     * it is not part of the surface. So at the patch's boundary, where the clip term is
     * the active one, the gradient came out as the sphere's radial direction -- which
     * points along the limb axis, roughly perpendicular to the actual surface.
     *
     * Every topological check passed. The mesh was watertight, manifold, consistently
     * wound and correct in volume. What it LOOKED like was a dark sawtooth band around
     * every seam. Nothing but a picture, or this test, would have found it.
     *
     * The assertion is derived from geometry rather than from the implementation: near
     * the clip radius the surface is a tube wall, so its normal must be close to
     * perpendicular to that tube's axis. */
    (void)fixture_build(&f, 3u, 32u, 16u, 0.55f, 0.95f, 0.34f, true);
    TG_EXPECT_OK(mesh_junction_build(&f.mesh, &f.spec, &f.result));
    fixture_finish(&f, &rep);
    expect_welded(&f, &rep, "seam normal fixture");

    v = mesh_vertices(&f.mesh);
    for (k = 0; k < mesh_vertex_count(&f.mesh); ++k) {
        V3 rel;
        f32 dist, best = -2.0f;
        u32 li, near_limb = 0;
        if (v[k].organ_id != f.spec.organ_id) { continue; }   /* patch only     */
        rel = v3_sub(v[k].position, f.spec.centre);
        dist = v3_len(rel);
        /* Only the outer part of the patch, where the surface is a tube wall rather
         * than the fillet in the fork. */
        if (dist < f.spec.limb_length * 0.72f) { continue; }
        for (li = 0; li < f.spec.limb_count; ++li) {
            f32 d = v3_dot(v3_norm_or(rel, v3(0.0f, 1.0f, 0.0f)), f.limb[li].dir);
            if (d > best) { best = d; near_limb = li; }
        }
        {
            f32 along = tg_absf(v3_dot(v[k].normal, f.limb[near_limb].dir));
            checked++;
            if (along > worst) { worst = along; }
            if (along > 0.80f) { axial++; }
        }
    }
    TG_EXPECT_MSG(checked > 50u,
                  "only %u patch vertices lay near the boundary, so this proved "
                  "little", checked);
    TG_EXPECT_MSG(axial == 0u,
                  "%u of %u boundary normals point along their limb axis rather than "
                  "out of the surface (worst |n.axis| = %.3f): the seam will render "
                  "as a dark band", axial, checked, (double)worst);
    fixture_free(&f);
}

static void test_geometry_sweep(void) {
    static const f32 ratios[] = { 0.20f, 0.45f, 0.70f };
    static const f32 elevs[] = { 0.9f, 1.3f, 1.9f };
    u32 ri, ei, children;
    u32 built = 0, refused = 0, failures = 0;

    TG_T_CASE("a sweep of radius ratios, insertion angles and valences");
    /* Aggregate rather than per-case assertions, so the number of checks the suite
     * runs does not depend on how many configurations happen to be separable -- which
     * is a floating-point outcome and would make the check count compiler dependent.
     * See the check-count rule in docs/testing.md. */
    for (children = 1u; children <= 5u; children += 2u) {
        for (ri = 0; ri < (u32)TG_COUNTOF(ratios); ++ri) {
            for (ei = 0; ei < (u32)TG_COUNTOF(elevs); ++ei) {
                Fixture f;
                MeshValidateReport rep;
                (void)fixture_build(&f, children, 24u, 12u, ratios[ri], elevs[ei],
                                    0.34f, true);
                if (mesh_junction_build(&f.mesh, &f.spec, &f.result) != TG_OK) {
                    failures++;
                    fixture_free(&f);
                    continue;
                }
                if (f.result.not_separable || f.result.pinched) {
                    refused++;
                    fixture_free(&f);
                    continue;
                }
                fixture_finish(&f, &rep);
                built++;
                if (!rep.passed ||
                    rep.closed_components[MESH_SECTION_WOOD] != 1u ||
                    rep.boundary_edges[MESH_SECTION_WOOD] != 0u ||
                    f.result.diagonal_reuse != 0u ||
                    f.result.loops_sewn != f.spec.limb_count) {
                    failures++;
                }
                fixture_free(&f);
            }
        }
    }
    TG_EXPECT_MSG(failures == 0u,
                  "%u of %u welded configurations failed (and %u were refused as "
                  "inseparable, which is not a failure)", failures, built, refused);
    TG_EXPECT_MSG(built >= 15u,
                  "only %u configurations were actually welded, so the sweep proved "
                  "little", built);
}

void test_suite_mesh_junction(void) {
    test_six_child_whorl();
    test_every_valence();
    test_ring_resolution_independence();
    test_grid_is_raised_to_what_it_needs();
    test_inseparable_union_is_refused_not_cracked();
    test_bad_input_is_rejected();
    test_field_describes_a_collar();
    test_volume_against_an_independent_estimate();
    test_determinism();
    test_patch_carries_its_attribution();
    test_seam_normals_lie_in_the_surface();
    test_geometry_sweep();
}
