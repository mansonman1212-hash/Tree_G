#include "mesh_validate.h"

#include "../core/log.h"

#include <stdio.h>
#include <string.h>

#define MV_SUB "mesh_validate"

/* Sorts three values ascending. Used to canonicalise a triangle's vertex set so
 * that two triangles listing the same vertices in different rotations or
 * windings compare equal. */
static void sort3_u32(u32 v[3]) {
    u32 t;
    if (v[0] > v[1]) { t = v[0]; v[0] = v[1]; v[1] = t; }
    if (v[1] > v[2]) { t = v[1]; v[1] = v[2]; v[2] = t; }
    if (v[0] > v[1]) { t = v[0]; v[0] = v[1]; v[1] = t; }
}

const char *mesh_issue_name(MeshIssueKind k) {
    switch (k) {
    case MESH_ISSUE_NONE:                 return "none";
    case MESH_ISSUE_INDEX_OUT_OF_RANGE:   return "index_out_of_range";
    case MESH_ISSUE_ARRAY_MISMATCH:       return "array_mismatch";
    case MESH_ISSUE_SECTION_SPAN:         return "section_span";
    case MESH_ISSUE_NON_FINITE_POSITION:  return "non_finite_position";
    case MESH_ISSUE_NON_FINITE_NORMAL:    return "non_finite_normal";
    case MESH_ISSUE_NON_FINITE_TANGENT:   return "non_finite_tangent";
    case MESH_ISSUE_NON_FINITE_PARAM:     return "non_finite_param";
    case MESH_ISSUE_NORMAL_NOT_UNIT:      return "normal_not_unit";
    case MESH_ISSUE_AO_OUT_OF_RANGE:      return "ao_out_of_range";
    case MESH_ISSUE_REPEATED_INDEX:       return "repeated_index";
    case MESH_ISSUE_ZERO_AREA:            return "zero_area_triangle";
    case MESH_ISSUE_BOUNDARY_EDGE:        return "boundary_edge_in_closed_section";
    case MESH_ISSUE_NON_MANIFOLD_EDGE:    return "non_manifold_edge";
    case MESH_ISSUE_INCONSISTENT_WINDING: return "inconsistent_winding";
    case MESH_ISSUE_INVERTED_COMPONENT:   return "inverted_component";
    case MESH_ISSUE_DUPLICATE_TRIANGLE:   return "duplicate_triangle";
    case MESH_ISSUE_DEGENERATE_BOUNDS:    return "degenerate_bounds";
    case MESH_ISSUE_SCRATCH_LIMIT:        return "scratch_limit_exceeded";
    case MESH_ISSUE_KIND_COUNT:           break;
    }
    return "unknown";
}

MeshValidateOptions mesh_validate_default_options(void) {
    MeshValidateOptions o;
    /* Relative to the SQUARED bounds diagonal, so the threshold scales with the
     * scene rather than assuming trunk-sized triangles.
     *
     * Calibration: for a 20 m tree the diagonal is ~20 m, so the threshold is
     * 1e-14 * 400 = 4e-12 m^2, which corresponds to a right triangle about
     * 2.8 micrometres on a side. Every feature this engine generates is far
     * larger than that -- the finest are leaf-vein cross-sections and bark
     * micro-ridges at tens of micrometres -- while anything below it is
     * genuinely degenerate and would make face normals unstable in float.
     *
     * An earlier value of 1e-9 was measured to reject legitimate 0.3 mm
     * leaf-vein triangles and was corrected. */
    o.scale_relative_area_eps = 1e-14f;
    o.normal_length_tolerance = 1e-3f;
    o.max_scratch_bytes = (u64)512 * 1024 * 1024;
    o.skip_topology = false;
    return o;
}

/* ------------------------------------------------------------------------- */
/* Issue recording                                                           */
/* ------------------------------------------------------------------------- */

