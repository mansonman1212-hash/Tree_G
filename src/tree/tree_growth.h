/* tree_growth.h -- developmental growth simulation.
 *
 * Layer 2. Portable C17.
 *
 * WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
 *
 *   It simulates an ordered growth HISTORY. It does not create a finished
 *   silhouette and scatter branches inside it, which the project directive
 *   forbids and which is the root cause of most synthetic-looking trees.
 *
 *   It is a controlled hybrid with one responsibility per mechanism, exactly as
 *   docs/research.md specifies. No single algorithm is treated as the biological
 *   answer:
 *
 *     A. Developmental grammar   decides what nodes, internodes, buds and axis
 *                                categories may exist, the rhythm of flushes,
 *                                phyllotaxis, whorl structure, and which orders
 *                                are legal. Comes from the profile.
 *
 *     B. Resource signalling     decides WHICH buds break and how much extension
 *                                each shoot gets, via a basipetal light-gathering
 *                                pass followed by an acropetal Borchert-Honda
 *                                partition with a profile apical-control
 *                                coefficient. This is what produces apical
 *                                dominance, suppressed interior shoots and
 *                                genuinely unequal siblings.
 *
 *     C. Space colonization      decides only the DIRECTION a tip turns, from
 *                                unclaimed space in the crown envelope. It never
 *                                decides whether a shoot exists.
 *
 *     D. Tropisms                gravitropism toward the axis set angle,
 *                                phototropism toward the light, both applied as
 *                                BOUNDED per-step angular budgets so curvature
 *                                accumulates as a segmented arc rather than a
 *                                constant-radius sweep.
 *
 *     E. Mortality               shade-driven and history-dependent, never a
 *                                random cull. A shoot dies after its light stays
 *                                below the profile threshold for longer than the
 *                                profile's suppression tolerance.
 *
 * DETERMINISM
 *   Every random draw uses a substream keyed by (seed, purpose, stable id), so
 *   results are independent of iteration order and of how many organs preceded
 *   them. No global stream is advanced anywhere in this file.
 *
 * OUTPUT
 *   A TreeGraph containing trunk, branch and root axes with internodes, buds and
 *   recorded life history. Radii are NOT set here: that is the mechanics pass.
 */
#ifndef TG_TREE_GROWTH_H
#define TG_TREE_GROWTH_H

#include "../geom/spatial.h"
#include "tree_graph.h"
#include "tree_profile.h"

/* Per-step statistics. Every number is counted, never estimated, because the
 * educational construction view displays them. */
typedef struct GrowthStepStats {
    u16 step;
    u32 active_shoots;
    u32 segments_added;
    u32 buds_placed;
    u32 buds_broken;
    u32 shoots_killed;
    u32 attractors_remaining;
    f32 tallest_point_m;
    f32 total_light;
} GrowthStepStats;

#define GROWTH_MAX_RECORDED_STEPS 512

typedef struct GrowthResult {
    u32 attractors_initial;
    u32 attractors_consumed;
    u32 steps_run;
    u32 axes_created;
    u32 segments_created;
    u32 buds_created;
    u32 buds_broken;
    u32 shoots_killed;
    u32 max_order_reached;
    /* Why shoots stopped extending. A crown that comes out wrong is almost always
     * wrong because shoots stopped for the wrong reason, and without these
     * counters that is guesswork. Production diagnostics, not test scaffolding. */
    u32 stopped_by_height;      /* reached the target height                  */
    u32 stopped_by_envelope;    /* reached the crown surface                  */
    u32 stopped_by_shade;       /* self-pruned after prolonged suppression    */
    /* Shed because the live crown base rose past them: crown recession. Counted
     * separately from shade because the two are different mechanisms and a bole
     * that fails to clean itself has to be attributable to one of them. */
    u32 stopped_by_recession;
    /* Dead branches that have since dropped off. Filled by
     * tree_growth_shed_dead_wood, which runs after the mechanics pass rather than
     * during growth, because retention depends on radius. Reported next to the
     * death counters so "died" and "fell off" can be told apart. */
    u32 dead_organs_shed;
    u32 starved_steps;          /* step-instances with too little resource     */
    u32 extension_steps;        /* step-instances that actually extended       */

    bool hit_organ_limit;   /* reported honestly rather than hidden           */
    bool hit_step_limit;
    GrowthStepStats step[GROWTH_MAX_RECORDED_STEPS];
    u32 step_count;
} GrowthResult;

/* Cancellation: the UI must be able to abort a long generation. The flag is read
 * between steps; growth then returns TG_ERR_CANCELLED having left the graph in a
 * structurally valid (if incomplete) state, so nothing downstream can be handed
 * a half-written organ. */
typedef struct GrowthCancel {
    volatile int requested;
} GrowthCancel;

/* Grows the above-ground structure into `graph`. The graph must be empty. */
TgResult tree_growth_run(TreeGraph *graph, const TreeResolved *resolved,
                         GrowthCancel *cancel, GrowthResult *out_result);

/* Grows the root system. Called after the shoot system, because root scale is
 * derived from the realised above-ground structure rather than from the profile
 * alone -- roots must relate to the structure they actually support. */
TgResult tree_growth_roots(TreeGraph *graph, const TreeResolved *resolved,
                           GrowthCancel *cancel, GrowthResult *out_result);

/* Marks dead branches that have been dead long enough to have fallen off, with
 * ORGAN_FLAG_SHED. Fills `out_result->dead_organs_shed`.
 *
 * MUST RUN AFTER tree_mechanics_run, because retention scales with radius and
 * radii are the mechanics pass's output. It is a separate pass rather than part of
 * growth for that reason alone -- the information it needs does not exist yet while
 * the tree is growing.
 *
 * Death and detachment are different events, and this engine modelled only the
 * first. ORGAN_FLAG_SHED existed in tree_graph.h from the beginning, described as
 * "self-pruned: only a scar remains", and was set by nothing and read by nothing,
 * so a branch that died in year 8 was still skinned at full length in year 80.
 *
 * An organ is shed only when every one of its children is already shed, so what is
 * removed is always a distal suffix of an axis and no living wood is ever left
 * hanging off nothing. Nothing is deleted: the organ keeps its id and its history,
 * which is what lets the construction replay show a branch die and then drop. */
TgResult tree_growth_shed_dead_wood(TreeGraph *graph,
                                    const TreeResolved *resolved,
                                    u16 final_step, GrowthResult *out_result);

/* Exposed for the educational construction view and for tests: the attraction
 * point cloud that mediates crown competition. Never rendered as part of the
 * final tree. */
typedef struct AttractorCloud {
    V3 *points;
    u32 count;
    u64 bytes;
    Aabb bounds;
} AttractorCloud;

TgResult tree_growth_build_attractors(const TreeResolved *resolved,
                                      AttractorCloud *out);
void     tree_growth_free_attractors(AttractorCloud *cloud);

#endif /* TG_TREE_GROWTH_H */
