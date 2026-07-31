#include "test_util.h"
#include "test_suites.h"

#include "../src/geom/mesh_validate.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Builders                                                                  */
/*                                                                           */
/* These construct reference solids whose exact volume is known analytically,  */
/* so the validator's enclosed-volume result can be checked against an         */
/* independent value rather than against itself.                              */
/* ------------------------------------------------------------------------- */

static MeshVertex make_vertex(V3 p, V3 n, f32 u, f32 v, MeshMaterial mat,
                              MeshSection sec) {
    MeshVertex out;
    memset(&out, 0, sizeof out);
    out.position = p;
    out.normal = n;
    out.tangent = v3_any_perpendicular(n);
    out.param = v2(u, v);
    out.color = mesh_pack_rgba(0.5f, 0.4f, 0.3f, 1.0f);
    out.organ_id = 0;
    out.attrib = mesh_pack_attrib(mat, sec, 0);
    out.ao = 1.0f;
    return out;
}

/* Closed tapered tube (a trunk segment): two rings plus two caps. */
typedef struct TubeOptions {
    u32  segments;      /* ring resolution                                  */
    u32  rings;         /* number of cross-sections along the axis           */
    f32  r0, r1;        /* basal and distal radius                           */
    f32  height;
    bool omit_top_cap;  /* injects a hole                                    */
    bool flip_one_quad; /* injects an inconsistent winding                   */
    bool duplicate_one; /* injects a duplicate triangle                      */
    bool invert_all;    /* injects a fully inside-out component              */
    /* Duplicates the rim vertices so the cap has its own copies. This is what
     * real generated geometry must do at any hard edge, and it is the case the
     * validator's position-welding exists to handle. */
    bool split_rim;
    MeshSection section;
} TubeOptions;

static TgResult build_tube(Mesh *m, const TubeOptions *o, u32 organ_id) {
    u32 *rings;
    u32 s, k;
    TgResult r;
    u32 center_bottom = 0, center_top = 0;
    u64 ring_bytes;

    TG_CHECK(o->segments >= 3 && o->rings >= 2);
    ring_bytes = (u64)o->segments * (u64)o->rings * sizeof(u32);
    rings = (u32 *)tg_alloc(ring_bytes);
    if (rings == NULL) { return TG_ERR_OUT_OF_MEMORY; }

    r = mesh_begin_section(m, o->section);
    if (r != TG_OK) { goto done; }

    for (k = 0; k < o->rings; ++k) {
        f32 t = (f32)k / (f32)(o->rings - 1u);
        f32 y = t * o->height;
        f32 rad = tg_lerpf(o->r0, o->r1, t);
        Frame f = frame_make(v3(0.0f, y, 0.0f), v3(0, 1, 0), v3(1, 0, 0));
        for (s = 0; s < o->segments; ++s) {
            f32 theta = (f32)s / (f32)o->segments * TG_TAU_F;
            V3 p = frame_ring_point(f, theta, rad);
            V3 n = frame_ring_dir(f, theta);
            MeshVertex v = make_vertex(p, n, t, theta / TG_TAU_F,
                                       MESH_MAT_BARK_MATURE, o->section);
            u32 idx;
            r = mesh_add_vertex(m, &v, &idx);
            if (r != TG_OK) { goto done; }
            rings[k * o->segments + s] = idx;
        }
    }

    /* Cap centres. */
    {
        MeshVertex vb = make_vertex(v3(0, 0, 0), v3(0, -1, 0), 0.0f, 0.0f,
                                    MESH_MAT_SAPWOOD, o->section);
        MeshVertex vt = make_vertex(v3(0, o->height, 0), v3(0, 1, 0), 1.0f, 0.0f,
                                    MESH_MAT_SAPWOOD, o->section);
        r = mesh_add_vertex(m, &vb, &center_bottom);
        if (r != TG_OK) { goto done; }
        r = mesh_add_vertex(m, &vt, &center_top);
        if (r != TG_OK) { goto done; }
    }

    for (k = 0; k + 1 < o->rings; ++k) {
        const u32 *a = &rings[k * o->segments];
        const u32 *b = &rings[(k + 1u) * o->segments];
        if (o->invert_all) {
            /* Reverse the stitch order for the whole wall. */
            for (s = 0; s < o->segments; ++s) {
                u32 j = (s + 1u) % o->segments;
                r = mesh_add_quad(m, a[s], b[s], b[j], a[j], organ_id);
                if (r != TG_OK) { goto done; }
            }
        } else {
            r = mesh_stitch_rings(m, a, b, o->segments, organ_id);
            if (r != TG_OK) { goto done; }
        }
    }

    /* Bottom cap: ring wound CCW about +Y, so the outward-facing bottom cap
     * needs the reversed winding. */
    if (o->split_rim) {
        /* Duplicate the rim into fresh vertices carrying the cap's own axial
         * normal. Positions are copied bit-exactly, which is what allows the
         * validator to weld them and still see a closed surface. */
        u32 *dup = (u32 *)tg_alloc((u64)o->segments * sizeof(u32));
        if (dup == NULL) { r = TG_ERR_OUT_OF_MEMORY; goto done; }
        for (s = 0; s < o->segments; ++s) {
            MeshVertex v = mesh_vertices(m)[rings[s]];
            v.normal = v3(0, -1, 0);
            v.attrib = mesh_pack_attrib(MESH_MAT_SAPWOOD, o->section,
                                        MESH_VFLAG_SEAM);
            r = mesh_add_vertex(m, &v, &dup[s]);
            if (r != TG_OK) { tg_free(dup, (u64)o->segments * sizeof(u32)); goto done; }
        }
        r = mesh_cap_ring(m, dup, o->segments, center_bottom, false, organ_id);
        tg_free(dup, (u64)o->segments * sizeof(u32));
        if (r != TG_OK) { goto done; }
    } else {
        r = mesh_cap_ring(m, &rings[0], o->segments, center_bottom,
                          o->invert_all ? true : false, organ_id);
        if (r != TG_OK) { goto done; }
    }

    if (!o->omit_top_cap) {
        if (o->split_rim) {
            u32 *dup = (u32 *)tg_alloc((u64)o->segments * sizeof(u32));
            const u32 *top = &rings[(o->rings - 1u) * o->segments];
            if (dup == NULL) { r = TG_ERR_OUT_OF_MEMORY; goto done; }
            for (s = 0; s < o->segments; ++s) {
                MeshVertex v = mesh_vertices(m)[top[s]];
                v.normal = v3(0, 1, 0);
                v.attrib = mesh_pack_attrib(MESH_MAT_SAPWOOD, o->section,
                                            MESH_VFLAG_SEAM);
                r = mesh_add_vertex(m, &v, &dup[s]);
                if (r != TG_OK) {
                    tg_free(dup, (u64)o->segments * sizeof(u32));
                    goto done;
                }
            }
            r = mesh_cap_ring(m, dup, o->segments, center_top, true, organ_id);
            tg_free(dup, (u64)o->segments * sizeof(u32));
            if (r != TG_OK) { goto done; }
        } else {
            r = mesh_cap_ring(m, &rings[(o->rings - 1u) * o->segments], o->segments,
                              center_top, o->invert_all ? false : true, organ_id);
            if (r != TG_OK) { goto done; }
        }
    }

    if (o->flip_one_quad) {
        /* Add a reversed duplicate of one wall quad's two triangles. This
         * creates edges used by three triangles AND a same-direction pair. */
        const u32 *a = &rings[0];
        const u32 *b = &rings[o->segments];
        r = mesh_add_triangle(m, a[0], b[1], a[1], organ_id);
        if (r != TG_OK) { goto done; }
    }

    if (o->duplicate_one) {
        const u32 *a = &rings[0];
        const u32 *b = &rings[o->segments];
        r = mesh_add_triangle(m, a[0], a[1], b[1], organ_id);
        if (r != TG_OK) { goto done; }
    }

    r = mesh_end_section(m);

done:
    tg_free(rings, ring_bytes);
    return r;
}

