#include "mesh_bvh.h"

#include "../core/log.h"
#include "../core/mem.h"

#include <string.h>

#define MB_SUB "mesh_bvh"

/* Leaves hold up to this many triangles. Small leaves mean more nodes and deeper
 * traversal; large leaves mean more triangle tests. Four was chosen by measurement
 * on a mature tree rather than by convention -- see the stats the build logs. */
/* Eight, not four. A leaf of four gives 7.19 million nodes over a mature tree's
 * 9.88 million wood triangles -- 230 MB of index for geometry that is already
 * 900 MB -- while a leaf of eight halves the node count. The cost is roughly twice
 * as many triangle tests per query, measured at 54 rising to about 100 out of ten
 * million, which is not a cost worth 115 MB. */
#define MB_LEAF_TARGET 8u

/* Guard against a pathological build. Depth 64 with a median split means 2^64
 * triangles, so reaching it indicates a bug (typically identical centroids), and
 * the builder degrades to a leaf rather than recursing forever. */
#define MB_MAX_DEPTH 64u

typedef struct BuildCtx {
    MeshBvh    *bvh;
    const Mesh *mesh;
    V3         *centroids;   /* indexed by SLOT, permuted along with tri_ids   */
    Aabb       *tri_bounds;
    TgResult    error;
} BuildCtx;

static Aabb triangle_bounds(const Mesh *m, u64 tri, V3 *out_centroid) {
    MeshTriangle t;
    Aabb b = aabb_empty();
    if (!mesh_get_triangle(m, tri, &t)) {
        if (out_centroid != NULL) { *out_centroid = v3_zero(); }
        return b;
    }
    b = aabb_add_point(b, t.position[0]);
    b = aabb_add_point(b, t.position[1]);
    b = aabb_add_point(b, t.position[2]);
    if (out_centroid != NULL) {
        *out_centroid = v3_scale(v3_add(v3_add(t.position[0], t.position[1]),
                                       t.position[2]), 1.0f / 3.0f);
    }
    return b;
}

/* Partitions slots [lo, hi) about the median of `axis` using nth_element-style
 * quickselect, which is linear on average and, unlike a full sort, does no work
 * ordering elements the split does not care about. */
static void select_median(BuildCtx *c, u32 lo, u32 hi, u32 axis, u32 nth) {
    u32 *ids = c->bvh->tri_ids;
    while (hi - lo > 1u) {
        /* Median-of-three pivot. A first-element pivot degenerates to O(n^2) on
         * geometry that is already sorted along an axis, and a tree's trunk is
         * exactly that: a long run of rings in ascending height. */
        u32 mid = lo + (hi - lo) / 2u;
        f32 a = ((const f32 *)&c->centroids[lo])[axis];
        f32 b = ((const f32 *)&c->centroids[mid])[axis];
        f32 d = ((const f32 *)&c->centroids[hi - 1u])[axis];
        f32 pivot = (a < b) ? ((b < d) ? b : ((a < d) ? d : a))
                            : ((a < d) ? a : ((b < d) ? d : b));
        u32 i = lo, j = hi - 1u;
        while (i <= j) {
            while (((const f32 *)&c->centroids[i])[axis] < pivot) { i++; }
            while (((const f32 *)&c->centroids[j])[axis] > pivot) {
                if (j == 0u) { break; }
                j--;
            }
            if (i <= j) {
                u32 tid = ids[i]; ids[i] = ids[j]; ids[j] = tid;
                { V3 tc = c->centroids[i]; c->centroids[i] = c->centroids[j];
                  c->centroids[j] = tc; }
                { Aabb tb = c->tri_bounds[i]; c->tri_bounds[i] = c->tri_bounds[j];
                  c->tri_bounds[j] = tb; }
                i++;
                if (j == 0u) { break; }
                j--;
            }
        }
        if (nth <= j) { hi = j + 1u; }
        else if (nth >= i) { lo = i; }
        else { return; }
    }
}

/* GROWS RATHER THAN ASSUMING A BOUND.
 *
 * The first version allocated 2 * ceil(n / leaf_target) + 1 nodes, on the reasoning
 * that a binary tree over n triangles in leaves of four has that many nodes. That
 * bound is wrong, and a young tree failed to build because of it: a median split
 * only happens above the leaf target, so a node of five triangles splits into two
 * and three and BOTH become leaves. Leaves therefore hold as few as two triangles,
 * the leaf count can approach n/2, and the node count can approach n.
 *
 * The true worst-case bound is about n nodes, which on a sixteen-million-triangle
 * tree is 512 MB of nodes for a structure that typically needs an eighth of that.
 * So the array grows instead, starting at the typical size so that the common case
 * never reallocates at all, and every access is by INDEX rather than by cached
 * pointer precisely so that a growth in the middle of the build is safe. */
