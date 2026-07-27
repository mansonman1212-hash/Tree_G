/* tree_skin.h -- sweeps the biological graph into a woody triangle surface.
 *
 * Layer 2. Portable C17.
 *
 * WHAT THIS DOES
 *   Walks every axis in order, emits a cross-section ring at each internode
 *   boundary using the rotation-minimising frame ALREADY STORED in the graph, and
 *   stitches consecutive rings into a tube wall. Every visible feature is real
 *   indexed triangle geometry; nothing here is a shader trick.
 *
 * WHY THE FRAMES COME FROM THE GRAPH
 *   The frames were propagated at organ creation and rebuilt after bending, so
 *   this pass never computes one. That is deliberate: if meshing derived its own
 *   frames it could introduce twist that the graph does not know about, and the
 *   educational view would be showing frames the surface does not actually use.
 *
 * CROSS-SECTIONS ARE NOT CIRCLES
 *   Three deviations, each with a cause rather than being noise:
 *     - profile lobing, a low-order harmonic that keeps the silhouette from
 *       reading as a lathe-turned cylinder;
 *     - reaction-wood eccentricity, applied on the side the profile dictates
 *       (upper for angiosperm tension wood, lower for gymnosperm compression
 *       wood), which is what makes loaded branches oval with an off-centre pith;
 *     - branch-collar swelling on the parent around every union.
 *
 * KNOWN INTERIM STATE, STATED PLAINLY
 *   Each axis is currently swept as its own closed tube. A lateral's basal rings
 *   are flared into a collar and pushed to meet the parent's surface, so there is
 *   no visible gap, but the tubes still INTERSECT inside the parent rather than
 *   sharing a welded junction surface. The project directive forbids leaving
 *   internal overlapping branch tubes, so this is an explicitly temporary state,
 *   not a design: `mesh_junction` will replace it. It is recorded in
 *   docs/limitations.md and reported by SkinResult.interpenetrating_unions.
 */
#ifndef TG_TREE_SKIN_H
#define TG_TREE_SKIN_H

#include "../geom/mesh.h"
#include "tree_graph.h"
#include "tree_profile.h"

typedef struct SkinResult {
    u32 axes_meshed;
    u32 axes_skipped_empty;
    u32 rings_emitted;
    u32 vertices;
    u32 triangles;
    u32 min_ring_segments;
    u32 max_ring_segments;
    u32 collars_applied;
    u32 bark_axes;                /* axes thick enough to carry bark relief   */
    u32 interpenetrating_unions;  /* honest count of the interim state above  */
    u32 hard_corner_violations;   /* from the normal pass; should be 0        */
    bool hit_vertex_limit;
} SkinResult;

/* Appends the woody surface to `mesh` as MESH_SECTION_WOOD. The mesh must not
 * already have that section open or used. The graph must have radii assigned
 * (run tree_mechanics first) -- otherwise every ring would collapse to the tip
 * radius, which is reported rather than silently produced. */
TgResult tree_skin_build(Mesh *mesh, const TreeGraph *graph,
                         const TreeResolved *resolved, SkinResult *out);

/* Number of cross-section segments chosen for a given radius, exposed so the
 * statistics panel and the tests can reason about tessellation without
 * duplicating the rule. */
u32 tree_skin_ring_segments(const TreeResolved *r, f32 radius);

#endif /* TG_TREE_SKIN_H */