/* UV sphere. Volume 4/3 pi r^3 in the limit; the polyhedral approximation is
 * slightly smaller, which the test accounts for. */
static TgResult build_sphere(Mesh *m, MeshSection sec, V3 center, f32 radius,
                             u32 slices, u32 stacks, u32 organ_id,
                             bool inverted) {
    u32 *grid;
    u64 grid_bytes;
    u32 i, j;
    TgResult r;
    u32 north = 0, south = 0;

    TG_CHECK(slices >= 3 && stacks >= 2);
    grid_bytes = (u64)slices * (u64)(stacks - 1u) * sizeof(u32);
    grid = (u32 *)tg_alloc(grid_bytes);
    if (grid == NULL) { return TG_ERR_OUT_OF_MEMORY; }

    r = mesh_begin_section(m, sec);
    if (r != TG_OK) { goto done; }

    for (j = 1; j < stacks; ++j) {
        f32 phi = (f32)j / (f32)stacks * TG_PI_F; /* 0 at north pole */
        for (i = 0; i < slices; ++i) {
            f32 theta = (f32)i / (f32)slices * TG_TAU_F;
            V3 n = v3(sinf(phi) * cosf(theta), cosf(phi), sinf(phi) * sinf(theta));
            V3 p = v3_add(center, v3_scale(n, radius));
            MeshVertex v = make_vertex(p, n, (f32)j / (f32)stacks,
                                       (f32)i / (f32)slices, MESH_MAT_LEAF_UPPER,
                                       sec);
            u32 idx;
            r = mesh_add_vertex(m, &v, &idx);
            if (r != TG_OK) { goto done; }
            grid[(j - 1u) * slices + i] = idx;
        }
    }
    {
        MeshVertex vn = make_vertex(v3_add(center, v3(0, radius, 0)), v3(0, 1, 0),
                                    0.0f, 0.0f, MESH_MAT_LEAF_UPPER, sec);
        MeshVertex vs = make_vertex(v3_add(center, v3(0, -radius, 0)), v3(0, -1, 0),
                                    1.0f, 0.0f, MESH_MAT_LEAF_UPPER, sec);
        r = mesh_add_vertex(m, &vn, &north);
        if (r != TG_OK) { goto done; }
        r = mesh_add_vertex(m, &vs, &south);
        if (r != TG_OK) { goto done; }
    }

    /* North fan. Worked out by evaluating the face normal directly rather than
     * by guessing: for phi=45deg, theta=0 and 90deg, the triangle
     * (north, ring[i], ring[i+1]) has normal pointing INWARD, so the outward
     * order is (north, ring[i+1], ring[i]). The south fan is the mirror image
     * and therefore uses (south, ring[i], ring[i+1]). */
    for (i = 0; i < slices; ++i) {
        u32 a = grid[i];
        u32 b = grid[(i + 1u) % slices];
        r = inverted ? mesh_add_triangle(m, north, a, b, organ_id)
                     : mesh_add_triangle(m, north, b, a, organ_id);
        if (r != TG_OK) { goto done; }
    }
    for (j = 1; j + 1 < stacks; ++j) {
        for (i = 0; i < slices; ++i) {
            u32 i1 = (i + 1u) % slices;
            u32 a = grid[(j - 1u) * slices + i];
            u32 b = grid[(j - 1u) * slices + i1];
            u32 c = grid[j * slices + i1];
            u32 d = grid[j * slices + i];
            /* Outward winding for the band, derived independently (see the
             * comment on the fan below): with `a,b` on the upper ring and
             * `c,d` on the lower ring, (a,b,c) has an outward normal. */
            r = inverted ? mesh_add_quad(m, a, d, c, b, organ_id)
                         : mesh_add_quad(m, a, b, c, d, organ_id);
            if (r != TG_OK) { goto done; }
        }
    }
    for (i = 0; i < slices; ++i) {
        u32 a = grid[(stacks - 2u) * slices + i];
        u32 b = grid[(stacks - 2u) * slices + (i + 1u) % slices];
        r = inverted ? mesh_add_triangle(m, south, b, a, organ_id)
                     : mesh_add_triangle(m, south, a, b, organ_id);
        if (r != TG_OK) { goto done; }
    }

    r = mesh_end_section(m);

done:
    tg_free(grid, grid_bytes);
    return r;
}

static TubeOptions default_tube(void) {
    TubeOptions o;
    memset(&o, 0, sizeof o);
    o.segments = 24;
    o.rings = 6;
    o.r0 = 0.5f;
    o.r1 = 0.35f;
    o.height = 4.0f;
    o.section = MESH_SECTION_WOOD;
    return o;
}

