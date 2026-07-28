/* mesh_bvh.h -- static bounding-volume hierarchy over a finalised mesh.
 *
 * Layer 1. Portable C17.
 *
 * WHY THIS EXISTS
 *   The directive requires that the tree "withstands close technical inspection".
 *   That is not only a statement about how the geometry looks: it means a viewer
 *   must be able to point at a surface and be told what it is -- which organ, what
 *   order, how old, how much foliage it carries, what it weighs. Every one of those
 *   facts is already in the graph and every triangle already records its organ id;
 *   what is missing is the ability to go from a pixel to a triangle in less than
 *   the 16 million triangle tests a mature tree would otherwise need.
 *
 *   It is also what makes a cutaway or a section query possible later, and what a
 *   collision or contact-shadow pass would want.
 *
 * WHY IT IS BUILT ONCE AND NEVER TOUCHED AGAIN
 *   The tree is permanently static after generation, which is a requirement rather
 *   than a convenience. That removes every hard problem a BVH normally has: no
 *   refit, no incremental insertion, no rebalancing, no thread safety on mutation.
 *   The tree is laid out as two flat arrays, the node array and a permutation of
 *   triangle ids, both sized exactly once.
 *
 * WHAT IT IS NOT
 *   Not a general-purpose ray tracer. There is no packet traversal, no SIMD, no
 *   surface-area-heuristic sweep. A binary split at the median of the longest axis
 *   is used because it is O(n log n) with no tuning parameters and produces trees
 *   within a small factor of an SAH build for geometry as uniformly distributed as
 *   a tree's surface. The cost is stated rather than assumed: mesh_bvh_stats
 *   reports the depth and leaf occupancy actually achieved.
 */
#ifndef TG_MESH_BVH_H
#define TG_MESH_BVH_H

#include "mesh.h"

/* A node is 32 bytes so two fit in a cache line. Interior nodes store the index of
 * their FIRST child and rely on the second being adjacent, which is what keeps the
 * node to 32 bytes without a second index. */
typedef struct MeshBvhNode {
    Aabb bounds;          /* 24 bytes                                         */
    u32  first;           /* interior: left child index. leaf: first tri slot  */
    u32  count;           /* 0 => interior node                               */
} MeshBvhNode;

typedef struct MeshBvh {
    MeshBvhNode *nodes;
    u32         *tri_ids;     /* permutation of triangle ids                  */
    u32          node_count;
    u32          node_capacity;
    u32          tri_count;
    /* Slots allocated, which is the triangle count of the whole mesh rather than
     * the number kept by the section mask. Recorded because the allocator's leak
     * gate matches frees against allocations exactly, and freeing tri_count when
     * tri_slots were allocated is a silent accounting error that only shows up as a
     * phantom leak somewhere else. */
    u32          tri_slots;
    u32          max_depth;
    u64          node_bytes;   /* what was actually allocated for nodes        */
    /* Times the node array had to grow during the build. Reported because a build
     * that reallocates repeatedly is a sign the initial estimate is wrong for this
     * geometry, and that should be visible rather than merely slow. */
    u32          grow_events;
    /* Sections included in the build. A pick that should ignore foliage is far
     * cheaper against a wood-only tree than against a filtered full one. */
    u32          section_mask;
} MeshBvh;

typedef struct MeshBvhStats {
    u32 nodes;
    u32 leaves;
    u32 max_depth;
    u32 max_leaf_triangles;
    f32 mean_leaf_triangles;
    u64 bytes;
    u32 grow_events;
} MeshBvhStats;

typedef struct MeshBvhHit {
    bool  hit;
    u64   triangle;
    f32   t;              /* distance along the ray                           */
    V3    position;
    V3    normal;         /* geometric face normal                            */
    f32   bary[3];
    u32   organ_id;
    MeshSection section;
    u32   nodes_visited;  /* reported so the cost of a pick is never a guess   */
    u32   triangles_tested;
} MeshBvhHit;

/* Builds over every triangle whose section is set in `section_mask`, where bit i
 * corresponds to MeshSection i. Pass MESH_BVH_ALL_SECTIONS for everything.
 * The mesh must be finalised; a BVH over a mesh that can still grow would be a
 * dangling index waiting to happen. */
#define MESH_BVH_ALL_SECTIONS 0xFFFFFFFFu

TgResult mesh_bvh_build(MeshBvh *bvh, const Mesh *m, u32 section_mask);
void     mesh_bvh_destroy(MeshBvh *bvh);
void     mesh_bvh_stats(const MeshBvh *bvh, MeshBvhStats *out);

/* Closest hit along the ray. `cull_backface` false is the right default for
 * inspection: a viewer inside a hollow branch still expects to pick its wall. */
bool mesh_bvh_raycast(const MeshBvh *bvh, const Mesh *m, Ray ray,
                      bool cull_backface, MeshBvhHit *out);

/* Every triangle whose bounds overlap `box`, written into `out_ids` up to
 * `capacity`. `out_total` receives the true count even when it exceeds capacity,
 * so a caller can tell a truncated answer from a complete one. */
TgResult mesh_bvh_query_box(const MeshBvh *bvh, const Mesh *m, Aabb box,
                            u64 *out_ids, u32 capacity, u32 *out_written,
                            u32 *out_total);

#endif /* TG_MESH_BVH_H */