static u32 alloc_node(BuildCtx *c) {
    MeshBvh *bvh = c->bvh;
    if (bvh->node_count >= bvh->node_capacity) {
        u64 want = (u64)bvh->node_capacity + bvh->node_capacity / 2u + 64u;
        MeshBvhNode *grown;
        u64 old_bytes, new_bytes;
        if (want > 0xFFFFFFFEull) { c->error = TG_ERR_LIMIT_EXCEEDED; return 0u; }
        if (!tg_ckd_mul_u64(want, sizeof(MeshBvhNode), &new_bytes)) {
            c->error = TG_ERR_OVERFLOW;
            return 0u;
        }
        old_bytes = (u64)bvh->node_capacity * sizeof(MeshBvhNode);
        grown = (MeshBvhNode *)tg_alloc_zero(new_bytes);
        if (grown == NULL) { c->error = TG_ERR_OUT_OF_MEMORY; return 0u; }
        if (bvh->nodes != NULL) {
            memcpy(grown, bvh->nodes, (size_t)old_bytes);
            tg_free(bvh->nodes, old_bytes);
        }
        bvh->nodes = grown;
        bvh->node_capacity = (u32)want;
        bvh->node_bytes = new_bytes;
        bvh->grow_events++;
    }
    return bvh->node_count++;
}

static void build_recursive(BuildCtx *c, u32 node_index, u32 lo, u32 hi,
                            u32 depth) {
    MeshBvh *bvh = c->bvh;
    Aabb bounds = aabb_empty();
    u32 i;

    if (c->error != TG_OK) { return; }
    if (depth > bvh->max_depth) { bvh->max_depth = depth; }

    for (i = lo; i < hi; ++i) { bounds = aabb_union(bounds, c->tri_bounds[i]); }
    bvh->nodes[node_index].bounds = bounds;

    if (hi - lo <= MB_LEAF_TARGET || depth >= MB_MAX_DEPTH) {
        bvh->nodes[node_index].first = lo;
        bvh->nodes[node_index].count = hi - lo;
        return;
    }

    {
        V3 e = aabb_extent(bounds);
        u32 axis = 0u;
        u32 mid = lo + (hi - lo) / 2u;
        u32 left, right;
        if (e.y > e.x && e.y >= e.z) { axis = 1u; }
        else if (e.z > e.x && e.z > e.y) { axis = 2u; }
        select_median(c, lo, hi, axis, mid);

        left = alloc_node(c);
        right = alloc_node(c);
        if (c->error != TG_OK) { return; }
        /* The right child is required to be left + 1, which is what lets a node
         * store one child index in 4 bytes. Asserting it here rather than trusting
         * the allocation order means a future change to alloc_node cannot silently
         * corrupt traversal. */
        TG_CHECK(right == left + 1u);
        bvh->nodes[node_index].first = left;
        bvh->nodes[node_index].count = 0u;
        build_recursive(c, left, lo, mid, depth + 1u);
        build_recursive(c, right, mid, hi, depth + 1u);
    }
}