static u64 issue_count_of(const MeshValidateReport *r, MeshIssueKind k) {
    u32 i;
    u64 total = 0;
    for (i = 0; i < r->issue_count; ++i) {
        if (r->issue[i].kind == k) { total += r->issue[i].count; }
    }
    return total;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_vertex_layout(void) {
    TG_T_CASE("vertex packing helpers round-trip");
    {
        u32 c = mesh_pack_rgba(0.0f, 0.25098f, 0.50196f, 1.0f);
        f32 r, g, b, a;
        mesh_unpack_rgba(c, &r, &g, &b, &a);
        TG_EXPECT_NEAR(r, 0.0f, 0.004);
        TG_EXPECT_NEAR(g, 0.25098f, 0.004);
        TG_EXPECT_NEAR(b, 0.50196f, 0.004);
        TG_EXPECT_NEAR(a, 1.0f, 0.004);
        /* Out-of-range input must saturate, not wrap into a wrong channel. */
        c = mesh_pack_rgba(2.0f, -1.0f, 0.0f, 0.0f);
        mesh_unpack_rgba(c, &r, &g, &b, &a);
        TG_EXPECT_NEAR(r, 1.0f, 0.004);
        TG_EXPECT_NEAR(g, 0.0f, 0.004);
    }

    TG_T_CASE("attrib packing keeps material, section and flags separate");
    {
        u32 a = mesh_pack_attrib(MESH_MAT_WOUNDWOOD, MESH_SECTION_LEAF, 0xBEEF);
        TG_EXPECT_EQ_U64(mesh_attrib_material(a), MESH_MAT_WOUNDWOOD);
        TG_EXPECT_EQ_U64(mesh_attrib_section(a), MESH_SECTION_LEAF);
        TG_EXPECT_EQ_U64(mesh_attrib_flags(a), 0xBEEF);
    }

    TG_T_CASE("every section and material has metadata");
    {
        u32 i;
        for (i = 0; i < MESH_SECTION_COUNT; ++i) {
            const MeshSectionInfo *info = mesh_section_info((MeshSection)i);
            TG_EXPECT(info != NULL && info->name != NULL);
            /* Outward orientation is only meaningful for closed sections. */
            if (info != NULL && info->require_outward_orientation) {
                TG_EXPECT(info->require_closed_manifold);
            }
        }
        TG_EXPECT(mesh_section_info(MESH_SECTION_COUNT) == NULL);
        for (i = 0; i < MESH_MAT_COUNT; ++i) {
            TG_EXPECT(strcmp(mesh_material_name((MeshMaterial)i), "invalid") != 0);
        }
    }
}

static void test_section_discipline(void) {
    Mesh m;
    MeshVertex v = make_vertex(v3(0, 0, 0), v3(0, 1, 0), 0, 0,
                               MESH_MAT_BARK_YOUNG, MESH_SECTION_WOOD);

    TG_T_CASE("appending outside a section is refused");
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_ERR(mesh_add_vertex(&m, &v, NULL), TG_ERR_INVALID_STATE);

    TG_T_CASE("nested begin_section is refused");
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_WOOD));
    TG_EXPECT_ERR(mesh_begin_section(&m, MESH_SECTION_LEAF), TG_ERR_INVALID_STATE);

    TG_T_CASE("out-of-range triangle index is caught at insertion");
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    TG_EXPECT_ERR(mesh_add_triangle(&m, 0, 1, 99, 0), TG_ERR_INVALID_ARGUMENT);
    TG_T_CASE("repeated index in a triangle is caught at insertion");
    TG_EXPECT_ERR(mesh_add_triangle(&m, 0, 1, 1, 0), TG_ERR_INVALID_ARGUMENT);
    TG_T_CASE("a failed triangle insert leaves the arrays consistent");
    TG_EXPECT_EQ_U64(mesh_index_count(&m), 0);
    TG_EXPECT_EQ_U64(mesh_triangle_count(&m), 0);

    TG_T_CASE("reopening a closed section is refused");
    TG_EXPECT_OK(mesh_end_section(&m));
    TG_EXPECT_ERR(mesh_begin_section(&m, MESH_SECTION_WOOD), TG_ERR_INVALID_STATE);

    TG_T_CASE("finalize with an open section is refused");
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_LEAF));
    TG_EXPECT_ERR(mesh_finalize(&m), TG_ERR_INVALID_STATE);
    TG_EXPECT_OK(mesh_end_section(&m));
    TG_EXPECT_OK(mesh_finalize(&m));

    TG_T_CASE("a finalized mesh refuses every mutation");
    TG_EXPECT_ERR(mesh_begin_section(&m, MESH_SECTION_BUD), TG_ERR_INVALID_STATE);
    TG_EXPECT_ERR(mesh_compute_normals(&m, MESH_SECTION_WOOD, 1.0f, NULL),
                  TG_ERR_INVALID_STATE);
    TG_EXPECT_ERR(mesh_finalize(&m), TG_ERR_INVALID_STATE);

    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_closed_tube_valid(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;
    u32 hard_violations = 999;

    TG_T_CASE("a correctly built closed tube passes validation");
    TG_EXPECT_OK(mesh_init(&m, 256, 512));
    TG_EXPECT_OK(build_tube(&m, &o, 7));
    TG_EXPECT_OK(mesh_compute_normals(&m, MESH_SECTION_WOOD, 1.2f, &hard_violations));
    TG_EXPECT_OK(mesh_compute_tangents(&m, MESH_SECTION_WOOD));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, NULL, &rep));
    TG_EXPECT(rep.passed);
    TG_EXPECT_EQ_U64(rep.issue_count, 0);

    TG_T_CASE("the tube is one closed component with no boundary edges");
    TG_EXPECT_EQ_U64(rep.closed_components[MESH_SECTION_WOOD], 1);
    TG_EXPECT_EQ_U64(rep.boundary_edges[MESH_SECTION_WOOD], 0);

    TG_T_CASE("enclosed volume matches the analytic truncated cone");
    {
        /* V = (pi h / 3)(r0^2 + r0 r1 + r1^2), reduced by the polygonal
         * approximation factor for an n-gon cross-section:
         *   A_ngon / A_circle = (n / (2 pi)) sin(2 pi / n)
         * Using an independent formula rather than the implementation's own
         * output is the point of this check. */
        f64 r0 = (f64)o.r0, r1 = (f64)o.r1, h = (f64)o.height;
        f64 v_cone = (TG_PI * h / 3.0) * (r0 * r0 + r0 * r1 + r1 * r1);
        f64 n = (f64)o.segments;
        f64 poly_factor = (n / (2.0 * TG_PI)) * sin(2.0 * TG_PI / n);
        f64 expected = v_cone * poly_factor;
        TG_EXPECT_MSG(fabs(rep.enclosed_volume[MESH_SECTION_WOOD] - expected)
                          < expected * 0.01,
                      "volume %.9f, analytic %.9f",
                      rep.enclosed_volume[MESH_SECTION_WOOD], expected);
        /* Positive volume proves outward winding. */
        TG_EXPECT(rep.enclosed_volume[MESH_SECTION_WOOD] > 0.0);
    }

    TG_T_CASE("smooth normals on a tube wall point radially outward");
    {
        const MeshVertex *v = mesh_vertices(&m);
        u32 i;
        u32 checked = 0;
        for (i = 0; i < 24; ++i) {
            /* Wall vertices of the second ring: away from the caps, so the
             * area-weighted average must be almost exactly radial. */
            const MeshVertex *w = &v[24 + i];
            V3 radial = v3_norm_or(v3(w->position.x, 0.0f, w->position.z), v3(1, 0, 0));
            TG_EXPECT_NEAR(v3_dot(w->normal, radial), 1.0f, 0.02);
            checked++;
        }
        TG_EXPECT_EQ_U64(checked, 24);
    }

    TG_T_CASE("tangents are unit and orthogonal to the normals");
    {
        const MeshVertex *v = mesh_vertices(&m);
        u64 i;
        for (i = 0; i < mesh_vertex_count(&m); ++i) {
            TG_EXPECT_NEAR(v3_len(v[i].tangent), 1.0f, 1e-3);
            TG_EXPECT_NEAR(v3_dot(v[i].tangent, v[i].normal), 0.0f, 1e-3);
        }
    }

    /* The tube shares its rim vertices between the wall and the cap, so the
     * averaged normal at the rim is a compromise between radial and axial and
     * genuinely misrepresents both surfaces. The check is supposed to say so.
     * The exact count is an implementation detail of which corners are visited;
     * the invariant that matters is "reported at all here, and zero once split"
     * -- see test_hard_edges_and_watertightness_coexist. */
    TG_T_CASE("un-split rim vertices ARE reported as hard-corner violations");
    TG_EXPECT_MSG(hard_violations > 0,
                  "a 90-degree cap rim was silently smoothed without complaint");

    TG_T_CASE("triangle lookup reports real indices and provenance");
    {
        MeshTriangle t;
        TG_EXPECT(mesh_get_triangle(&m, 0, &t));
        TG_EXPECT_EQ_U64(t.organ_id, 7);
        TG_EXPECT_EQ_U64(t.section, MESH_SECTION_WOOD);
        TG_EXPECT(t.area > 0.0f);
        TG_EXPECT_NEAR(v3_len(t.normal), 1.0f, 1e-4);
        /* The reported indices must actually be the index-buffer values. */
        TG_EXPECT_EQ_U64(t.index[0], mesh_indices(&m)[0]);
        TG_EXPECT_EQ_U64(t.index[1], mesh_indices(&m)[1]);
        TG_EXPECT_EQ_U64(t.index[2], mesh_indices(&m)[2]);
        TG_EXPECT(!mesh_get_triangle(&m, mesh_triangle_count(&m), &t));
    }

    TG_T_CASE("stats report real, self-consistent counts");
    {
        MeshStats st = mesh_stats(&m);
        TG_EXPECT_EQ_U64(st.vertices, mesh_vertex_count(&m));
        TG_EXPECT_EQ_U64(st.triangles, mesh_triangle_count(&m));
        TG_EXPECT_EQ_U64(st.section_triangles[MESH_SECTION_WOOD],
                         mesh_triangle_count(&m));
        TG_EXPECT_EQ_U64(st.gpu_bytes_estimate,
                         st.vertices * sizeof(MeshVertex) + st.triangles * 3 * 4);
    }

    mesh_destroy(&m);
}

