/* tree_bark.h -- bark relief as a displacement field on the wood surface.
 *
 * Layer 2. Portable C17.
 *
 * WHY THIS IS GEOMETRY AND NOT A NORMAL MAP
 *   The directive is explicit: every visible feature exists as real polygons. Bark
 *   is the surface a viewer gets closest to and the one that gives a procedural
 *   tree away fastest, and a normal map gives itself away in three specific ways
 *   that no amount of texture resolution fixes: the silhouette stays a smooth
 *   cylinder, grazing light does not produce real occlusion inside the furrows, and
 *   parallax is absent so the ridges slide across the surface as the camera moves.
 *   A furrow that is 15 mm deep in the mesh does all three for free.
 *
 * WHAT THIS MODULE IS
 *   A pure field: given a point on a branch -- its angle, its arc length from the
 *   axis base, its radius and its cambial maturity -- it returns how far to move
 *   the surface inward and which material is exposed there. It builds no geometry
 *   itself. `tree_skin` calls it while laying down rings, because bark relief and
 *   the tube it sits on have to be the same surface; adding a second shell would
 *   produce two surfaces a millimetre apart, which is worse than no bark at all.
 *
 * WHERE BARK EXISTS
 *   Not everywhere. A first-year twig has a smooth epidermis, and the deep furrows
 *   of a mature oak exist only where the cambium has been laying down and splitting
 *   rhytidome for decades. Relief is therefore gated on RADIUS, which is the
 *   cheapest honest proxy for cambial age, and this is also what keeps the cost
 *   affordable: the trunk and the primary limbs of a mature broadleaf are a handful
 *   of axes out of a hundred thousand.
 *
 * DETERMINISM AND SEAMLESSNESS
 *   The field is hash-based value noise on a lattice that wraps EXACTLY at an
 *   integer number of cells around the axis, so there is no seam at theta = 0. The
 *   longitudinal coordinate is absolute arc length from the axis base, so a ridge
 *   runs continuously across internode boundaries instead of restarting at every
 *   ring.
 */
#ifndef TG_TREE_BARK_H
#define TG_TREE_BARK_H

#include "../geom/mesh.h"
#include "tree_graph.h"
#include "tree_profile.h"

/* Everything the field needs that is constant for one axis. Computed once by
 * tree_bark_axis_setup so the per-vertex path stays cheap. */
typedef struct BarkAxisField {
    BarkFamily family;
    u32 cells_around;     /* integer, so the lattice wraps with no seam       */
    f32 cells_per_metre;  /* longitudinal lattice density                     */
    f32 depth_m;          /* furrow depth at full maturity                    */
    f32 feature_m;
    u64 seed;             /* per-axis, so no two limbs share a pattern        */
    bool active;          /* false => this axis carries no relief at all      */
} BarkAxisField;

typedef struct BarkSample {
    f32 displacement_m;   /* <= 0, inward from the smooth surface             */
    f32 exposure;         /* 1 on a ridge crest, 0 in a furrow floor          */
    MeshMaterial material;
} BarkSample;

/* Radius below which no relief is generated, in metres. */
f32 tree_bark_min_radius(const TreeResolved *resolved);

/* Prepares the per-axis constants. `max_radius` is the axis's thickest point,
 * which is what fixes the lattice density for the whole axis: letting it follow
 * the local radius would make the cell size change along a taper and shear every
 * ridge. */
void tree_bark_axis_setup(const TreeResolved *resolved, u32 axis_id,
                          f32 max_radius, BarkFamily family,
                          BarkAxisField *out);

/* Samples the field. `theta` in radians, `arc_m` the arc length from the axis
 * base, `radius` the local smooth radius, `maturity` in [0,1]. */
BarkSample tree_bark_sample(const BarkAxisField *f, f32 theta, f32 arc_m,
                            f32 radius, f32 maturity);

/* Longitudinal ring spacing needed to resolve this axis's relief, in metres.
 * Returns a large value when the axis carries none, which is the caller's signal
 * to keep one ring per internode. */
f32 tree_bark_ring_spacing(const BarkAxisField *f);

/* Ring segments needed to resolve the relief around the axis. Returns 0 when
 * there is none. */
u32 tree_bark_ring_segments(const BarkAxisField *f);

/* Which bark family is expressed at a given radius and physiological age. The
 * profile names a juvenile and a mature family; this decides which one a
 * particular part of the tree is showing, so one trunk can legitimately carry
 * smooth young branch bark and deeply furrowed mature bark at once. */
BarkFamily tree_bark_family_at(const TreeResolved *resolved, f32 radius,
                               f32 *out_maturity);

#endif /* TG_TREE_BARK_H */
