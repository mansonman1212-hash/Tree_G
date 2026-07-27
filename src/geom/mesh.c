#include "mesh.h"

#include "../core/log.h"

#include <string.h>

#define MESH_SUB "mesh"

/* ------------------------------------------------------------------------- */
/* Section and material metadata                                             */
/* ------------------------------------------------------------------------- */

static const MeshSectionInfo g_section_info[MESH_SECTION_COUNT] = {
    /* name                closed  outward  cull   two-sided */
    { "wood",              true,   true,    true,  false },
    { "exposed_wood",      false,  false,   true,  false },
    /* A peeling flake is a shell with a free tear edge. Open by design. */
    { "bark_flake",        false,  false,   false, true  },
    { "bud",               true,   true,    true,  false },
    { "petiole",           true,   true,    true,  false },
    /* A leaf is a thin closed solid, so it IS a closed manifold -- just one
     * component per leaf. It is shaded two-sided because light physically
     * transmits through the blade, and it is not backface-culled because the
     * underside is a real, visible surface. */
    { "leaf",              true,   true,    false, true  },
    { "needle",            true,   true,    false, true  },
    { "fascicle",          true,   true,    true,  false },
    /* A cut surface is by definition an open boundary. */
    { "cutaway",           false,  false,   false, true  }
};

const MeshSectionInfo *mesh_section_info(MeshSection s) {
    if ((u32)s >= (u32)MESH_SECTION_COUNT) { return NULL; }
    return &g_section_info[s];
}