/* The single most important structural test in this file.
 *
 * Real bark geometry needs HARD normals at fracture edges, which requires
 * duplicated vertices. Duplicated vertices break index-level edge sharing. If
 * the validator tested topology on index identity it would report every hard
 * edge as a hole, and the only way to get a "watertight" tree would be to make
 * all bark smooth -- which is precisely the quality regression this project
 * forbids. So watertightness must be judged on welded positions. */
static void test_hard_edges_and_watertightness_coexist(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;
    u32 hard_violations = 999;

    TG_T_CASE("split rim vertices give crisp normals AND a watertight solid");
    o.split_rim = true;
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 11));
    TG_EXPECT_OK(mesh_compute_normals(&m, MESH_SECTION_WOOD, 1.2f, &hard_violations));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, NULL, &rep));

    TG_EXPECT_MSG(rep.passed, "split-rim tube failed validation");
    TG_T_CASE("no boundary edges despite the duplicated rim vertices");
    TG_EXPECT_EQ_U64(rep.boundary_edges[MESH_SECTION_WOOD], 0);
    TG_T_CASE("still exactly one closed component after welding");
    TG_EXPECT_EQ_U64(rep.closed_components[MESH_SECTION_WOOD], 1);
    TG_T_CASE("splitting eliminated the hard-corner violations");
    TG_EXPECT_MSG(hard_violations == 0,
                  "%u violations remain after splitting the rim", hard_violations);

    TG_T_CASE("the split really did duplicate vertices (the test is not vacuous)");
    {
        /* 6 rings x 24 + 2 centres + 2 x 24 duplicates = 194. Asserting the
         * count guards against the builder silently ignoring split_rim. */
        TG_EXPECT_EQ_U64(mesh_vertex_count(&m), 6 * 24 + 2 + 48);
    }

    TG_T_CASE("cap normals are exactly axial, wall normals exactly radial");
    {
        const MeshVertex *v = mesh_vertices(&m);
        /* Duplicated bottom-rim vertices start right after the two cap centres. */
        u32 dup0 = 6 * 24 + 2;
        TG_EXPECT_V3_NEAR(v[dup0].normal, v3(0, -1, 0), 1e-6);
        TG_EXPECT(mesh_attrib_flags(v[dup0].attrib) & MESH_VFLAG_SEAM);
        {
            const MeshVertex *w = &v[0]; /* bottom ring, shared only by the wall */
            V3 radial = v3_norm_or(v3(w->position.x, 0.0f, w->position.z), v3(1, 0, 0));
            TG_EXPECT_NEAR(v3_dot(w->normal, radial), 1.0f, 1e-3);
        }
    }

    TG_T_CASE("volume is unaffected by the vertex duplication");
    {
        f64 r0 = (f64)o.r0, r1 = (f64)o.r1, h = (f64)o.height;
        f64 v_cone = (TG_PI * h / 3.0) * (r0 * r0 + r0 * r1 + r1 * r1);
        f64 n = (f64)o.segments;
        f64 expected = v_cone * (n / (2.0 * TG_PI)) * sin(2.0 * TG_PI / n);
        TG_EXPECT_MSG(fabs(rep.enclosed_volume[MESH_SECTION_WOOD] - expected)
                          < expected * 0.01,
                      "volume %.9f, analytic %.9f",
                      rep.enclosed_volume[MESH_SECTION_WOOD], expected);
    }

    mesh_destroy(&m);
}

