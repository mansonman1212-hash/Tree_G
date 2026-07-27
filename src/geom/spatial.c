#include "spatial.h"

#include "../core/log.h"

#include <string.h>

#define SP_SUB "spatial"

static void cell_coords(const SpatialGrid *g, V3 p, i32 out[3]) {
    /* floorf, not truncation: a negative coordinate must map to the cell below
     * zero, and truncation toward zero would fold two cells into one. */
    out[0] = (i32)floorf((p.x - g->origin.x) * g->inv_cell_size);
    out[1] = (i32)floorf((p.y - g->origin.y) * g->inv_cell_size);
    out[2] = (i32)floorf((p.z - g->origin.z) * g->inv_cell_size);
}

static u64 cell_index(const SpatialGrid *g, i32 x, i32 y, i32 z) {
    /* Caller must have clamped. */
    return (u64)x + (u64)g->dim[0] * ((u64)y + (u64)g->dim[1] * (u64)z);
}

static i32 clamp_axis(i32 v, u32 dim) {
    if (v < 0) { return 0; }
    if ((u32)v >= dim) { return (i32)dim - 1; }
    return v;
}

TgResult spatial_build(SpatialGrid *g, const V3 *points, u32 point_count,
                       f32 cell_size, u64 max_cells) {
    Aabb bounds = aabb_empty();
    u32 i;
    V3 extent;
    u64 cells;

    TG_CHECK(g != NULL);
    memset(g, 0, sizeof *g);
    g->points = points;
    g->point_count = point_count;
    g->live_count = 0;

    if (max_cells == 0) { max_cells = 1u << 22; } /* 4M cells, 16 MiB of heads */

    if (point_count == 0 || points == NULL) {
        /* An empty grid is a valid, queryable object. The generator legitimately
         * runs out of attraction points, and that must not be a special case at
         * every call site. */
        g->cell_size = 1.0f;
        g->inv_cell_size = 1.0f;
        g->dim[0] = g->dim[1] = g->dim[2] = 1;
        g->cell_count = 1;
        g->cell_head_bytes = sizeof(u32);
        g->cell_head = (u32 *)tg_alloc(g->cell_head_bytes);
        if (g->cell_head == NULL) { return TG_ERR_OUT_OF_MEMORY; }
        g->cell_head[0] = TG_INVALID_ID;
        return TG_OK;
    }

    for (i = 0; i < point_count; ++i) {
        if (!v3_finite(points[i])) {
            TG_LOG_ERRORF(SP_SUB, "point %u is not finite", i);
            return TG_ERR_INVALID_ARGUMENT;
        }
        bounds = aabb_add_point(bounds, points[i]);
    }

    extent = aabb_extent(bounds);

    if (!(cell_size > 0.0f)) {
        /* Target ~2 points per cell on average, which balances the cost of the
         * neighbourhood scan against the number of cells touched. Derived from
         * the point density rather than guessed. */
        f32 volume = tg_maxf(extent.x, 1e-4f) * tg_maxf(extent.y, 1e-4f)
                   * tg_maxf(extent.z, 1e-4f);
        f32 per_point = volume / (f32)point_count;
        cell_size = powf(tg_maxf(per_point * 2.0f, 1e-12f), 1.0f / 3.0f);
    }
    if (!(cell_size > 0.0f) || !tg_finitef(cell_size)) { cell_size = 1.0f; }

    /* Grow the cell size until the grid fits the cell budget. Reported, not
     * silently failed: a degenerate point set (all points on a line) would
     * otherwise demand an enormous grid in one axis. */
    for (;;) {
        u32 d0 = (u32)tg_maxf(1.0f, floorf(extent.x / cell_size) + 1.0f);
        u32 d1 = (u32)tg_maxf(1.0f, floorf(extent.y / cell_size) + 1.0f);
        u32 d2 = (u32)tg_maxf(1.0f, floorf(extent.z / cell_size) + 1.0f);
        u64 total;
        if (!tg_ckd_mul_u64((u64)d0, (u64)d1, &total) ||
            !tg_ckd_mul_u64(total, (u64)d2, &total)) {
            cell_size *= 2.0f;
            continue;
        }
        if (total <= max_cells) {
            g->dim[0] = d0;
            g->dim[1] = d1;
            g->dim[2] = d2;
            cells = total;
            break;
        }
        cell_size *= 1.5f;
        if (!tg_finitef(cell_size)) { return TG_ERR_INVALID_ARGUMENT; }
    }

    g->origin = bounds.mn;
    g->cell_size = cell_size;
    g->inv_cell_size = 1.0f / cell_size;
    g->cell_count = cells;

    if (!tg_ckd_mul_u64(cells, sizeof(u32), &g->cell_head_bytes) ||
        !tg_ckd_mul_u64((u64)point_count, sizeof(u32), &g->next_bytes) ||
        !tg_ckd_mul_u64((u64)point_count, sizeof(bool), &g->removed_bytes)) {
        return TG_ERR_OVERFLOW;
    }

    g->cell_head = (u32 *)tg_alloc(g->cell_head_bytes);
    g->next = (u32 *)tg_alloc(g->next_bytes);
    g->removed = (bool *)tg_alloc_zero(g->removed_bytes);
    if (g->cell_head == NULL || g->next == NULL || g->removed == NULL) {
        spatial_destroy(g);
        return TG_ERR_OUT_OF_MEMORY;
    }

    {
        u64 ci;
        for (ci = 0; ci < cells; ++ci) { g->cell_head[ci] = TG_INVALID_ID; }
    }

    /* Insert in DESCENDING index order so that each cell's chain ends up in
     * ascending index order. Queries then produce ascending indices per cell
     * naturally, which reduces the work the final ordering step has to do. */
    for (i = point_count; i-- > 0;) {
        i32 c[3];
        u64 ci;
        cell_coords(g, points[i], c);
        c[0] = clamp_axis(c[0], g->dim[0]);
        c[1] = clamp_axis(c[1], g->dim[1]);
        c[2] = clamp_axis(c[2], g->dim[2]);
        ci = cell_index(g, c[0], c[1], c[2]);
        TG_ASSERT(ci < g->cell_count);
        g->next[i] = g->cell_head[ci];
        g->cell_head[ci] = i;
    }
    g->live_count = point_count;
    return TG_OK;
}

