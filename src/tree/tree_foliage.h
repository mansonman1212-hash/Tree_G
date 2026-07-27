/* tree_foliage.h -- generates leaves and needles as real polygon geometry.
 *
 * Layer 2. Portable C17.
 *
 * WHAT THIS DOES
 *   Places foliage on the shoots that bear it and builds each leaf, petiole and
 *   needle as indexed triangles. There are no alpha-tested cards, no billboards
 *   and no textured quads anywhere in this pass: a blade is a lobed, cupped,
 *   drooping shell with thickness, and a needle is a tapered closed prism. That
 *   is the project directive, and it is also what makes a leaf survive being
 *   looked at edge-on, which is exactly where a card stops working.
 *
 * WHICH SHOOTS BEAR FOLIAGE
 *   This pass does NOT decide. It calls tree_mechanics_bears_foliage and computes
 *   its leaf count from the same leaves_per_metre_of_shoot the mechanics pass used
 *   to build supported_leaf_area. That is deliberate: the deflection solved by the
 *   mechanics pass is driven by foliage mass, and if the geometry disagreed with
 *   the mass then the tree would be bending under leaves that are not there. The
 *   agreement is checked, not assumed -- FoliageResult reports the leaf area the
 *   generated geometry actually has next to the area the mechanics assumed.
 *
 * THE HONEST LIMIT
 *   A mature broadleaf on this model wants about 134,000 leaves and an 80-year
 *   conifer about 10,000,000 needles. Those are not errors; a real spruce carries
 *   tens of millions of needles. Ten million needles is 80 million triangles, so
 *   the count IS budgeted, and the budget is the one thing in this pass that is a
 *   compromise rather than a model. Every leaf that exists is real geometry; the
 *   number that exist is capped by `foliage_triangle_budget`, thinning is uniform
 *   over the crown rather than clustered, leaf SIZE is never inflated to
 *   compensate, and `leaves_wanted` versus `leaves_placed` is reported on every
 *   run so the shortfall is a visible number rather than a silent one.
 */
#ifndef TG_TREE_FOLIAGE_H
#define TG_TREE_FOLIAGE_H

#include "../geom/mesh.h"
#include "tree_graph.h"
#include "tree_profile.h"

typedef struct FoliageResult {
    u32 bearing_segments;      /* shoots that carry foliage at all           */
    u64 leaves_wanted;         /* what the botany asks for                   */
    u64 leaves_placed;         /* what the triangle budget allowed           */
    u32 petioles_placed;
    u32 fascicles_placed;
    u32 vertices;
    u32 triangles;
    u32 triangles_per_leaf;    /* the tessellation this quality level chose  */
    f32 target_leaf_area_m2;   /* what the mechanics pass assumed            */
    f32 realised_leaf_area_m2; /* measured from the generated blades         */
    bool hit_triangle_budget;
    bool hit_vertex_limit;
} FoliageResult;

/* Appends foliage to `mesh`. Requires the mechanics pass to have run, because
 * foliage sits on the bent, radius-assigned skeleton -- leaves attached to the
 * unbent skeleton would hang in the air beside their twigs. */
TgResult tree_foliage_build(Mesh *mesh, const TreeGraph *graph,
                            const TreeResolved *resolved, u16 final_step,
                            FoliageResult *out);

/* One-sided area of a single blade or needle at this individual's leaf scale, in
 * square metres. Exposed so the mechanics pass, this pass and the tests all agree
 * on one definition instead of three. */
f32 tree_foliage_unit_area(const TreeResolved *resolved);

/* Spanwise stations one blade is tessellated with at this quality level. Exposed
 * because the unit area is integrated at exactly this rate, so a coarser blade
 * genuinely has less area and the mechanics must be told the same number. */
u32 tree_foliage_stations(const TreeResolved *resolved);

#endif /* TG_TREE_FOLIAGE_H */