/* Welding must NOT paper over a genuine crack. */
static void test_near_miss_is_still_a_crack(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;

    TG_T_CASE("a rim displaced by one ulp is reported as a crack, not welded");
    o.split_rim = true;
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 11));
    {
        /* Nudge one duplicated rim vertex by a single ulp. Geometrically this is
         * a sub-micron gap -- exactly the kind of near-miss that a distance
         * tolerance would silently accept and that would later show as a black
         * seam in the render. */
        MeshVertex *v = mesh_vertex_mut(&m, 6 * 24 + 2);
        u32 bits;
        memcpy(&bits, &v->position.x, sizeof bits);
        bits += 1u;
        memcpy(&v->position.x, &bits, sizeof v->position.x);
    }
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_MSG(issue_count_of(&rep, MESH_ISSUE_BOUNDARY_EDGE) > 0,
                  "a one-ulp gap was welded away instead of being reported");
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_sphere_volume(void) {
    Mesh m;
    MeshValidateReport rep;

    TG_T_CASE("sphere passes and its volume matches 4/3 pi r^3 within polygon error");
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_sphere(&m, MESH_SECTION_LEAF, v3(1, 2, 3), 0.5f, 48, 24, 3,
                              false));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, NULL, &rep));
    TG_EXPECT(rep.passed);
    {
        f64 exact = 4.0 / 3.0 * TG_PI * 0.125;
        f64 got = rep.enclosed_volume[MESH_SECTION_LEAF];
        /* An inscribed polyhedron is strictly smaller than the sphere. */
        TG_EXPECT_MSG(got < exact, "polyhedral volume %.9f should be < %.9f",
                      got, exact);
        TG_EXPECT_MSG(got > exact * 0.99, "volume %.9f too small vs %.9f",
                      got, exact);
    }
    TG_T_CASE("volume is independent of the origin offset");
    {
        Mesh m2;
        MeshValidateReport rep2;
        TG_EXPECT_OK(mesh_init(&m2, 0, 0));
        TG_EXPECT_OK(build_sphere(&m2, MESH_SECTION_LEAF, v3(-40, 77, 12), 0.5f, 48,
                                  24, 3, false));
        TG_EXPECT_OK(mesh_finalize(&m2));
        TG_EXPECT_OK(mesh_validate(&m2, NULL, &rep2));
        /* Tolerance is RELATIVE and reflects a real physical limit: positions
         * are float32, so at an offset of ~89 m the position quantum is about
         * 7.6e-6 m, which is ~1.5e-5 of the 0.5 m radius. A volume agreement
         * better than 1e-5 relative is therefore the best achievable and
         * demanding more would be demanding that float32 be exact.
         *
         * Before the per-component local-origin fix in mesh_validate this same
         * comparison differed by 3.7%. */
        TG_EXPECT_NEAR(rep2.enclosed_volume[MESH_SECTION_LEAF],
                       rep.enclosed_volume[MESH_SECTION_LEAF],
                       rep.enclosed_volume[MESH_SECTION_LEAF] * 1e-5);
        mesh_destroy(&m2);
    }
    mesh_destroy(&m);
}

static void test_detects_hole(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;

    TG_T_CASE("a missing cap is detected as boundary edges in a closed section");
    o.omit_top_cap = true;
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(!rep.passed);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_BOUNDARY_EDGE), o.segments);
    TG_EXPECT_EQ_U64(rep.boundary_edges[MESH_SECTION_WOOD], o.segments);
    mesh_validate_log_report(&rep);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_detects_open_section_is_allowed(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;

    TG_T_CASE("the same open geometry PASSES in a section declared open");
    /* This is the peeling-bark-flake case: a shell with a free tear edge must be
     * accepted, and only the woody section is required watertight. If this test
     * fails, the validator is too strict and would block a legitimate feature. */
    o.omit_top_cap = true;
    o.section = MESH_SECTION_BARK_FLAKE;
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, NULL, &rep));
    TG_EXPECT(rep.passed);
    TG_T_CASE("boundary edges are still counted for an open section");
    TG_EXPECT_EQ_U64(rep.boundary_edges[MESH_SECTION_BARK_FLAKE], o.segments);
    mesh_destroy(&m);
}

