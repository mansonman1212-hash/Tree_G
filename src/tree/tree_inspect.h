/* tree_inspect.h -- answering "what am I looking at?" about a point on the tree.
 *
 * Layer 2. Portable C17.
 *
 * WHY THIS EXISTS
 *   "Withstands close technical inspection" is not only a claim about how the
 *   geometry looks. A tree that can be interrogated -- point at a surface, be told
 *   which organ it is, what order, what year it was formed, what it carries, what it
 *   weighs -- is one whose claims can be CHECKED, by the user and by the tests. Every
 *   fact reported here already exists in the graph and every triangle already
 *   records its organ; this module is the join between the two, plus the derived
 *   quantities that need a walk up the tree (depth from the trunk, path length from
 *   the base, which axis and which order chain led here).
 *
 *   It is also the module that makes a wrong tree diagnosable. "The crown is too
 *   sparse" is an opinion; "this twig is order 7, was formed in year 74, is 1.9 mm
 *   thick and carries 0.004 m2 of leaf" is a measurement.
 *
 * NO RENDERING, NO UI
 *   Pure queries against a built Tree. The caller supplies the ray; producing one
 *   from a mouse position is camera_pick_ray's job.
 */
#ifndef TG_TREE_INSPECT_H
#define TG_TREE_INSPECT_H

#include "tree_build.h"

typedef struct InspectResult {
    bool hit;

    /* Where. */
    V3   position;
    V3   normal;
    f32  distance;          /* along the query ray                            */
    u64  triangle;
    MeshSection  section;
    MeshMaterial material;

    /* Which organ. */
    u32       organ_id;
    OrganType organ_type;
    u32       branch_order;
    u32       axis_id;
    AxisKind  axis_kind;
    bool      dead;
    bool      terminal;
    u16       created_step;
    u16       death_step;    /* 0 when alive                                  */

    /* What it is. */
    f32 length_m;
    f32 radius_base_m;
    f32 radius_tip_m;
    f32 physiological_age_years;
    f32 eccentricity;

    /* What it carries. All from the basipetal accumulation, so these are the same
     * numbers the mechanics pass loaded the beam with. */
    f32 supported_leaf_area_m2;
    f32 supported_mass_kg;
    f32 light;

    /* Derived by walking to the base. */
    u32 depth_from_base;        /* organs between here and the root organ     */
    f32 path_length_from_base_m;/* summed along the actual path, not straight  */
    f32 height_above_ground_m;

    /* Diagnostic cost of the query itself, so a slow pick is measurable rather
     * than suspected. */
    u32 nodes_visited;
    u32 triangles_tested;
} InspectResult;

/* Closest surface along `ray`. Returns false when the ray misses. Requires the
 * tree's BVH to have been built; without it this would be a linear scan over
 * sixteen million triangles and the caller would discover that as a stall. */
bool tree_inspect_ray(const Tree *t, Ray ray, InspectResult *out);

/* The same record for a named organ, with no ray involved. `position` is the
 * organ's midpoint and `normal` its direction. Used by the tests and by any UI
 * that already knows which organ it means. */
bool tree_inspect_organ(const Tree *t, u32 organ_id, InspectResult *out);

/* A human-readable multi-line description, truncated safely. Returns the number of
 * bytes that WOULD have been written, so truncation is detectable. */
u64 tree_inspect_describe(const Tree *t, const InspectResult *r, char *buf,
                          u64 size);

/* Summary of everything inside a box, for a region query rather than a point one:
 * "what is in this part of the crown". */
typedef struct InspectRegion {
    u32 triangles;
    u32 organs;              /* distinct organs touched                       */
    u32 living_organs;
    u32 max_branch_order;
    u16 earliest_step;
    u16 latest_step;
    f32 total_leaf_area_m2;  /* borne BY the organs in the box                */
    f32 min_radius_m;
    f32 max_radius_m;
    bool truncated;          /* the box held more triangles than were sampled */
} InspectRegion;

bool tree_inspect_region(const Tree *t, Aabb box, InspectRegion *out);

#endif /* TG_TREE_INSPECT_H */