void spatial_destroy(SpatialGrid *g) {
    if (g == NULL) { return; }
    if (g->cell_head != NULL) { tg_free(g->cell_head, g->cell_head_bytes); }
    if (g->next != NULL) { tg_free(g->next, g->next_bytes); }
    if (g->removed != NULL) { tg_free(g->removed, g->removed_bytes); }
    memset(g, 0, sizeof *g);
}

bool spatial_is_removed(const SpatialGrid *g, u32 point_index) {
    TG_CHECK(g != NULL);
    if (point_index >= g->point_count) { return true; }
    if (g->removed == NULL) { return false; }
    return g->removed[point_index];
}

bool spatial_remove(SpatialGrid *g, u32 point_index) {
    TG_CHECK(g != NULL);
    if (point_index >= g->point_count || g->removed == NULL) { return false; }
    if (g->removed[point_index]) { return false; }
    g->removed[point_index] = true;
    TG_ASSERT(g->live_count > 0);
    g->live_count--;
    return true;
}

/* Inserts `value` into the ascending-order prefix `out[0..*count)`, keeping the
 * array sorted and bounded by `capacity`. When full, an element is only accepted
 * if it is smaller than the current maximum, so the retained set is always the
 * numerically smallest indices -- deterministic regardless of visit order. */
static void insert_sorted_bounded(u32 *out, u32 capacity, u32 *count, u32 value) {
    u32 n = *count;
    u32 i;
    if (capacity == 0) { return; }
    if (n == capacity) {
        if (value >= out[n - 1]) { return; }
        n--; /* drop the largest to make room */
    }
    i = n;
    while (i > 0 && out[i - 1] > value) {
        out[i] = out[i - 1];
        i--;
    }
    out[i] = value;
    *count = n + 1;
}