static void test_detects_winding_and_nonmanifold(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;

    TG_T_CASE("a reversed extra triangle is detected as non-manifold");
    o.flip_one_quad = true;
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, MESH_ISSUE_NON_MANIFOLD_EDGE) > 0 ||
              issue_count_of(&rep, MESH_ISSUE_INCONSISTENT_WINDING) > 0);
    mesh_destroy(&m);

    TG_T_CASE("a duplicated triangle is named as a duplicate, not just non-manifold");
    o = default_tube();
    o.duplicate_one = true;
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_DUPLICATE_TRIANGLE), 1);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_detects_inverted_component(void) {
    Mesh m;
    MeshValidateReport rep;

    TG_T_CASE("a fully inside-out solid is detected as an inverted component");
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_sphere(&m, MESH_SECTION_LEAF, v3_zero(), 0.4f, 16, 8, 0,
                              true));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_INVERTED_COMPONENT), 1);
    TG_EXPECT(rep.enclosed_volume[MESH_SECTION_LEAF] < 0.0);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_one_bad_leaf_among_many(void) {
    Mesh m;
    MeshValidateReport rep;
    u32 i;

    /* THE key test for per-component checking: nine correct leaves and one
     * inverted leaf. If volume were summed over the whole section the total
     * would still be positive and the defect would ship. */
    TG_T_CASE("one inverted leaf among nine correct ones is still detected");
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_LEAF));
    TG_EXPECT_OK(mesh_end_section(&m));
    mesh_destroy(&m);

    /* Rebuild properly: all ten spheres must live in ONE section, so they are
     * added inside a single begin/end pair. */
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_LEAF));
    for (i = 0; i < 10; ++i) {
        /* Inline sphere construction so all components share the open section. */
        u32 slices = 12, stacks = 6, j, k;
        u32 grid[12 * 5];
        u32 north, south;
        bool inv = (i == 4);
        V3 center = v3((f32)i * 2.0f, 0.0f, 0.0f);
        f32 radius = 0.3f;
        for (j = 1; j < stacks; ++j) {
            f32 phi = (f32)j / (f32)stacks * TG_PI_F;
            for (k = 0; k < slices; ++k) {
                f32 theta = (f32)k / (f32)slices * TG_TAU_F;
                V3 nrm = v3(sinf(phi) * cosf(theta), cosf(phi),
                            sinf(phi) * sinf(theta));
                MeshVertex v = make_vertex(v3_add(center, v3_scale(nrm, radius)),
                                           nrm, 0.5f, 0.5f, MESH_MAT_LEAF_UPPER,
                                           MESH_SECTION_LEAF);
                TG_EXPECT_OK(mesh_add_vertex(&m, &v, &grid[(j - 1u) * slices + k]));
            }
        }
        {
            MeshVertex vn = make_vertex(v3_add(center, v3(0, radius, 0)),
                                        v3(0, 1, 0), 0, 0, MESH_MAT_LEAF_UPPER,
                                        MESH_SECTION_LEAF);
            MeshVertex vs = make_vertex(v3_add(center, v3(0, -radius, 0)),
                                        v3(0, -1, 0), 1, 0, MESH_MAT_LEAF_UPPER,
                                        MESH_SECTION_LEAF);
            TG_EXPECT_OK(mesh_add_vertex(&m, &vn, &north));
            TG_EXPECT_OK(mesh_add_vertex(&m, &vs, &south));
        }
        for (k = 0; k < slices; ++k) {
            u32 a = grid[k], b = grid[(k + 1u) % slices];
            TG_EXPECT_OK(inv ? mesh_add_triangle(&m, north, a, b, i)
                             : mesh_add_triangle(&m, north, b, a, i));
        }
        for (j = 1; j + 1 < stacks; ++j) {
            for (k = 0; k < slices; ++k) {
                u32 k1 = (k + 1u) % slices;
                u32 a = grid[(j - 1u) * slices + k];
                u32 b = grid[(j - 1u) * slices + k1];
                u32 c = grid[j * slices + k1];
                u32 d = grid[j * slices + k];
                TG_EXPECT_OK(inv ? mesh_add_quad(&m, a, d, c, b, i)
                                 : mesh_add_quad(&m, a, b, c, d, i));
            }
        }
        for (k = 0; k < slices; ++k) {
            u32 a = grid[(stacks - 2u) * slices + k];
            u32 b = grid[(stacks - 2u) * slices + (k + 1u) % slices];
            TG_EXPECT_OK(inv ? mesh_add_triangle(&m, south, b, a, i)
                             : mesh_add_triangle(&m, south, a, b, i));
        }
    }
    TG_EXPECT_OK(mesh_end_section(&m));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_EQ_U64(rep.closed_components[MESH_SECTION_LEAF], 10);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_INVERTED_COMPONENT), 1);
    TG_T_CASE("the summed section volume would have hidden it");
    TG_EXPECT_MSG(rep.enclosed_volume[MESH_SECTION_LEAF] > 0.0,
                  "summed volume was %.9f; the point of the per-component check "
                  "is that this number is positive and therefore useless alone",
                  rep.enclosed_volume[MESH_SECTION_LEAF]);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_detects_numeric_corruption(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;
    volatile f32 zero = 0.0f;

    TG_T_CASE("a NaN position is detected");
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    mesh_vertex_mut(&m, 5)->position.x = 0.0f / zero;
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, MESH_ISSUE_NON_FINITE_POSITION) >= 1);
    mesh_destroy(&m);

    TG_T_CASE("a non-unit normal is detected");
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    mesh_vertex_mut(&m, 5)->normal = v3(0.0f, 0.5f, 0.0f);
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_NORMAL_NOT_UNIT), 1);
    mesh_destroy(&m);

    TG_T_CASE("an out-of-range AO value is detected");
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    mesh_vertex_mut(&m, 5)->ao = 1.5f;
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_AO_OUT_OF_RANGE), 1);
    mesh_destroy(&m);

    TG_T_CASE("a collapsed (zero-area) triangle is detected");
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    /* Collapse a wall vertex onto its neighbour: the two triangles sharing them
     * become slivers of zero area without changing topology. */
    mesh_vertex_mut(&m, 1)->position = mesh_vertices(&m)[0].position;
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_ERR(mesh_validate(&m, NULL, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, MESH_ISSUE_ZERO_AREA) >= 1);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_scale_relative_area_threshold(void) {
    Mesh m;
    MeshValidateReport rep;
    MeshValidateOptions opt = mesh_validate_default_options();

    /* A small leaf-vein-scale triangle must NOT be rejected merely for being
     * small. This is why the threshold is relative to the bounds diagonal. */
    TG_T_CASE("fine sub-millimetre geometry is accepted in a tree-sized scene");
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_CUTAWAY));
    {
        /* One far-away vertex sets the scene scale to ~20 m; the triangle itself
         * is 0.3 mm on a side, the scale of a real leaf vein cross-section. */
        MeshVertex a = make_vertex(v3(0, 0, 0), v3(0, 1, 0), 0, 0,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        MeshVertex b = make_vertex(v3(0.0003f, 0, 0), v3(0, 1, 0), 1, 0,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        MeshVertex c = make_vertex(v3(0, 0, 0.0003f), v3(0, 1, 0), 0, 1,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        MeshVertex d = make_vertex(v3(0, 20.0f, 0), v3(0, 1, 0), 0, 0,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        MeshVertex e = make_vertex(v3(1.0f, 20.0f, 0), v3(0, 1, 0), 0, 0,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        MeshVertex f = make_vertex(v3(0.0f, 20.0f, 1.0f), v3(0, 1, 0), 0, 0,
                                   MESH_MAT_LEAF_UPPER, MESH_SECTION_CUTAWAY);
        u32 ia, ib, ic, id, ie, iff;
        TG_EXPECT_OK(mesh_add_vertex(&m, &a, &ia));
        TG_EXPECT_OK(mesh_add_vertex(&m, &b, &ib));
        TG_EXPECT_OK(mesh_add_vertex(&m, &c, &ic));
        TG_EXPECT_OK(mesh_add_vertex(&m, &d, &id));
        TG_EXPECT_OK(mesh_add_vertex(&m, &e, &ie));
        TG_EXPECT_OK(mesh_add_vertex(&m, &f, &iff));
        TG_EXPECT_OK(mesh_add_triangle(&m, ia, ib, ic, 0));
        TG_EXPECT_OK(mesh_add_triangle(&m, id, ie, iff, 0));
    }
    TG_EXPECT_OK(mesh_end_section(&m));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, &opt, &rep));
    TG_EXPECT_MSG(rep.passed, "fine geometry was wrongly rejected");
    TG_EXPECT(rep.min_triangle_area > 0.0f);
    mesh_destroy(&m);
}

static void test_fingerprint_behaviour(void) {
    Mesh a, b;
    TubeOptions o = default_tube();
    TgFingerprint fa, fb;

    TG_T_CASE("identical construction gives an identical fingerprint");
    TG_EXPECT_OK(mesh_init(&a, 0, 0));
    TG_EXPECT_OK(build_tube(&a, &o, 3));
    TG_EXPECT_OK(mesh_compute_normals(&a, MESH_SECTION_WOOD, 1.2f, NULL));
    TG_EXPECT_OK(mesh_finalize(&a));

    TG_EXPECT_OK(mesh_init(&b, 999, 999)); /* different reservation on purpose */
    TG_EXPECT_OK(build_tube(&b, &o, 3));
    TG_EXPECT_OK(mesh_compute_normals(&b, MESH_SECTION_WOOD, 1.2f, NULL));
    TG_EXPECT_OK(mesh_finalize(&b));

    fa = mesh_fingerprint(&a);
    fb = mesh_fingerprint(&b);
    TG_EXPECT_MSG(tg_fp_equal(fa, fb),
                  "fingerprint depends on allocation history: %llx vs %llx",
                  (unsigned long long)fa.value, (unsigned long long)fb.value);
    TG_EXPECT(!fa.saw_non_finite);
    mesh_destroy(&b);

    TG_T_CASE("moving a single vertex by one ulp changes the fingerprint");
    {
        Mesh c;
        TgFingerprint fc;
        u32 bits;
        TG_EXPECT_OK(mesh_init(&c, 0, 0));
        TG_EXPECT_OK(build_tube(&c, &o, 3));
        TG_EXPECT_OK(mesh_compute_normals(&c, MESH_SECTION_WOOD, 1.2f, NULL));
        {
            MeshVertex *v = mesh_vertex_mut(&c, 10);
            memcpy(&bits, &v->position.y, sizeof bits);
            bits += 1u;
            memcpy(&v->position.y, &bits, sizeof v->position.y);
        }
        TG_EXPECT_OK(mesh_finalize(&c));
        fc = mesh_fingerprint(&c);
        TG_EXPECT(!tg_fp_equal(fa, fc));
        mesh_destroy(&c);
    }

    TG_T_CASE("a different organ id changes the fingerprint");
    {
        Mesh d;
        TgFingerprint fd;
        TG_EXPECT_OK(mesh_init(&d, 0, 0));
        TG_EXPECT_OK(build_tube(&d, &o, 4)); /* organ 4 instead of 3 */
        TG_EXPECT_OK(mesh_compute_normals(&d, MESH_SECTION_WOOD, 1.2f, NULL));
        TG_EXPECT_OK(mesh_finalize(&d));
        fd = mesh_fingerprint(&d);
        TG_EXPECT(!tg_fp_equal(fa, fd));
        mesh_destroy(&d);
    }

    mesh_destroy(&a);
}

static void test_limits_and_reset(void) {
    Mesh m;
    MeshVertex v = make_vertex(v3(0, 0, 0), v3(0, 1, 0), 0, 0,
                               MESH_MAT_BARK_YOUNG, MESH_SECTION_WOOD);

    TG_T_CASE("vertex and triangle ceilings are reported, not exceeded");
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    m.max_vertices = 4;
    m.max_triangles = 1;
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_WOOD));
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    v.position = v3(1, 0, 0);
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    v.position = v3(0, 0, 1);
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    v.position = v3(1, 1, 1);
    TG_EXPECT_OK(mesh_add_vertex(&m, &v, NULL));
    TG_EXPECT_ERR(mesh_add_vertex(&m, &v, NULL), TG_ERR_LIMIT_EXCEEDED);
    TG_EXPECT_OK(mesh_add_triangle(&m, 0, 1, 2, 0));
    TG_EXPECT_ERR(mesh_add_triangle(&m, 0, 1, 3, 0), TG_ERR_LIMIT_EXCEEDED);
    TG_T_CASE("a refused triangle does not corrupt the arrays");
    TG_EXPECT_EQ_U64(mesh_index_count(&m), 3);
    TG_EXPECT_EQ_U64(mesh_triangle_count(&m), 1);
    TG_EXPECT_OK(mesh_end_section(&m));

    TG_T_CASE("reset clears content and allows rebuilding the same section");
    mesh_reset(&m);
    TG_EXPECT_EQ_U64(mesh_vertex_count(&m), 0);
    TG_EXPECT_EQ_U64(mesh_triangle_count(&m), 0);
    TG_EXPECT(!m.finalized);
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_WOOD));
    TG_EXPECT_OK(mesh_end_section(&m));
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_topology_scratch_limit(void) {
    Mesh m;
    TubeOptions o = default_tube();
    MeshValidateReport rep;
    MeshValidateOptions opt = mesh_validate_default_options();

    TG_T_CASE("a scratch budget too small reports a failure instead of passing");
    /* Critical behaviour: being unable to verify topology must never be
     * reported as success. */
    opt.max_scratch_bytes = 16;
    tg_test_logs_mute();
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(build_tube(&m, &o, 1));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT(mesh_validate(&m, &opt, &rep) != TG_OK);
    TG_EXPECT(!rep.passed);
    TG_EXPECT_EQ_U64(issue_count_of(&rep, MESH_ISSUE_SCRATCH_LIMIT), 1);

    TG_T_CASE("skip_topology still runs the numeric and geometric checks");
    opt = mesh_validate_default_options();
    opt.skip_topology = true;
    TG_EXPECT_OK(mesh_validate(&m, &opt, &rep));
    TG_EXPECT(rep.passed);
    TG_EXPECT_EQ_U64(rep.closed_components[MESH_SECTION_WOOD], 0);
    mesh_destroy(&m);
    tg_test_logs_unmute();
}