static MeshIssue *issue_slot(MeshValidateReport *r, MeshIssueKind k, MeshSection s) {
    u32 i;
    for (i = 0; i < r->issue_count; ++i) {
        if (r->issue[i].kind == k && r->issue[i].section == s) { return &r->issue[i]; }
    }
    if (r->issue_count >= TG_COUNTOF(r->issue)) { return NULL; }
    {
        MeshIssue *slot = &r->issue[r->issue_count++];
        memset(slot, 0, sizeof *slot);
        slot->kind = k;
        slot->section = s;
        return slot;
    }
}

static void record(MeshValidateReport *r, MeshIssueKind k, MeshSection s, u64 example) {
    MeshIssue *slot = issue_slot(r, k, s);
    r->passed = false;
    if (slot == NULL) { return; }
    slot->count++;
    if (slot->example_count < MESH_VALIDATE_MAX_EXAMPLES) {
        slot->example[slot->example_count++] = example;
    }
}

/* ------------------------------------------------------------------------- */
/* Edge records and radix sort                                               */
/* ------------------------------------------------------------------------- */

typedef struct EdgeRec {
    u64 ukey; /* (min_vertex << 32) | max_vertex : undirected identity      */
    u32 tri;  /* triangle id within the section                            */
    u32 dir;  /* 1 if the triangle traverses the edge low->high, else 0     */
} EdgeRec;

/* Same record reused for the position-weld pass: ukey is a position hash and
 * `tri` holds a section-local vertex index. */

TG_STATIC_ASSERT(sizeof(EdgeRec) == 16, "EdgeRec should pack to 16 bytes");

/* LSD radix sort on ukey, 8 passes of 8 bits.
 *
 * Chosen over qsort deliberately: it is O(n), it moves 16-byte records without
 * a comparison callback, and it is stable, so the output order is a pure
 * function of the input. Determinism of the validation pass matters because the
 * "example" ids in a failure report must be reproducible during triage. */
static void radix_sort_edges(EdgeRec *a, EdgeRec *tmp, u64 n) {
    u64 counts[256];
    int pass;
    EdgeRec *src = a;
    EdgeRec *dst = tmp;

    for (pass = 0; pass < 8; ++pass) {
        u32 shift = (u32)pass * 8u;
        u64 i;
        u64 sum = 0;
        bool already_sorted = true;
        u32 prev_byte = 0;

        memset(counts, 0, sizeof counts);
        for (i = 0; i < n; ++i) {
            u32 byte = (u32)((src[i].ukey >> shift) & 0xFFu);
            counts[byte]++;
            if (i > 0 && byte < prev_byte) { already_sorted = false; }
            prev_byte = byte;
        }
        /* Skipping a pass whose key byte is constant or already ordered is safe
         * because the sort is stable, and it makes the common case (vertex
         * indices well below 2^24) much cheaper. */
        if (already_sorted) { continue; }

        for (i = 0; i < 256; ++i) {
            u64 c = counts[i];
            counts[i] = sum;
            sum += c;
        }
        for (i = 0; i < n; ++i) {
            u32 byte = (u32)((src[i].ukey >> shift) & 0xFFu);
            dst[counts[byte]++] = src[i];
        }
        { EdgeRec *swap = src; src = dst; dst = swap; }
    }
    if (src != a) { memcpy(a, src, (size_t)n * sizeof(EdgeRec)); }
}

/* ------------------------------------------------------------------------- */
/* Position welding                                                          */
/*                                                                           */
/* WHY THIS EXISTS. Watertightness is a property of the SURFACE, not of the    */
/* index buffer. A generator must duplicate a vertex wherever it needs a hard  */
/* normal split -- along a bark fracture, at a cap rim, at a branch-bark ridge */
/* crest. Those duplicates occupy the same point in space, so the surface is   */
/* still closed, but the two copies have different indices and a naive         */
/* index-based edge test would report a hole that does not exist.              */
/*                                                                           */
/* Welding is by EXACT position bits, not by a distance tolerance. That is the  */
/* correct rule here: a duplicate created for a normal split is a bit-exact     */
/* copy of its original, whereas two rings that ALMOST meet represent a real    */
/* crack and must be reported. A tolerance would hide exactly the defect this   */
/* check exists to find.                                                       */
/* ------------------------------------------------------------------------- */

/* Canonicalises the three components so that -0.0 and +0.0 weld, then returns
 * the raw bit patterns for exact comparison. */