const char *mesh_material_name(MeshMaterial m) {
    static const char *names[MESH_MAT_COUNT] = {
        "bark_young", "bark_mature", "bark_furrow", "bark_flake",
        "woundwood", "dead_wood", "heartwood", "sapwood", "cambium", "pith",
        "root", "bud_scale", "petiole",
        "leaf_upper", "leaf_lower", "leaf_edge", "needle"
    };
    if ((u32)m >= (u32)MESH_MAT_COUNT) { return "invalid"; }
    return names[m];
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------- */

TgResult mesh_init(Mesh *m, u64 reserve_vertices, u64 reserve_triangles) {
    TgResult r;
    u32 i;

    TG_CHECK(m != NULL);
    memset(m, 0, sizeof *m);
    m->open_section = MESH_SECTION_COUNT;
    m->bounds = aabb_empty();
    m->max_vertices = MESH_DEFAULT_MAX_VERTICES;
    m->max_triangles = MESH_DEFAULT_MAX_TRIANGLES;

    r = TG_ARRAY_INIT(&m->vertices, MeshVertex, reserve_vertices, "mesh.vertices");
    if (r != TG_OK) { goto fail; }
    r = TG_ARRAY_INIT(&m->indices, u32, reserve_triangles * 3, "mesh.indices");
    if (r != TG_OK) { goto fail; }
    r = TG_ARRAY_INIT(&m->tri_organ, u32, reserve_triangles, "mesh.tri_organ");
    if (r != TG_OK) { goto fail; }

    for (i = 0; i < MESH_SECTION_COUNT; ++i) {
        memset(&m->span[i], 0, sizeof m->span[i]);
        m->section_used[i] = false;
    }
    return TG_OK;

fail:
    mesh_destroy(m);
    return r;
}

void mesh_destroy(Mesh *m) {
    if (m == NULL) { return; }
    tg_array_free(&m->vertices);
    tg_array_free(&m->indices);
    tg_array_free(&m->tri_organ);
    memset(m, 0, sizeof *m);
    m->open_section = MESH_SECTION_COUNT;
    m->bounds = aabb_empty();
}

void mesh_reset(Mesh *m) {
    u32 i;
    TG_CHECK(m != NULL);
    tg_array_clear(&m->vertices);
    tg_array_clear(&m->indices);
    tg_array_clear(&m->tri_organ);
    for (i = 0; i < MESH_SECTION_COUNT; ++i) {
        memset(&m->span[i], 0, sizeof m->span[i]);
        m->section_used[i] = false;
    }
    m->bounds = aabb_empty();
    m->open_section = MESH_SECTION_COUNT;
    m->finalized = false;
}

MeshVertex *mesh_vertex_mut(Mesh *m, u64 index) {
    TG_CHECK(m != NULL);
    TG_CHECK_MSG(!m->finalized, "attempted to mutate a finalized mesh");
    TG_CHECK(index < m->vertices.count);
    return (MeshVertex *)tg_array_at(&m->vertices, index);
}

/* ------------------------------------------------------------------------- */
/* Sections                                                                  */
/* ------------------------------------------------------------------------- */

TgResult mesh_begin_section(Mesh *m, MeshSection s) {
    MeshSectionSpan *sp;

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if ((u32)s >= (u32)MESH_SECTION_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (m->open_section != MESH_SECTION_COUNT) {
        TG_LOG_ERRORF(MESH_SUB, "mesh_begin_section(%s) while '%s' is still open",
                      g_section_info[s].name,
                      g_section_info[m->open_section].name);
        return TG_ERR_INVALID_STATE;
    }
    if (m->section_used[s]) {
        /* Reopening would break the contiguity guarantee that the whole design
         * relies on. Reported as a programming error, not silently allowed. */
        TG_LOG_ERRORF(MESH_SUB, "section '%s' reopened; sections must be contiguous",
                      g_section_info[s].name);
        return TG_ERR_INVALID_STATE;
    }

    sp = &m->span[s];
    sp->first_vertex = (u32)m->vertices.count;
    sp->vertex_count = 0;
    sp->first_index = (u32)m->indices.count;
    sp->index_count = 0;
    sp->first_triangle = (u32)m->tri_organ.count;
    sp->triangle_count = 0;

    m->section_used[s] = true;
    m->open_section = s;
    return TG_OK;
}

TgResult mesh_end_section(Mesh *m) {
    TG_CHECK(m != NULL);
    if (m->open_section == MESH_SECTION_COUNT) { return TG_ERR_INVALID_STATE; }
    m->open_section = MESH_SECTION_COUNT;
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Appending                                                                 */
/* ------------------------------------------------------------------------- */

TgResult mesh_add_vertices(Mesh *m, const MeshVertex *v, u64 count, u32 *out_first) {
    TgResult r;
    u64 first;
    u64 new_total;

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if (m->open_section == MESH_SECTION_COUNT) {
        TG_LOG_ERRORF(MESH_SUB, "mesh_add_vertices outside any section");
        return TG_ERR_INVALID_STATE;
    }
    if (count == 0) {
        if (out_first != NULL) { *out_first = (u32)m->vertices.count; }
        return TG_OK;
    }
    if (!tg_ckd_add_u64(m->vertices.count, count, &new_total)) {
        return TG_ERR_OVERFLOW;
    }
    if (new_total > m->max_vertices || new_total > (u64)UINT32_MAX) {
        /* u32 indices are a deliberate format choice; exceeding them is a
         * reportable limit, not an abort. */
        TG_LOG_ERRORF(MESH_SUB,
                      "vertex limit exceeded: %llu requested, cap %llu",
                      (unsigned long long)new_total,
                      (unsigned long long)tg_min_u64(m->max_vertices, UINT32_MAX));
        return TG_ERR_LIMIT_EXCEEDED;
    }

    r = tg_array_push_n(&m->vertices, v, count, &first);
    if (r != TG_OK) { return r; }

    m->span[m->open_section].vertex_count += (u32)count;
    if (out_first != NULL) { *out_first = (u32)first; }
    return TG_OK;
}

TgResult mesh_add_vertex(Mesh *m, const MeshVertex *v, u32 *out_index) {
    return mesh_add_vertices(m, v, 1, out_index);
}

TgResult mesh_add_triangle(Mesh *m, u32 a, u32 b, u32 c, u32 organ_id) {
    u32 idx[3];
    u64 vcount;
    u64 new_tris;
    TgResult r;

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if (m->open_section == MESH_SECTION_COUNT) {
        TG_LOG_ERRORF(MESH_SUB, "mesh_add_triangle outside any section");
        return TG_ERR_INVALID_STATE;
    }

    vcount = m->vertices.count;
    if (a >= vcount || b >= vcount || c >= vcount) {
        /* Caught here, at the mistake, rather than during validation. */
        TG_LOG_ERRORF(MESH_SUB,
                      "triangle references out-of-range vertex (%u,%u,%u) of %llu",
                      a, b, c, (unsigned long long)vcount);
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (a == b || b == c || a == c) {
        TG_LOG_ERRORF(MESH_SUB, "degenerate triangle with repeated index (%u,%u,%u)",
                      a, b, c);
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (!tg_ckd_add_u64(m->tri_organ.count, 1, &new_tris)) { return TG_ERR_OVERFLOW; }
    if (new_tris > m->max_triangles) {
        TG_LOG_ERRORF(MESH_SUB, "triangle limit exceeded: cap %llu",
                      (unsigned long long)m->max_triangles);
        return TG_ERR_LIMIT_EXCEEDED;
    }

    idx[0] = a;
    idx[1] = b;
    idx[2] = c;
    r = tg_array_push_n(&m->indices, idx, 3, NULL);
    if (r != TG_OK) { return r; }
    r = tg_array_push(&m->tri_organ, &organ_id, NULL);
    if (r != TG_OK) {
        /* Keep the two arrays consistent: roll the indices back. Leaving them
         * mismatched would corrupt every subsequent triangle id. */
        (void)tg_array_resize(&m->indices, m->indices.count - 3);
        return r;
    }

    m->span[m->open_section].index_count += 3;
    m->span[m->open_section].triangle_count += 1;
    return TG_OK;
}

TgResult mesh_add_quad(Mesh *m, u32 a, u32 b, u32 c, u32 d, u32 organ_id) {
    TgResult r = mesh_add_triangle(m, a, b, c, organ_id);
    if (r != TG_OK) { return r; }
    return mesh_add_triangle(m, a, c, d, organ_id);
}

TgResult mesh_stitch_rings(Mesh *m, const u32 *ring_a, const u32 *ring_b,
                           u32 ring_size, u32 organ_id) {
    u32 i;
    TgResult r;

    TG_CHECK(m != NULL);
    if (ring_a == NULL || ring_b == NULL) { return TG_ERR_INVALID_ARGUMENT; }
    if (ring_size < 3) { return TG_ERR_INVALID_ARGUMENT; }

    for (i = 0; i < ring_size; ++i) {
        u32 j = (i + 1u) % ring_size;
        /* Quad (a_i, a_j, b_j, b_i). With rings wound counter-clockwise about
         * the frame tangent and ring_b distal, this ordering yields outward
         * normals under the CCW-front convention. Verified by the outward
         * signed-volume check in mesh_validate. */
        r = mesh_add_quad(m, ring_a[i], ring_a[j], ring_b[j], ring_b[i], organ_id);
        if (r != TG_OK) { return r; }
    }
    return TG_OK;
}

TgResult mesh_cap_ring(Mesh *m, const u32 *ring, u32 ring_size, u32 center,
                       bool outward, u32 organ_id) {
    u32 i;
    TgResult r;

    TG_CHECK(m != NULL);
    if (ring == NULL || ring_size < 3) { return TG_ERR_INVALID_ARGUMENT; }

    for (i = 0; i < ring_size; ++i) {
        u32 j = (i + 1u) % ring_size;
        if (outward) {
            r = mesh_add_triangle(m, center, ring[i], ring[j], organ_id);
        } else {
            r = mesh_add_triangle(m, center, ring[j], ring[i], organ_id);
        }
        if (r != TG_OK) { return r; }
    }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Normals and tangents                                                      */
/* ------------------------------------------------------------------------- */

TgResult mesh_compute_normals(Mesh *m, MeshSection s, f32 hard_angle,
                              u32 *out_hard_corner_violations) {
    const MeshSectionSpan *sp;
    const u32 *ind;
    MeshVertex *verts;
    V3 *accum;
    u32 i, t, violations = 0;
    u64 accum_bytes;

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if ((u32)s >= (u32)MESH_SECTION_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (out_hard_corner_violations != NULL) { *out_hard_corner_violations = 0; }
    if (!m->section_used[s]) { return TG_OK; }

    sp = &m->span[s];
    if (sp->vertex_count == 0 || sp->triangle_count == 0) { return TG_OK; }

    if (!tg_ckd_mul_u64(sp->vertex_count, sizeof(V3), &accum_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    accum = (V3 *)tg_alloc_zero(accum_bytes);
    if (accum == NULL) {
        TG_LOG_ERRORF(MESH_SUB, "normal accumulation buffer (%llu bytes) failed",
                      (unsigned long long)accum_bytes);
        return TG_ERR_OUT_OF_MEMORY;
    }

    ind = mesh_indices(m);
    verts = (MeshVertex *)m->vertices.data;

    /* Pass 1: accumulate ANGLE-WEIGHTED face normals.
     *
     * WHY ANGLE WEIGHTING AND NOT AREA WEIGHTING. Almost all woody geometry here
     * is built by stitching cross-section rings into quads, and every quad is
     * split along a fixed diagonal. With area weighting, the two triangles of a
     * quad contribute unequally to the corners the diagonal passes through, so a
     * vertex receives a biased normal whose error is aligned with the diagonal
     * direction. Repeated over a whole trunk that reads as faint diagonal
     * striping in the shading -- a visual artefact with no physical cause.
     *
     * Angle weighting (Thurmer-Wuthrich) is invariant to how a polygon is
     * triangulated, because the interior angles at a polygon corner always sum
     * to that corner's full angle regardless of the diagonal chosen. It is
     * therefore the correct weighting for this mesh topology.
     *
     * Measured on a 24-segment tapered tube: area weighting left the rim normal
     * 0.056 rad off the true surface normal, angle weighting 0.037 rad, which is
     * exactly the tube's own taper angle -- i.e. no residual error. */
    for (t = 0; t < sp->triangle_count; ++t) {
        u32 base = sp->first_index + t * 3u;
        u32 tri[3];
        V3 p[3];
        V3 fn;
        u32 k;

        tri[0] = ind[base + 0];
        tri[1] = ind[base + 1];
        tri[2] = ind[base + 2];
        /* Indices are guaranteed within the section span because vertices and
         * triangles for a section are appended together. Assert rather than
         * branch in the hot loop. */
        TG_ASSERT(tri[0] >= sp->first_vertex &&
                  tri[0] < sp->first_vertex + sp->vertex_count);
        TG_ASSERT(tri[1] >= sp->first_vertex &&
                  tri[1] < sp->first_vertex + sp->vertex_count);
        TG_ASSERT(tri[2] >= sp->first_vertex &&
                  tri[2] < sp->first_vertex + sp->vertex_count);

        for (k = 0; k < 3; ++k) { p[k] = verts[tri[k]].position; }
        fn = triangle_normal_unnormalized(p[0], p[1], p[2]);
        if (!v3_finite(fn) || v3_len_sq(fn) < TG_TINY_F) { continue; }
        fn = v3_scale(fn, 1.0f / v3_len(fn));

        for (k = 0; k < 3; ++k) {
            V3 e1 = v3_sub(p[(k + 1u) % 3u], p[k]);
            V3 e2 = v3_sub(p[(k + 2u) % 3u], p[k]);
            f32 w = v3_angle_between(e1, e2);
            if (!tg_finitef(w) || w <= 0.0f) { continue; }
            accum[tri[k] - sp->first_vertex] =
                v3_add(accum[tri[k] - sp->first_vertex], v3_scale(fn, w));
        }
    }

    /* Pass 2: normalise. A vertex whose accumulated normal cancels to zero
     * (possible on a pathological fold) keeps whatever normal it had rather
     * than becoming NaN. */
    for (i = 0; i < sp->vertex_count; ++i) {
        MeshVertex *v = &verts[sp->first_vertex + i];
        v->normal = v3_norm_or(accum[i], v->normal);
    }

    /* Pass 3: report corners where smoothing has visibly lied about the surface.
     * This does not modify geometry -- it tells the generator it forgot to split.
     *
     * `hard_angle` is the maximum acceptable DIHEDRAL SPREAD between the faces
     * meeting at a vertex, which is the quantity that actually decides whether
     * smoothing is defensible.
     *
     * Comparing a face normal directly against the vertex normal does NOT
     * measure that, and getting this wrong is easy: at a clean 90-degree edge the
     * averaged normal lands on the bisector, so every incident face deviates by
     * only 45 degrees and a 69-degree threshold sees nothing wrong. That is
     * precisely the case this check must catch.
     *
     * Since the largest deviation of any incident face from the average is at
     * least half the total spread, comparing 2*deviation against hard_angle is a
     * sound and cheap proxy: flag when dot(face, vertex) < cos(hard_angle / 2). */
    if (hard_angle > 0.0f && hard_angle < TG_PI_F) {
        f32 cos_limit = cosf(hard_angle * 0.5f);
        for (t = 0; t < sp->triangle_count; ++t) {
            u32 base = sp->first_index + t * 3u;
            u32 tri[3];
            V3 fn;
            u32 k;
            tri[0] = ind[base + 0];
            tri[1] = ind[base + 1];
            tri[2] = ind[base + 2];
            fn = v3_norm_or(triangle_normal_unnormalized(verts[tri[0]].position,
                                                        verts[tri[1]].position,
                                                        verts[tri[2]].position),
                            v3_zero());
            if (v3_len_sq(fn) < 0.5f) { continue; }
            for (k = 0; k < 3; ++k) {
                if (v3_dot(fn, verts[tri[k]].normal) < cos_limit) { violations++; }
            }
        }
    }

    tg_free(accum, accum_bytes);
    if (out_hard_corner_violations != NULL) {
        *out_hard_corner_violations = violations;
    }
    return TG_OK;
}

TgResult mesh_compute_tangents(Mesh *m, MeshSection s) {
    const MeshSectionSpan *sp;
    const u32 *ind;
    MeshVertex *verts;
    V3 *accum;
    u32 i, t;
    u64 accum_bytes;

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if ((u32)s >= (u32)MESH_SECTION_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (!m->section_used[s]) { return TG_OK; }

    sp = &m->span[s];
    if (sp->vertex_count == 0 || sp->triangle_count == 0) { return TG_OK; }

    if (!tg_ckd_mul_u64(sp->vertex_count, sizeof(V3), &accum_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    accum = (V3 *)tg_alloc_zero(accum_bytes);
    if (accum == NULL) { return TG_ERR_OUT_OF_MEMORY; }

    ind = mesh_indices(m);
    verts = (MeshVertex *)m->vertices.data;

    /* Standard per-triangle tangent from the intrinsic parameterisation.
     * param.x is the longitudinal coordinate (arc length along the axis, or
     * blade length), so the derived tangent follows the grain -- which is what
     * anisotropic wood and bark shading needs. */
    for (t = 0; t < sp->triangle_count; ++t) {
        u32 base = sp->first_index + t * 3u;
        u32 ia = ind[base + 0], ib = ind[base + 1], ic = ind[base + 2];
        V3 e1 = v3_sub(verts[ib].position, verts[ia].position);
        V3 e2 = v3_sub(verts[ic].position, verts[ia].position);
        f32 du1 = verts[ib].param.x - verts[ia].param.x;
        f32 dv1 = verts[ib].param.y - verts[ia].param.y;
        f32 du2 = verts[ic].param.x - verts[ia].param.x;
        f32 dv2 = verts[ic].param.y - verts[ia].param.y;
        f32 det = du1 * dv2 - du2 * dv1;
        V3 tan_;
        if (tg_absf(det) < 1e-12f) { continue; }
        tan_ = v3_scale(v3_sub(v3_scale(e1, dv2), v3_scale(e2, dv1)), 1.0f / det);
        if (!v3_finite(tan_)) { continue; }
        accum[ia - sp->first_vertex] = v3_add(accum[ia - sp->first_vertex], tan_);
        accum[ib - sp->first_vertex] = v3_add(accum[ib - sp->first_vertex], tan_);
        accum[ic - sp->first_vertex] = v3_add(accum[ic - sp->first_vertex], tan_);
    }

    for (i = 0; i < sp->vertex_count; ++i) {
        MeshVertex *v = &verts[sp->first_vertex + i];
        /* Gram-Schmidt against the final normal, then a deterministic fallback
         * so no tangent is ever zero or NaN. */
        V3 t_ = v3_reject_unit(accum[i], v->normal);
        v->tangent = v3_norm_or(t_, v3_any_perpendicular(v->normal));
    }

    tg_free(accum, accum_bytes);
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Finalisation                                                              */
/* ------------------------------------------------------------------------- */

TgResult mesh_finalize(Mesh *m) {
    const MeshVertex *v;
    u64 i, n;
    Aabb b = aabb_empty();

    TG_CHECK(m != NULL);
    if (m->finalized) { return TG_ERR_INVALID_STATE; }
    if (m->open_section != MESH_SECTION_COUNT) {
        TG_LOG_ERRORF(MESH_SUB, "mesh_finalize with section '%s' still open",
                      g_section_info[m->open_section].name);
        return TG_ERR_INVALID_STATE;
    }
    if (m->indices.count != m->tri_organ.count * 3u) {
        TG_LOG_ERRORF(MESH_SUB, "index/triangle array mismatch: %llu vs %llu*3",
                      (unsigned long long)m->indices.count,
                      (unsigned long long)m->tri_organ.count);
        return TG_ERR_INVALID_STATE;
    }

    v = mesh_vertices(m);
    n = m->vertices.count;
    for (i = 0; i < n; ++i) { b = aabb_add_point(b, v[i].position); }
    m->bounds = b;

    /* Release surplus capacity: the finalised tree is long-lived, and a 1.5x
     * growth factor can otherwise hold a third of the buffer wasted. */
    tg_array_shrink_to_fit(&m->vertices);
    tg_array_shrink_to_fit(&m->indices);
    tg_array_shrink_to_fit(&m->tri_organ);

    m->finalized = true;
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Queries                                                                   */
/* ------------------------------------------------------------------------- */

MeshSection mesh_triangle_section(const Mesh *m, u64 tri_id) {
    u32 s;
    TG_CHECK(m != NULL);
    for (s = 0; s < MESH_SECTION_COUNT; ++s) {
        if (!m->section_used[s]) { continue; }
        if (tri_id >= m->span[s].first_triangle &&
            tri_id < (u64)m->span[s].first_triangle + m->span[s].triangle_count) {
            return (MeshSection)s;
        }
    }
    return MESH_SECTION_COUNT;
}

bool mesh_get_triangle(const Mesh *m, u64 tri_id, MeshTriangle *out) {
    const u32 *ind;
    const MeshVertex *verts;
    u64 base;
    u32 k;

    TG_CHECK(m != NULL);
    if (out == NULL) { return false; }
    if (tri_id >= m->tri_organ.count) { return false; }

    ind = mesh_indices(m);
    verts = mesh_vertices(m);
    base = tri_id * 3u;

    for (k = 0; k < 3; ++k) {
        out->index[k] = ind[base + k];
        out->position[k] = verts[out->index[k]].position;
    }
    out->normal = v3_norm_or(triangle_normal_unnormalized(out->position[0],
                                                          out->position[1],
                                                          out->position[2]),
                             v3_zero());
    out->area = triangle_area(out->position[0], out->position[1], out->position[2]);
    out->organ_id = ((const u32 *)m->tri_organ.data)[tri_id];
    out->section = mesh_triangle_section(m, tri_id);
    out->material = mesh_attrib_material(verts[out->index[0]].attrib);
    return true;
}

Aabb mesh_section_bounds(const Mesh *m, MeshSection s) {
    Aabb b = aabb_empty();
    const MeshVertex *v;
    u32 i;

    TG_CHECK(m != NULL);
    if ((u32)s >= (u32)MESH_SECTION_COUNT || !m->section_used[s]) { return b; }
    v = mesh_vertices(m);
    for (i = 0; i < m->span[s].vertex_count; ++i) {
        b = aabb_add_point(b, v[m->span[s].first_vertex + i].position);
    }
    return b;
}

TgFingerprint mesh_fingerprint(const Mesh *m) {
    TgFingerprint f = tg_fp_begin(0x54524545u /* 'TREE' */);
    const MeshVertex *v;
    const u32 *ind;
    const u32 *organs;
    u64 i, n;
    u32 s;

    TG_CHECK(m != NULL);

    tg_fp_add_u64(&f, m->vertices.count);
    tg_fp_add_u64(&f, m->indices.count);
    tg_fp_add_u64(&f, m->tri_organ.count);

    v = mesh_vertices(m);
    n = m->vertices.count;
    for (i = 0; i < n; ++i) {
        tg_fp_add_f32(&f, v[i].position.x);
        tg_fp_add_f32(&f, v[i].position.y);
        tg_fp_add_f32(&f, v[i].position.z);
        tg_fp_add_f32(&f, v[i].normal.x);
        tg_fp_add_f32(&f, v[i].normal.y);
        tg_fp_add_f32(&f, v[i].normal.z);
        tg_fp_add_f32(&f, v[i].tangent.x);
        tg_fp_add_f32(&f, v[i].tangent.y);
        tg_fp_add_f32(&f, v[i].tangent.z);
        tg_fp_add_f32(&f, v[i].param.x);
        tg_fp_add_f32(&f, v[i].param.y);
        tg_fp_add_u32(&f, v[i].color);
        tg_fp_add_u32(&f, v[i].organ_id);
        tg_fp_add_u32(&f, v[i].attrib);
        tg_fp_add_f32(&f, v[i].ao);
    }

    ind = mesh_indices(m);
    tg_fp_add_u32_array(&f, ind, m->indices.count);

    organs = mesh_tri_organs(m);
    tg_fp_add_u32_array(&f, organs, m->tri_organ.count);

    for (s = 0; s < MESH_SECTION_COUNT; ++s) {
        tg_fp_add_u32(&f, m->section_used[s] ? 1u : 0u);
        tg_fp_add_u32(&f, m->span[s].first_vertex);
        tg_fp_add_u32(&f, m->span[s].vertex_count);
        tg_fp_add_u32(&f, m->span[s].first_index);
        tg_fp_add_u32(&f, m->span[s].index_count);
        tg_fp_add_u32(&f, m->span[s].first_triangle);
        tg_fp_add_u32(&f, m->span[s].triangle_count);
    }

    tg_fp_add_f32(&f, m->bounds.mn.x);
    tg_fp_add_f32(&f, m->bounds.mn.y);
    tg_fp_add_f32(&f, m->bounds.mn.z);
    tg_fp_add_f32(&f, m->bounds.mx.x);
    tg_fp_add_f32(&f, m->bounds.mx.y);
    tg_fp_add_f32(&f, m->bounds.mx.z);
    return f;
}

MeshStats mesh_stats(const Mesh *m) {
    MeshStats st;
    u32 s;

    TG_CHECK(m != NULL);
    memset(&st, 0, sizeof st);
    st.vertices = m->vertices.count;
    st.triangles = m->tri_organ.count;
    for (s = 0; s < MESH_SECTION_COUNT; ++s) {
        st.section_triangles[s] = m->section_used[s]
                                    ? m->span[s].triangle_count : 0u;
    }
    st.cpu_bytes = m->vertices.capacity * sizeof(MeshVertex)
                 + m->indices.capacity * sizeof(u32)
                 + m->tri_organ.capacity * sizeof(u32);
    /* GPU side holds the interleaved vertex buffer and the index buffer. The
     * per-triangle organ array stays CPU-side unless the picking shader needs
     * it, so it is not counted here. Labelled an estimate because driver
     * padding and heap granularity are not modelled. */
    st.gpu_bytes_estimate = m->vertices.count * sizeof(MeshVertex)
                          + m->indices.count * sizeof(u32);
    st.bounds = m->bounds;
    return st;
}