static void test_empty_mesh(void) {
    Mesh m;
    MeshValidateReport rep;

    TG_T_CASE("an empty mesh is valid and reports zeros: the startup state");
    /* The application starts with no tree at all. That state must validate
     * cleanly and report genuinely zero counts. */
    TG_EXPECT_OK(mesh_init(&m, 0, 0));
    TG_EXPECT_OK(mesh_finalize(&m));
    TG_EXPECT_OK(mesh_validate(&m, NULL, &rep));
    TG_EXPECT(rep.passed);
    TG_EXPECT_EQ_U64(rep.checked_triangles, 0);
    TG_EXPECT_EQ_U64(rep.checked_vertices, 0);
    {
        MeshStats st = mesh_stats(&m);
        TG_EXPECT_EQ_U64(st.vertices, 0);
        TG_EXPECT_EQ_U64(st.triangles, 0);
        TG_EXPECT_EQ_U64(st.gpu_bytes_estimate, 0);
    }
    mesh_destroy(&m);
}

/* Emits an axis-aligned box as a closed, outward-wound solid: 8 vertices, 12
 * triangles. Used to build a mesh large enough that the topology check's scratch
 * budget matters. */
/* Failures are ACCUMULATED rather than asserted per call. Asserting inside the
 * helper added 336 000 checks to the suite for 24 000 boxes, which is both slow in a
 * debug build and noise in the per-suite check count that regressions are read
 * against. */