TgResult spatial_query_radius(const SpatialGrid *g, V3 center, f32 radius,
                              u32 *out, u32 out_capacity,
                              u32 *out_count, u32 *out_total) {
    i32 lo[3], hi[3];
    i32 x, y, z;
    f32 r2;
    u32 count = 0;
    u32 total = 0;

    TG_CHECK(g != NULL);
    if (out_count != NULL) { *out_count = 0; }
    if (out_total != NULL) { *out_total = 0; }
    if (g->point_count == 0 || g->next == NULL) { return TG_OK; }
    if (!(radius > 0.0f) || !v3_finite(center)) { return TG_OK; }

    r2 = radius * radius;

    /* Scan every cell the query sphere can touch. Computed from the radius, not
     * fixed at 3x3x3: with a cell size derived from point density the radius may
     * span several cells, and a hard-coded neighbourhood would silently miss
     * points -- a bug that shows up as branches ignoring nearby attractors. */
    {
        V3 mn = v3_sub(center, v3_splat(radius));
        V3 mx = v3_add(center, v3_splat(radius));
        i32 c0[3], c1[3];
        cell_coords(g, mn, c0);
        cell_coords(g, mx, c1);
        lo[0] = clamp_axis(c0[0], g->dim[0]);
        lo[1] = clamp_axis(c0[1], g->dim[1]);
        lo[2] = clamp_axis(c0[2], g->dim[2]);
        hi[0] = clamp_axis(c1[0], g->dim[0]);
        hi[1] = clamp_axis(c1[1], g->dim[1]);
        hi[2] = clamp_axis(c1[2], g->dim[2]);
    }

    for (z = lo[2]; z <= hi[2]; ++z) {
        for (y = lo[1]; y <= hi[1]; ++y) {
            for (x = lo[0]; x <= hi[0]; ++x) {
                u64 ci = cell_index(g, x, y, z);
                u32 p = g->cell_head[ci];
                while (p != TG_INVALID_ID) {
                    if (!g->removed[p] &&
                        v3_dist_sq(g->points[p], center) <= r2) {
                        total++;
                        insert_sorted_bounded(out, out_capacity, &count, p);
                    }
                    p = g->next[p];
                }
            }
        }
    }

    if (out_count != NULL) { *out_count = count; }
    if (out_total != NULL) { *out_total = total; }
    return TG_OK;
}

u32 spatial_nearest(const SpatialGrid *g, V3 center, f32 max_radius,
                    f32 *out_dist_sq) {
    i32 lo[3], hi[3];
    i32 x, y, z;
    f32 best = 3.402823466e38f;
    u32 best_idx = TG_INVALID_ID;
    f32 r2;

    TG_CHECK(g != NULL);
    if (out_dist_sq != NULL) { *out_dist_sq = 0.0f; }
    if (g->point_count == 0 || g->next == NULL) { return TG_INVALID_ID; }
    if (!(max_radius > 0.0f) || !v3_finite(center)) { return TG_INVALID_ID; }
    r2 = max_radius * max_radius;

    {
        V3 mn = v3_sub(center, v3_splat(max_radius));
        V3 mx = v3_add(center, v3_splat(max_radius));
        i32 c0[3], c1[3];
        cell_coords(g, mn, c0);
        cell_coords(g, mx, c1);
        lo[0] = clamp_axis(c0[0], g->dim[0]);
        lo[1] = clamp_axis(c0[1], g->dim[1]);
        lo[2] = clamp_axis(c0[2], g->dim[2]);
        hi[0] = clamp_axis(c1[0], g->dim[0]);
        hi[1] = clamp_axis(c1[1], g->dim[1]);
        hi[2] = clamp_axis(c1[2], g->dim[2]);
    }

    for (z = lo[2]; z <= hi[2]; ++z) {
        for (y = lo[1]; y <= hi[1]; ++y) {
            for (x = lo[0]; x <= hi[0]; ++x) {
                u64 ci = cell_index(g, x, y, z);
                u32 p = g->cell_head[ci];
                while (p != TG_INVALID_ID) {
                    if (!g->removed[p]) {
                        f32 d2 = v3_dist_sq(g->points[p], center);
                        if (d2 <= r2) {
                            /* Strict less-than, plus an explicit lower-index
                             * tie-break, so an exact tie cannot depend on which
                             * cell was visited first. */
                            if (d2 < best || (d2 == best && p < best_idx)) {
                                best = d2;
                                best_idx = p;
                            }
                        }
                    }
                    p = g->next[p];
                }
            }
        }
    }

    if (best_idx != TG_INVALID_ID && out_dist_sq != NULL) { *out_dist_sq = best; }
    return best_idx;
}

SpatialStats spatial_stats(const SpatialGrid *g) {
    SpatialStats st;
    u64 c;
    u64 occupied_total = 0;

    TG_CHECK(g != NULL);
    memset(&st, 0, sizeof st);
    st.cell_count = g->cell_count;
    st.bytes = g->cell_head_bytes + g->next_bytes + g->removed_bytes;

    if (g->cell_head == NULL) { return st; }
    for (c = 0; c < g->cell_count; ++c) {
        u32 n = 0;
        u32 p = g->cell_head[c];
        while (p != TG_INVALID_ID) {
            n++;
            p = g->next[p];
        }
        if (n > 0) {
            st.occupied_cells++;
            occupied_total += n;
            if (n > st.max_cell_occupancy) { st.max_cell_occupancy = n; }
        }
    }
    if (st.occupied_cells > 0) {
        st.mean_occupancy_of_occupied =
            (f32)occupied_total / (f32)st.occupied_cells;
    }
    return st;
}
