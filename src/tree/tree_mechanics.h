/* tree_mechanics.h -- radial growth, static mechanics, and reaction wood.
 *
 * Layer 2. Portable C17.
 *
 * The growth simulation produces a skeleton with lengths and directions but no
 * thickness and no load response. This pass supplies both, once, and then the
 * geometry is final. Nothing here runs per frame; the project directive forbids
 * any post-generation motion.
 *
 * FOUR THINGS HAPPEN, IN THIS ORDER, AND THE ORDER MATTERS
 *
 *  1. SUPPORTED FOLIAGE AREA, basipetal.
 *     Which shoots actually bear foliage depends on the profile: a deciduous tree
 *     carries leaves on the current year's shoots, an evergreen conifer retains
 *     needles on several age classes. Summed from the tips toward the base.
 *
 *  2. RADII, from the pipe model.
 *     Cross-sectional area is proportional to supported foliage, expressed in the
 *     generalised Leonardo form r = k * A^(1/delta) with delta from the profile.
 *     k is CALIBRATED so the trunk base lands on the resolved individual's
 *     trunk_base_radius_m, which makes the radial model and the size model agree
 *     by construction rather than by coincidence. A basal flare is then added.
 *
 *  3. SUPPORTED MASS, basipetal.
 *     Needs the radii from step 2: wood mass is the volume of each frustum times
 *     the profile's green-wood density, plus foliage mass from step 1.
 *
 *  4. STATIC DEFLECTION, acropetal.
 *     Each segment is a tapered cantilever loaded by everything distal to it.
 *     The per-segment rotation propagates RIGIDLY to the whole distal subtree,
 *     which is what a cantilever actually does and what produces the compound
 *     profile real branches have. Reaction-wood eccentricity is recorded where
 *     the bending moment is largest, with the SIGN taken from the profile:
 *     tension wood above in angiosperms, compression wood below in gymnosperms.
 *
 * AFTER BENDING, FRAMES MUST BE REBUILT.
 *   Rotation-minimising frames were propagated during growth against the
 *   pre-deflection directions. Bending changes those directions, so a frame that
 *   was perpendicular no longer is, and every cross-section built from it would
 *   be sheared. This pass re-propagates the frames along each axis. Forgetting
 *   this is a silent, subtle geometry corruption, so the graph validator's
 *   orthogonality check is the guard.
 */
#ifndef TG_TREE_MECHANICS_H
#define TG_TREE_MECHANICS_H

#include "tree_graph.h"
#include "tree_profile.h"

typedef struct MechanicsResult {
    /* Foliage. */
    f32 total_leaf_area_m2;
    u32 foliage_bearing_segments;
    u32 terminal_shoots;   /* living segments with nothing living distal to them */

    /* Radii. */
    f32 trunk_base_radius_m;      /* realised, after flare                   */
    f32 trunk_base_radius_target; /* what the resolved individual asked for   */
    f32 max_radius_m;
    f32 min_radius_m;
    f32 pipe_calibration_k;

    /* Mass. */
    f32 total_wood_mass_kg;
    f32 total_foliage_mass_kg;

    /* Deflection. */
    f32 max_tip_deflection_m;     /* largest displacement of any tip          */
    f32 max_segment_rotation_rad;
    u32 clamped_rotations;        /* segments whose bend hit the stability cap */
    u32 segments_considered;      /* shoot segments the deflection pass visited */
    f32 max_eccentricity;

    /* Sign audit. Counts segments whose OWN bend moved their own tip upward,
     * measured against their direction after the inherited rotation and before
     * their own. This isolates the deflection sign from rigid propagation, and
     * must always be zero: a segment cannot bend upward under gravity.
     *
     * Note that a TIP may legitimately rise while this stays zero. A branch that
     * points back toward the trunk sits on the proximal side of its parent's
     * bending pivot, so a correct downward rotation of the parent lifts it. That
     * is rigid-body geometry, not a sign error, and confusing the two cost a
     * debugging cycle. */
    u32 own_bend_upward;

    /* Honest reporting of anything that had to be limited. */
    bool hit_rotation_clamp;
} MechanicsResult;

/* Runs the whole pass over a grown graph. The graph must not be finalised.
 * Returns TG_ERR_INVALID_STATE if the graph is empty or already finalised. */
TgResult tree_mechanics_run(TreeGraph *graph, const TreeResolved *resolved,
                            MechanicsResult *out);

/* Foliage area borne by one segment, in square metres. Exposed because the leaf
 * generator must place exactly the foliage the mechanics pass accounted for --
 * two independent estimates would disagree and the tree's mass would not match
 * its appearance. */
f32 tree_mechanics_segment_leaf_area(const Organ *o, const TreeResolved *r,
                                     u16 final_step);

/* True when a segment is young enough to still bear foliage, given the profile's
 * retention. */
bool tree_mechanics_bears_foliage(const Organ *o, const TreeResolved *r,
                                  u16 final_step);

#endif /* TG_TREE_MECHANICS_H */