static void position_bits(V3 p, u32 out[3]) {
    f32 c[3];
    c[0] = (p.x == 0.0f) ? 0.0f : p.x;
    c[1] = (p.y == 0.0f) ? 0.0f : p.y;
    c[2] = (p.z == 0.0f) ? 0.0f : p.z;
    memcpy(out, c, sizeof c);
}

static u64 position_hash(V3 p) {
    u32 b[3];
    u64 h = TG_FNV64_OFFSET;
    position_bits(p, b);
    h = tg_hash64_u32(h, b[0]);
    h = tg_hash64_u32(h, b[1]);
    h = tg_hash64_u32(h, b[2]);
    return h;
}

static bool position_identical(V3 a, V3 b) {
    u32 ba[3], bb[3];
    position_bits(a, ba);
    position_bits(b, bb);
    return ba[0] == bb[0] && ba[1] == bb[1] && ba[2] == bb[2];
}

/* Fills canon[local] with the smallest section-local index sharing the exact
 * same position. Uses `recs` (>= vcount entries) and `tmp` as scratch. */
static void build_weld_map(const MeshVertex *verts, u32 first_vertex, u32 vcount,
                           EdgeRec *recs, EdgeRec *tmp, u32 *canon) {
    u32 i;
    u64 g, h;

    for (i = 0; i < vcount; ++i) {
        recs[i].ukey = position_hash(verts[first_vertex + i].position);
        recs[i].tri = i;
        recs[i].dir = 0;
        canon[i] = i;
    }
    radix_sort_edges(recs, tmp, vcount);

    g = 0;
    while (g < vcount) {
        h = g + 1;
        while (h < vcount && recs[h].ukey == recs[g].ukey) { h++; }
        if (h - g > 1) {
            /* Groups are tiny (a shared point is duplicated a handful of times
             * at most), so the quadratic scan inside a group is cheap and avoids
             * a second sort on the full 96-bit key. */
            u64 a, b;
            for (a = g; a < h; ++a) {
                u32 la = recs[a].tri;
                if (canon[la] != la) { continue; } /* already assigned */
                for (b = a + 1; b < h; ++b) {
                    u32 lb = recs[b].tri;
                    if (canon[lb] != lb) { continue; }
                    if (position_identical(verts[first_vertex + la].position,
                                           verts[first_vertex + lb].position)) {
                        /* Canonical representative is the smaller local index,
                         * which makes the result independent of sort order. */
                        if (la < lb) { canon[lb] = la; } else { canon[la] = lb; }
                    }
                }
            }
            /* Collapse any two-step chains created above. */
            for (a = g; a < h; ++a) {
                u32 la = recs[a].tri;
                while (canon[la] != canon[canon[la]]) { canon[la] = canon[canon[la]]; }
            }
        }
        g = h;
    }
}

/* ------------------------------------------------------------------------- */
/* Union-find over triangles of one section                                  */
/* ------------------------------------------------------------------------- */