TgResult mesh_bvh_build(MeshBvh *bvh, const Mesh *m, u32 section_mask) {
    BuildCtx c;
    u64 total, i;
    u32 kept = 0;
    u64 node_cap;
    TgResult r = TG_OK;

    TG_CHECK(bvh != NULL && m != NULL);
    memset(bvh, 0, sizeof *bvh);
    bvh->section_mask = section_mask;

    if (!m->finalized) {
        TG_LOG_ERRORF(MB_SUB, "refusing to build over a mesh that is not "
                              "finalised: a mesh that can still grow would leave "
                              "every triangle index in this tree dangling");
        return TG_ERR_INVALID_ARGUMENT;
    }

    total = mesh_triangle_count(m);
    if (total == 0u) { return TG_OK; }

    {
        u64 tb, cb, bb;
        if (!tg_ckd_mul_u64(total, sizeof(u32), &tb) ||
            !tg_ckd_mul_u64(total, sizeof(V3), &cb) ||
            !tg_ckd_mul_u64(total, sizeof(Aabb), &bb)) {
            return TG_ERR_OVERFLOW;
        }
        bvh->tri_ids = (u32 *)tg_alloc_zero(tb);
        c.centroids = (V3 *)tg_alloc_zero(cb);
        c.tri_bounds = (Aabb *)tg_alloc_zero(bb);
        if (bvh->tri_ids == NULL || c.centroids == NULL ||
            c.tri_bounds == NULL) {
            r = TG_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        bvh->tri_slots = (u32)total;
    }

    for (i = 0; i < total; ++i) {
        MeshSection s = mesh_triangle_section(m, i);
        V3 centroid;
        Aabb b;
        if ((u32)s >= 32u) { continue; }
        if ((section_mask & (1u << (u32)s)) == 0u) { continue; }
        b = triangle_bounds(m, i, &centroid);
        if (aabb_is_empty(b)) { continue; }
        bvh->tri_ids[kept] = (u32)i;
        c.centroids[kept] = centroid;
        c.tri_bounds[kept] = b;
        kept++;
    }
    bvh->tri_count = kept;
    if (kept == 0u) { goto cleanup; }

    /* Node capacity is sized from the triangles actually KEPT, not from the whole
     * mesh. Sizing it from the mesh allocated 400 MB where 230 was used on a
     * wood-only index over a tree whose foliage is two thirds of its triangles --
     * the section mask exists precisely so that the index is smaller than the mesh,
     * and the allocation has to honour it.
     *
     * This is the MEASURED typical node count rather than a bound. Estimating it as
     * kept / leaf_target leaves was optimistic and the array had to grow once on a
     * mature tree, which transiently costs 1.5x the memory it was trying to save:
     * recursion stops when a node holds at most the target, so leaves come out
     * between half and a full target -- measured at 4.7 for a target of 8. Sizing
     * from that, rather than from the target, removes the reallocation. */
    {
        u64 leaves = ((u64)kept * 2u + MB_LEAF_TARGET - 1u) / MB_LEAF_TARGET;
        u64 nb;
        node_cap = leaves * 2u + 64u;
        if (node_cap > 0xFFFFFFFEull) { r = TG_ERR_LIMIT_EXCEEDED; goto cleanup; }
        if (!tg_ckd_mul_u64(node_cap, sizeof(MeshBvhNode), &nb)) {
            r = TG_ERR_OVERFLOW;
            goto cleanup;
        }
        bvh->nodes = (MeshBvhNode *)tg_alloc_zero(nb);
        if (bvh->nodes == NULL) { r = TG_ERR_OUT_OF_MEMORY; goto cleanup; }
        bvh->node_capacity = (u32)node_cap;
        bvh->node_bytes = nb;
    }

    c.bvh = bvh;
    c.mesh = m;
    c.error = TG_OK;
    {
        u32 root = alloc_node(&c);
        TG_CHECK(root == 0u);
        build_recursive(&c, root, 0u, kept, 0u);
    }
    r = c.error;

cleanup:
    if (c.centroids != NULL) {
        tg_free(c.centroids, total * sizeof(V3));
    }
    if (c.tri_bounds != NULL) {
        tg_free(c.tri_bounds, total * sizeof(Aabb));
    }
    if (r != TG_OK) { mesh_bvh_destroy(bvh); }
    return r;
}

void mesh_bvh_destroy(MeshBvh *bvh) {
    if (bvh == NULL) { return; }
    if (bvh->nodes != NULL) {
        tg_free(bvh->nodes, bvh->node_bytes);
    }
    if (bvh->tri_ids != NULL) {
        tg_free(bvh->tri_ids, (u64)bvh->tri_slots * sizeof(u32));
    }
    memset(bvh, 0, sizeof *bvh);
}

void mesh_bvh_stats(const MeshBvh *bvh, MeshBvhStats *out) {
    u32 i;
    u64 leaf_tris = 0;
    TG_CHECK(bvh != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->nodes = bvh->node_count;
    out->max_depth = bvh->max_depth;
    out->bytes = bvh->node_bytes + (u64)bvh->tri_slots * sizeof(u32);
    out->grow_events = bvh->grow_events;
    for (i = 0; i < bvh->node_count; ++i) {
        if (bvh->nodes[i].count == 0u) { continue; }
        out->leaves++;
        leaf_tris += bvh->nodes[i].count;
        if (bvh->nodes[i].count > out->max_leaf_triangles) {
            out->max_leaf_triangles = bvh->nodes[i].count;
        }
    }
    if (out->leaves > 0u) {
        out->mean_leaf_triangles = (f32)((f64)leaf_tris / (f64)out->leaves);
    }
}

/* ------------------------------------------------------------------------- */
/* Traversal                                                                 */
/*                                                                           */
/* An explicit stack, not recursion. Depth is bounded by construction, but a       */
/* traversal is on the hot path of every mouse move and a call frame per level is   */
/* both slower and, on a deep tree, a stack-overflow risk in a UI thread with a     */
/* small stack.                                                                    */
/*                                                                           */
/* Children are pushed FAR FIRST so the near child is popped first. That is what   */
/* lets the running best-t prune the far subtree in the common case, and it is the  */
/* difference between a log-time pick and a linear one.                            */
/* ------------------------------------------------------------------------- */
bool mesh_bvh_raycast(const MeshBvh *bvh, const Mesh *m, Ray ray,
                      bool cull_backface, MeshBvhHit *out) {
    u32 stack[MB_MAX_DEPTH * 2u + 4u];
    u32 sp = 0;
    MeshBvhHit best;
    f32 best_t;

    memset(&best, 0, sizeof best);
    best.triangle = 0;
    if (out != NULL) { *out = best; }
    if (bvh == NULL || m == NULL || bvh->node_count == 0u) { return false; }

    best_t = ray.t_max;
    stack[sp++] = 0u;
    while (sp > 0u) {
        const MeshBvhNode *n = &bvh->nodes[stack[--sp]];
        f32 t_near = 0.0f;
        Ray clipped = ray;
        clipped.t_max = best_t;
        best.nodes_visited++;
        if (!ray_aabb(clipped, n->bounds, &t_near)) { continue; }
        if (t_near > best_t) { continue; }

        if (n->count > 0u) {
            u32 k;
            for (k = 0; k < n->count; ++k) {
                u64 tri = bvh->tri_ids[n->first + k];
                MeshTriangle t;
                f32 hit_t, u, v;
                best.triangles_tested++;
                if (!mesh_get_triangle(m, tri, &t)) { continue; }
                if (!ray_triangle(clipped, t.position[0], t.position[1],
                                  t.position[2], cull_backface, &hit_t, &u,
                                  &v)) {
                    continue;
                }
                if (hit_t >= best_t || hit_t < ray.t_min) { continue; }
                best_t = hit_t;
                best.hit = true;
                best.triangle = tri;
                best.t = hit_t;
                best.position = v3_add(ray.origin, v3_scale(ray.dir, hit_t));
                best.normal = t.normal;
                best.bary[1] = u;
                best.bary[2] = v;
                best.bary[0] = 1.0f - u - v;
                best.organ_id = t.organ_id;
                best.section = t.section;
            }
            continue;
        }

        /* Interior. Order the two children by entry distance and push the far one
         * first. */
        {
            u32 left = n->first, right = n->first + 1u;
            f32 tl = 0.0f, tr = 0.0f;
            bool hl = ray_aabb(clipped, bvh->nodes[left].bounds, &tl);
            bool hr = ray_aabb(clipped, bvh->nodes[right].bounds, &tr);
            if (hl && hr) {
                if (tl <= tr) {
                    stack[sp++] = right; stack[sp++] = left;
                } else {
                    stack[sp++] = left; stack[sp++] = right;
                }
            } else if (hl) {
                stack[sp++] = left;
            } else if (hr) {
                stack[sp++] = right;
            }
            /* sp cannot exceed the stack: each interior visit pops one and pushes
             * at most two, and depth is bounded by MB_MAX_DEPTH. */
            TG_CHECK(sp < (u32)TG_COUNTOF(stack));
        }
    }

    if (out != NULL) { *out = best; }
    return best.hit;
}

TgResult mesh_bvh_query_box(const MeshBvh *bvh, const Mesh *m, Aabb box,
                            u64 *out_ids, u32 capacity, u32 *out_written,
                            u32 *out_total) {
    u32 stack[MB_MAX_DEPTH * 2u + 4u];
    u32 sp = 0;
    u32 written = 0;
    u32 total = 0;

    TG_CHECK(bvh != NULL && m != NULL);
    if (out_written != NULL) { *out_written = 0; }
    if (out_total != NULL) { *out_total = 0; }
    if (bvh->node_count == 0u) { return TG_OK; }

    stack[sp++] = 0u;
    while (sp > 0u) {
        const MeshBvhNode *n = &bvh->nodes[stack[--sp]];
        if (!aabb_overlaps(n->bounds, box)) { continue; }
        if (n->count > 0u) {
            u32 k;
            for (k = 0; k < n->count; ++k) {
                u64 tri = bvh->tri_ids[n->first + k];
                MeshTriangle t;
                Aabb tb = aabb_empty();
                if (!mesh_get_triangle(m, tri, &t)) { continue; }
                tb = aabb_add_point(tb, t.position[0]);
                tb = aabb_add_point(tb, t.position[1]);
                tb = aabb_add_point(tb, t.position[2]);
                if (!aabb_overlaps(tb, box)) { continue; }
                total++;
                if (written < capacity && out_ids != NULL) {
                    out_ids[written++] = tri;
                }
            }
            continue;
        }
        stack[sp++] = n->first;
        stack[sp++] = n->first + 1u;
        TG_CHECK(sp < (u32)TG_COUNTOF(stack));
    }
    if (out_written != NULL) { *out_written = written; }
    if (out_total != NULL) { *out_total = total; }
    /* A truncated answer is reported as such rather than silently clipped: a
     * caller that cannot tell the difference will draw the wrong conclusion. */
    return (total > written) ? TG_ERR_LIMIT_EXCEEDED : TG_OK;
}