static u32 emit_box(Mesh *m, V3 lo, V3 hi, u32 organ) {
    u32 failures = 0;
    static const u8 face[6][4] = {
        { 0u, 2u, 3u, 1u },  /* -z */
        { 4u, 5u, 7u, 6u },  /* +z */
        { 0u, 1u, 5u, 4u },  /* -y */
        { 2u, 6u, 7u, 3u },  /* +y */
        { 0u, 4u, 6u, 2u },  /* -x */
        { 1u, 3u, 7u, 5u }   /* +x */
    };
    u32 idx[8];
    u32 i;
    for (i = 0; i < 8u; ++i) {
        MeshVertex v;
        memset(&v, 0, sizeof v);
        v.position = v3((i & 1u) ? hi.x : lo.x,
                        (i & 2u) ? hi.y : lo.y,
                        (i & 4u) ? hi.z : lo.z);
        v.normal = v3_norm_or(v3_sub(v.position, v3_scale(v3_add(lo, hi), 0.5f)),
                              v3(0.0f, 1.0f, 0.0f));
        v.tangent = v3(1.0f, 0.0f, 0.0f);
        v.color = 0xFF808080u;
        v.ao = 1.0f;
        v.organ_id = organ;
        v.attrib = mesh_pack_attrib(MESH_MAT_BARK_MATURE, MESH_SECTION_WOOD, 0);
        if (mesh_add_vertex(m, &v, &idx[i]) != TG_OK) { failures++; }
    }
    for (i = 0; i < 6u; ++i) {
        if (mesh_add_quad(m, idx[face[i][0]], idx[face[i][1]], idx[face[i][2]],
                          idx[face[i][3]], organ) != TG_OK) {
            failures++;
        }
    }
    return failures;
}

static void test_topology_scratch_is_a_peak_not_a_sum(void) {
    enum { BOXES = 24000 };   /* 288 000 triangles, 192 000 vertices          */
    Mesh m;
    MeshValidateReport rep;
    MeshValidateOptions opt;
    u32 i;

    TG_T_CASE("a large mesh validates within a budget the old accounting refused");
    /* The topology check runs four phases -- position welding, edge classification,
     * duplicate-triangle detection, per-component volume -- and each releases its
     * working set before the next allocates. It used to check the caller's limit
     * against the SUM of all four, and to materialise a 16-byte record per directed
     * edge plus a radix-sort scratch buffer of the same size.
     *
     * That was not a conservative simplification. It refused to verify the topology of
     * the two largest trees in the project -- a 20.5-million-triangle broadleaf and a
     * 17-million-triangle conifer -- at a two-gibibyte limit, which is exactly where a
     * topological defect is most likely and least visible. Both now verify at one
     * gibibyte, and an 80-year broadleaf verifies at the library's 512 MiB default.
     *
     * The budget below is chosen to sit between the two: comfortably above the peak
     * the phased implementation needs for this mesh, and comfortably below what the
     * summed accounting would have demanded. A regression to the old scheme fails
     * here rather than only on a tree too big to put in a test. */
    TG_EXPECT_OK(mesh_init(&m, BOXES * 8u + 16u, BOXES * 12u + 16u));
    TG_EXPECT_OK(mesh_begin_section(&m, MESH_SECTION_WOOD));
    {
        u32 emit_failures = 0;
        for (i = 0; i < (u32)BOXES; ++i) {
            f32 x = (f32)(i % 200u) * 1.0f;
            f32 z = (f32)(i / 200u) * 1.0f;
            emit_failures += emit_box(&m, v3(x, 0.0f, z),
                                      v3(x + 0.5f, 0.5f, z + 0.5f), i);
        }
        TG_EXPECT_MSG(emit_failures == 0u, "%u appends failed while building the "
                                           "fixture", emit_failures);
    }
    TG_EXPECT_OK(mesh_end_section(&m));
    TG_EXPECT_OK(mesh_finalize(&m));

    opt = mesh_validate_default_options();
    opt.max_scratch_bytes = (u64)16 * 1024 * 1024;
    TG_EXPECT_OK(mesh_validate(&m, &opt, &rep));
    TG_EXPECT_MSG(!rep.topology_not_checked,
                  "topology was skipped: %llu triangles were refused a 16 MiB "
                  "budget", (unsigned long long)mesh_triangle_count(&m));
    TG_EXPECT_MSG(rep.passed, "a mesh of %d disjoint boxes failed validation", BOXES);
    if (!rep.passed) { mesh_validate_log_report(&rep); }
    TG_EXPECT_MSG(rep.closed_components[MESH_SECTION_WOOD] == (u32)BOXES,
                  "%u closed components for %d boxes",
                  rep.closed_components[MESH_SECTION_WOOD], BOXES);
    TG_EXPECT_MSG(rep.boundary_edges[MESH_SECTION_WOOD] == 0,
                  "%llu boundary edges on closed boxes",
                  (unsigned long long)rep.boundary_edges[MESH_SECTION_WOOD]);
    /* 0.5^3 per box, and the volume is what proves the winding is outward. */
    TG_EXPECT_MSG(tg_absf((f32)rep.enclosed_volume[MESH_SECTION_WOOD]
                          - (f32)BOXES * 0.125f) < 1.0f,
                  "enclosed volume %.4f against an expected %.4f",
                  rep.enclosed_volume[MESH_SECTION_WOOD], (double)BOXES * 0.125);

    TG_T_CASE("and a budget below the peak is still refused, not silently ignored");
    opt.max_scratch_bytes = (u64)64 * 1024;
    TG_EXPECT(mesh_validate(&m, &opt, &rep) != TG_OK);
    TG_EXPECT_MSG(rep.topology_not_checked,
                  "a 64 KiB budget was accepted for 288 000 triangles");
    mesh_destroy(&m);
}

void test_suite_mesh(void) {
    test_topology_scratch_is_a_peak_not_a_sum();
    test_vertex_layout();
    test_section_discipline();
    test_closed_tube_valid();
    test_hard_edges_and_watertightness_coexist();
    test_near_miss_is_still_a_crack();
    test_sphere_volume();
    test_detects_hole();
    test_detects_open_section_is_allowed();
    test_detects_winding_and_nonmanifold();
    test_detects_inverted_component();
    test_one_bad_leaf_among_many();
    test_detects_numeric_corruption();
    test_scale_relative_area_threshold();
    test_fingerprint_behaviour();
    test_limits_and_reset();
    test_topology_scratch_limit();
    test_empty_mesh();
}