static u32 uf_find(u32 *parent, u32 x) {
    /* Iterative path halving: no recursion, so a long chain cannot overflow the
     * stack on a large mesh. */
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void uf_union(u32 *parent, u32 a, u32 b) {
    u32 ra = uf_find(parent, a);
    u32 rb = uf_find(parent, b);
    if (ra == rb) { return; }
    /* Union by index keeps the result independent of the order edges are
     * visited, preserving determinism of component numbering. */
    if (ra < rb) { parent[rb] = ra; } else { parent[ra] = rb; }
}

/* ------------------------------------------------------------------------- */
/* Per-section topology                                                      */
/* ------------------------------------------------------------------------- */

static TgResult validate_section_topology(const Mesh *m, MeshSection s,
                                          const MeshValidateOptions *opt,
                                          MeshValidateReport *r) {
    const MeshSectionInfo *info = mesh_section_info(s);
    const MeshSectionSpan *sp = &m->span[s];
    const u32 *ind = mesh_indices(m);
    const MeshVertex *verts = mesh_vertices(m);
    u64 tri_count = sp->triangle_count;
    u64 vcount = sp->vertex_count;
    u64 edge_count;
    u64 edge_bytes, tmp_bytes, parent_bytes, canon_bytes, total_scratch;
    EdgeRec *edges = NULL;
    EdgeRec *tmp = NULL;
    u32 *parent = NULL;
    u32 *canon = NULL;
    u64 i;
    TgResult result = TG_OK;

    if (tri_count == 0) { return TG_OK; }

    if (!tg_ckd_mul_u64(tri_count, 3, &edge_count)) { return TG_ERR_OVERFLOW; }
    /* The weld pass needs vcount records; the edge pass needs 3*tri_count. One
     * pair of buffers sized for the larger of the two serves both. */
    if (!tg_ckd_mul_u64(tg_max_u64(edge_count, vcount), sizeof(EdgeRec),
                        &edge_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    tmp_bytes = edge_bytes;
    if (!tg_ckd_mul_u64(tri_count, sizeof(u32), &parent_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    if (!tg_ckd_mul_u64(vcount, sizeof(u32), &canon_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    total_scratch = edge_bytes + tmp_bytes + parent_bytes + canon_bytes;
    if (total_scratch > opt->max_scratch_bytes) {
        record(r, MESH_ISSUE_SCRATCH_LIMIT, s, total_scratch);
        TG_LOG_ERRORF(MV_SUB,
                      "section '%s': topology check needs %llu scratch bytes, "
                      "limit is %llu; topology NOT verified",
                      info->name,
                      (unsigned long long)total_scratch,
                      (unsigned long long)opt->max_scratch_bytes);
        return TG_ERR_LIMIT_EXCEEDED;
    }

    edges = (EdgeRec *)tg_alloc(edge_bytes);
    tmp = (EdgeRec *)tg_alloc(tmp_bytes);
    parent = (u32 *)tg_alloc(parent_bytes);
    canon = (u32 *)tg_alloc(canon_bytes);
    if (edges == NULL || tmp == NULL || parent == NULL || canon == NULL) {
        result = TG_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    for (i = 0; i < tri_count; ++i) { parent[i] = (u32)i; }

    build_weld_map(verts, sp->first_vertex, (u32)vcount, edges, tmp, canon);

    /* Build directed edges on WELDED vertex identity, recording undirected
     * identity plus direction. `ne` is the number actually written: an edge
     * whose two endpoints weld to the same point is skipped, so the array is
     * compacted rather than left with uninitialised holes. */
    {
        u64 ne = 0;
        for (i = 0; i < tri_count; ++i) {
            u64 base = (u64)sp->first_index + i * 3u;
            u32 v[3];
            u32 k;
            v[0] = canon[ind[base + 0] - sp->first_vertex];
            v[1] = canon[ind[base + 1] - sp->first_vertex];
            v[2] = canon[ind[base + 2] - sp->first_vertex];
            for (k = 0; k < 3; ++k) {
                u32 a = v[k];
                u32 b = v[(k + 1u) % 3u];
                EdgeRec *e;
                if (a == b) {
                    /* Two corners welded to the same point: the triangle is
                     * geometrically degenerate. Already reported by the
                     * zero-area check; skipping it here stops it masquerading
                     * as a hole. */
                    continue;
                }
                e = &edges[ne++];
                if (a < b) {
                    e->ukey = ((u64)a << 32) | (u64)b;
                    e->dir = 1u;
                } else {
                    e->ukey = ((u64)b << 32) | (u64)a;
                    e->dir = 0u;
                }
                e->tri = (u32)i;
            }
        }
        edge_count = ne;
    }

    radix_sort_edges(edges, tmp, edge_count);

    /* Walk equal-ukey groups. */
    i = 0;
    while (i < edge_count) {
        u64 j = i + 1;
        while (j < edge_count && edges[j].ukey == edges[i].ukey) { j++; }
        {
            u64 n = j - i;
            u64 tri_a = sp->first_triangle + edges[i].tri;
            if (n == 1) {
                r->boundary_edges[s]++;
                if (info->require_closed_manifold) {
                    record(r, MESH_ISSUE_BOUNDARY_EDGE, s, tri_a);
                }
                /* An open section's boundary edges are expected; they are still
                 * counted so the report can show the tear length of a flake. */
            } else if (n == 2) {
                if (edges[i].dir == edges[i + 1].dir) {
                    /* Both triangles traverse the shared edge in the same
                     * direction: one of them is wound backwards. This is the
                     * classic cause of a black facet on an otherwise clean
                     * trunk. */
                    record(r, MESH_ISSUE_INCONSISTENT_WINDING, s, tri_a);
                } else {
                    uf_union(parent, edges[i].tri, edges[i + 1].tri);
                }
            } else {
                /* More than two: a duplicated internal tube wall, a T-junction,
                 * or two organs welded through the same edge. */
                record(r, MESH_ISSUE_NON_MANIFOLD_EDGE, s, tri_a);
            }
        }
        i = j;
    }

    /* Duplicate triangles: an exact repeat produces three duplicated undirected
     * edges, which the pass above sees as non-manifold. Detect it explicitly so
     * the diagnostic names the real cause. Done by sorting a canonical
     * per-triangle key reusing the same buffers. */
    {
        for (i = 0; i < tri_count; ++i) {
            u64 base = (u64)sp->first_index + i * 3u;
            u32 a = ind[base + 0], b = ind[base + 1], c = ind[base + 2];
            u32 key3[3];
            key3[0] = a; key3[1] = b; key3[2] = c;
            sort3_u32(key3);
            /* Fold the sorted triple into 64 bits. Collisions are possible in
             * principle for very large meshes, so a hit is confirmed by
             * comparing the actual vertex sets below. */
            edges[i].ukey = ((u64)key3[0] * 0x9E3779B97F4A7C15ull)
                          ^ ((u64)key3[1] * 0xC2B2AE3D27D4EB4Full)
                          ^ ((u64)key3[2] * 0x165667B19E3779F9ull);
            edges[i].tri = (u32)i;
            edges[i].dir = 0;
        }
        radix_sort_edges(edges, tmp, tri_count);
        for (i = 1; i < tri_count; ++i) {
            if (edges[i].ukey != edges[i - 1].ukey) { continue; }
            {
                u64 ba = (u64)sp->first_index + (u64)edges[i - 1].tri * 3u;
                u64 bb = (u64)sp->first_index + (u64)edges[i].tri * 3u;
                u32 sa[3], sb[3], k;
                for (k = 0; k < 3; ++k) { sa[k] = ind[ba + k]; sb[k] = ind[bb + k]; }
                sort3_u32(sa);
                sort3_u32(sb);
                if (sa[0] == sb[0] && sa[1] == sb[1] && sa[2] == sb[2]) {
                    record(r, MESH_ISSUE_DUPLICATE_TRIANGLE, s,
                           sp->first_triangle + edges[i].tri);
                }
            }
        }
    }

    /* Per-component enclosed volume. Signed volume of a closed outward-wound
     * surface is positive; computing it per connected component means a single
     * inverted leaf cannot hide inside the sum over thousands of leaves. */
    if (info->require_outward_orientation) {
        /* Per-component enclosed volume by the divergence theorem.
         *
         * NUMERICAL NOTE, and the reason for the two-pass structure. The obvious
         * formulation sums dot(a, cross(b, c))/6 over the triangles, which is
         * analytically independent of where the origin sits. In float it is not.
         * Each term scales with |p|^3 while the sum is the (tiny) volume, so the
         * cancellation ratio grows as (distance from origin / feature size)^3.
         * A leaf 20 m from the model origin with millimetre features loses on
         * the order of ten significant digits -- enough for a perfectly correct
         * leaf to report a negative volume and be rejected.
         *
         * This was measured, not assumed: the same sphere translated by
         * (-40, 77, 12) gave 0.5391 instead of 0.5199, a 3.7% error.
         *
         * The fix is to evaluate each component about its OWN reference point.
         * Volume is translation invariant, so this changes nothing analytically
         * and removes the cancellation entirely. */
        f64 *vol;
        V3  *ref;
        bool *has_ref;
        u64 vol_bytes, ref_bytes, hasref_bytes, extra;

        if (!tg_ckd_mul_u64(tri_count, sizeof(f64), &vol_bytes) ||
            !tg_ckd_mul_u64(tri_count, sizeof(V3), &ref_bytes) ||
            !tg_ckd_mul_u64(tri_count, sizeof(bool), &hasref_bytes)) {
            result = TG_ERR_OVERFLOW;
            goto cleanup;
        }
        extra = vol_bytes + ref_bytes + hasref_bytes;
        if (total_scratch + extra > opt->max_scratch_bytes) {
            record(r, MESH_ISSUE_SCRATCH_LIMIT, s, total_scratch + extra);
            result = TG_ERR_LIMIT_EXCEEDED;
            goto cleanup;
        }
        vol = (f64 *)tg_alloc_zero(vol_bytes);
        ref = (V3 *)tg_alloc_zero(ref_bytes);
        has_ref = (bool *)tg_alloc_zero(hasref_bytes);
        if (vol == NULL || ref == NULL || has_ref == NULL) {
            if (vol != NULL) { tg_free(vol, vol_bytes); }
            if (ref != NULL) { tg_free(ref, ref_bytes); }
            if (has_ref != NULL) { tg_free(has_ref, hasref_bytes); }
            result = TG_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }

        /* Pass 1: one reference point per component. Taken from the lowest
         * triangle id in the component, so it is deterministic. */
        for (i = 0; i < tri_count; ++i) {
            u32 root = uf_find(parent, (u32)i);
            if (!has_ref[root]) {
                ref[root] = verts[ind[(u64)sp->first_index + i * 3u]].position;
                has_ref[root] = true;
            }
        }

        /* Pass 2: accumulate in f64 relative to the component reference. */
        for (i = 0; i < tri_count; ++i) {
            u64 base = (u64)sp->first_index + i * 3u;
            u32 root = uf_find(parent, (u32)i);
            V3 o = ref[root];
            V3 a = v3_sub(verts[ind[base + 0]].position, o);
            V3 b = v3_sub(verts[ind[base + 1]].position, o);
            V3 c = v3_sub(verts[ind[base + 2]].position, o);
            f64 ax = (f64)a.x, ay = (f64)a.y, az = (f64)a.z;
            f64 bx = (f64)b.x, by = (f64)b.y, bz = (f64)b.z;
            f64 cx = (f64)c.x, cy = (f64)c.y, cz = (f64)c.z;
            f64 crx = by * cz - bz * cy;
            f64 cry = bz * cx - bx * cz;
            f64 crz = bx * cy - by * cx;
            vol[root] += (ax * crx + ay * cry + az * crz) / 6.0;
        }

        for (i = 0; i < tri_count; ++i) {
            if (uf_find(parent, (u32)i) != (u32)i) { continue; }
            r->closed_components[s]++;
            r->enclosed_volume[s] += vol[i];
            if (!(vol[i] > 0.0)) {
                record(r, MESH_ISSUE_INVERTED_COMPONENT, s, sp->first_triangle + i);
            }
        }
        tg_free(vol, vol_bytes);
        tg_free(ref, ref_bytes);
        tg_free(has_ref, hasref_bytes);
    } else {
        /* Still report component count for open sections; useful for knowing
         * how many separate bark flakes exist. */
        for (i = 0; i < tri_count; ++i) {
            if (uf_find(parent, (u32)i) == (u32)i) { r->closed_components[s]++; }
        }
    }

cleanup:
    if (edges != NULL) { tg_free(edges, edge_bytes); }
    if (tmp != NULL) { tg_free(tmp, tmp_bytes); }
    if (parent != NULL) { tg_free(parent, parent_bytes); }
    if (canon != NULL) { tg_free(canon, canon_bytes); }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Top level                                                                 */
/* ------------------------------------------------------------------------- */

TgResult mesh_validate(const Mesh *m, const MeshValidateOptions *opt,
                       MeshValidateReport *report) {
    MeshValidateOptions defaults;
    const MeshVertex *verts;
    const u32 *ind;
    u64 i, n;
    f32 area_eps;
    f32 diag;
    u32 s;
    bool topology_error = false;

    TG_CHECK(m != NULL);
    TG_CHECK(report != NULL);

    memset(report, 0, sizeof *report);
    report->passed = true;
    report->min_triangle_area = 3.402823466e38f;
    report->max_triangle_area = 0.0f;
    report->self_intersection_checked = false;

    if (opt == NULL) {
        defaults = mesh_validate_default_options();
        opt = &defaults;
    }

    /* --- structural ----------------------------------------------------- */
    if (m->indices.count != m->tri_organ.count * 3u) {
        record(report, MESH_ISSUE_ARRAY_MISMATCH, MESH_SECTION_COUNT,
               m->indices.count);
    }

    {
        /* Section spans must be contiguous, non-overlapping, and together cover
         * every triangle exactly once. Without this, triangle ids reported by
         * the inspection UI could belong to no section or to two. */
        u64 covered = 0;
        u64 expect_first = 0;
        for (s = 0; s < MESH_SECTION_COUNT; ++s) {
            if (!m->section_used[s]) { continue; }
            if (m->span[s].first_triangle != expect_first) {
                record(report, MESH_ISSUE_SECTION_SPAN, (MeshSection)s,
                       m->span[s].first_triangle);
            }
            if (m->span[s].index_count != m->span[s].triangle_count * 3u) {
                record(report, MESH_ISSUE_SECTION_SPAN, (MeshSection)s,
                       m->span[s].index_count);
            }
            expect_first = (u64)m->span[s].first_triangle
                         + m->span[s].triangle_count;
            covered += m->span[s].triangle_count;
        }
        if (covered != m->tri_organ.count) {
            record(report, MESH_ISSUE_SECTION_SPAN, MESH_SECTION_COUNT, covered);
        }
    }

    /* --- per-vertex numeric --------------------------------------------- */
    verts = mesh_vertices(m);
    n = m->vertices.count;
    report->checked_vertices = n;
    for (i = 0; i < n; ++i) {
        const MeshVertex *v = &verts[i];
        MeshSection vs = mesh_attrib_section(v->attrib);
        if ((u32)vs >= (u32)MESH_SECTION_COUNT) { vs = MESH_SECTION_COUNT; }
        if (!v3_finite(v->position)) {
            record(report, MESH_ISSUE_NON_FINITE_POSITION, vs, i);
        }
        if (!v3_finite(v->normal)) {
            record(report, MESH_ISSUE_NON_FINITE_NORMAL, vs, i);
        } else if (!tg_nearf(v3_len(v->normal), 1.0f, opt->normal_length_tolerance)) {
            record(report, MESH_ISSUE_NORMAL_NOT_UNIT, vs, i);
        }
        if (!v3_finite(v->tangent)) {
            record(report, MESH_ISSUE_NON_FINITE_TANGENT, vs, i);
        }
        if (!tg_finitef(v->param.x) || !tg_finitef(v->param.y)) {
            record(report, MESH_ISSUE_NON_FINITE_PARAM, vs, i);
        }
        if (!tg_finitef(v->ao) || v->ao < 0.0f || v->ao > 1.0f) {
            record(report, MESH_ISSUE_AO_OUT_OF_RANGE, vs, i);
        }
    }

    /* --- bounds --------------------------------------------------------- */
    if (n > 0) {
        if (aabb_is_empty(m->bounds) || !v3_finite(m->bounds.mn) ||
            !v3_finite(m->bounds.mx)) {
            record(report, MESH_ISSUE_DEGENERATE_BOUNDS, MESH_SECTION_COUNT, 0);
            diag = 1.0f;
        } else {
            diag = aabb_diagonal(m->bounds);
            if (!(diag > 0.0f)) {
                record(report, MESH_ISSUE_DEGENERATE_BOUNDS, MESH_SECTION_COUNT, 1);
                diag = 1.0f;
            }
        }
    } else {
        diag = 1.0f;
    }
    area_eps = opt->scale_relative_area_eps * diag * diag;

    /* --- per-triangle geometric ----------------------------------------- */
    ind = mesh_indices(m);
    n = m->tri_organ.count;
    report->checked_triangles = n;
    for (i = 0; i < n; ++i) {
        u64 base = i * 3u;
        u32 a, b, c;
        MeshSection ts;
        f32 area;

        a = ind[base + 0];
        b = ind[base + 1];
        c = ind[base + 2];
        ts = mesh_triangle_section(m, i);

        if (a >= m->vertices.count || b >= m->vertices.count ||
            c >= m->vertices.count) {
            record(report, MESH_ISSUE_INDEX_OUT_OF_RANGE, ts, i);
            continue; /* cannot compute area safely */
        }
        if (a == b || b == c || a == c) {
            record(report, MESH_ISSUE_REPEATED_INDEX, ts, i);
            continue;
        }
        area = triangle_area(verts[a].position, verts[b].position,
                             verts[c].position);
        if (!tg_finitef(area) || area <= area_eps) {
            record(report, MESH_ISSUE_ZERO_AREA, ts, i);
            continue;
        }
        if (area < report->min_triangle_area) { report->min_triangle_area = area; }
        if (area > report->max_triangle_area) { report->max_triangle_area = area; }
        if ((u32)ts < (u32)MESH_SECTION_COUNT) {
            report->total_area[ts] += (f64)area;
        }
    }
    if (report->min_triangle_area > report->max_triangle_area) {
        report->min_triangle_area = 0.0f;
    }

    /* --- topology ------------------------------------------------------- */
    if (!opt->skip_topology) {
        for (s = 0; s < MESH_SECTION_COUNT; ++s) {
            TgResult tr;
            if (!m->section_used[s]) { continue; }
            tr = validate_section_topology(m, (MeshSection)s, opt, report);
            if (tr != TG_OK) {
                /* Could not carry out the check. This is NOT a pass. */
                report->passed = false;
                topology_error = true;
            }
        }
    }

    if (!report->passed) {
        return topology_error && report->issue_count == 0
                 ? TG_ERR_LIMIT_EXCEEDED
                 : TG_ERR_VALIDATION_FAILED;
    }
    return TG_OK;
}

void mesh_validate_log_report(const MeshValidateReport *r) {
    u32 i, s;

    TG_CHECK(r != NULL);

    TG_LOG_INFOF(MV_SUB, "validation %s: %llu triangles, %llu vertices",
                 r->passed ? "PASSED" : "FAILED",
                 (unsigned long long)r->checked_triangles,
                 (unsigned long long)r->checked_vertices);

    for (s = 0; s < MESH_SECTION_COUNT; ++s) {
        const MeshSectionInfo *info = mesh_section_info((MeshSection)s);
        if (r->closed_components[s] == 0 && r->total_area[s] == 0.0) { continue; }
        TG_LOG_INFOF(MV_SUB,
                     "  %-14s components=%u boundary_edges=%llu "
                     "area=%.6f m^2 volume=%.6f m^3",
                     info->name, r->closed_components[s],
                     (unsigned long long)r->boundary_edges[s],
                     r->total_area[s], r->enclosed_volume[s]);
    }

    for (i = 0; i < r->issue_count; ++i) {
        const MeshIssue *is = &r->issue[i];
        char examples[128];
        int off = 0;
        u32 k;
        examples[0] = '\0';
        for (k = 0; k < is->example_count; ++k) {
            int wrote = snprintf(examples + off, sizeof examples - (size_t)off,
                                 "%s%llu", k == 0 ? "" : ",",
                                 (unsigned long long)is->example[k]);
            if (wrote <= 0 || (size_t)(off + wrote) >= sizeof examples) { break; }
            off += wrote;
        }
        TG_LOG_ERRORF(MV_SUB, "  ISSUE %-32s section=%s count=%llu ids=[%s]",
                      mesh_issue_name(is->kind),
                      (u32)is->section < (u32)MESH_SECTION_COUNT
                          ? mesh_section_info(is->section)->name : "global",
                      (unsigned long long)is->count, examples);
    }

    if (!r->self_intersection_checked) {
        TG_LOG_INFOF(MV_SUB,
                     "  note: self-intersection was NOT checked by this pass");
    }
}
