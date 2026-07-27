/* spatial.h -- uniform spatial hash grid over points.
 *
 * Layer 1. Portable C17.
 *
 * PURPOSE
 *   The growth simulation repeatedly asks "which attraction points lie within r
 *   of this bud?" and "does this leaf overlap one already placed?". Both are
 *   nearest-neighbour queries over a few hundred thousand points, run every
 *   growth step. A linear scan makes generation quadratic; this makes it linear
 *   in the number of points actually near the query.
 *
 * WHY A UNIFORM GRID RATHER THAN A KD-TREE OR BVH
 *   The point sets here are (a) approximately uniformly distributed inside a
 *   crown envelope and (b) *mutated* between growth steps, because attraction
 *   points are consumed as branches reach them. A uniform grid supports removal
 *   in O(1) without rebalancing; a kd-tree does not. The query radius is also
 *   known in advance and roughly constant, which is exactly the case a uniform
 *   grid handles optimally.
 *
 * DETERMINISM
 *   Every query returns results in ascending point-index order, never in bucket
 *   traversal order. This is not a convenience: the growth simulation picks
 *   among near points, and if the order depended on the grid's internal layout
 *   the tree would change whenever the cell size or insertion order changed.
 */
#ifndef TG_SPATIAL_H
#define TG_SPATIAL_H

#include "../core/math3d.h"
#include "../core/mem.h"

typedef struct SpatialGrid {
    /* Cell-linked-list representation: cell_head[c] is the first point index in
     * cell c, next[p] chains to the following point, TG_INVALID_ID terminates.
     * Chosen over per-cell dynamic arrays because it is a single flat allocation
     * with no per-cell growth and no pointer chasing into scattered blocks. */
    u32 *cell_head;
    u32 *next;

    V3   origin;      /* world position of cell (0,0,0) lower corner */
    f32  cell_size;
    f32  inv_cell_size;
    u32  dim[3];      /* grid resolution                            */
    u64  cell_count;

    const V3 *points; /* NOT owned: the caller keeps the point array alive */
    u32  point_count;
    u32  live_count;  /* points not marked removed                  */
    bool *removed;

    u64 cell_head_bytes;
    u64 next_bytes;
    u64 removed_bytes;
} SpatialGrid;

/* Builds a grid over `points` (borrowed, must outlive the grid).
 *
 * `cell_size` should be close to the intended query radius: a much smaller cell
 * makes the 3x3x3 neighbourhood scan miss candidates (the query compensates by
 * widening the scan, at a cost), and a much larger one degenerates toward a
 * linear scan. Pass 0 to derive it from the point bounds and count so that the
 * average occupancy is a few points per cell.
 *
 * `max_cells` bounds the allocation; if the requested resolution exceeds it the
 * cell size is increased until it fits, which is reported through the resulting
 * grid rather than failing. */
TgResult spatial_build(SpatialGrid *g, const V3 *points, u32 point_count,
                       f32 cell_size, u64 max_cells);
void     spatial_destroy(SpatialGrid *g);

/* Marks a point as removed. Used when an attraction point has been colonised.
 * O(1), no rebuild. Returns false if already removed or out of range. */
bool spatial_remove(SpatialGrid *g, u32 point_index);
bool spatial_is_removed(const SpatialGrid *g, u32 point_index);
static inline u32 spatial_live_count(const SpatialGrid *g) { return g->live_count; }

/* Collects live point indices within `radius` of `center`, in ASCENDING INDEX
 * ORDER, into `out` (capacity `out_capacity`).
 *
 * Writes the number stored to *out_count and the number that would have been
 * stored to *out_total. out_total > out_count means the caller's buffer was too
 * small: the result is then the numerically smallest indices, which is still
 * deterministic, and the caller can decide whether that matters. */
TgResult spatial_query_radius(const SpatialGrid *g, V3 center, f32 radius,
                              u32 *out, u32 out_capacity,
                              u32 *out_count, u32 *out_total);

/* Nearest live point within `max_radius`. Returns TG_INVALID_ID if none.
 * Ties (exactly equal squared distance) resolve to the lower index, so the
 * result does not depend on traversal order. */
u32 spatial_nearest(const SpatialGrid *g, V3 center, f32 max_radius,
                    f32 *out_dist_sq);

/* Diagnostics for tuning and for the statistics panel. */
typedef struct SpatialStats {
    u64 cell_count;
    u32 occupied_cells;
    u32 max_cell_occupancy;
    f32 mean_occupancy_of_occupied;
    u64 bytes;
} SpatialStats;

SpatialStats spatial_stats(const SpatialGrid *g);

#endif /* TG_SPATIAL_H */
