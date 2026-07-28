/* tree_build.h -- one entry point that turns settings into a finished tree.
 *
 * Layer 2. Portable C17.
 *
 * WHY THIS EXISTS
 *   Producing a tree currently means calling eight passes in the right order with
 *   the right arguments: resolve, grow shoots, grow roots, mechanics, skin, foliage,
 *   finalise, validate. Every caller that does that by hand is a place where the
 *   order can be got wrong, and getting it wrong is not loud -- skinning before the
 *   mechanics pass produces a complete, valid mesh of an unbent tree with no radii,
 *   and nothing complains. The reference generator already hand-wires the sequence;
 *   the Windows application would be the second copy, and the first divergence.
 *
 *   So the order lives in one place, with the dependencies stated as comments where
 *   they are enforced, and everything a caller could want is in one struct with one
 *   free function.
 *
 * WHAT ELSE IT OWNS
 *   - PROGRESS AND CANCELLATION. A mature tree takes seconds and a very old one
 *     tens of seconds. A UI that cannot report or abort that is unusable, and
 *     cancellation has to be checked inside growth rather than between passes,
 *     because growth is where nearly all the time goes.
 *   - THE CONSTRUCTION STAGE TABLE. The directive requires that the tree can be
 *     observed being constructed from nothing. Every organ records the step it was
 *     born in and every vertex carries that step, so the reveal itself is a clip
 *     test; what a replay additionally needs is to know WHAT to say about each
 *     stage. The table is derived from the finished graph rather than captured
 *     during simulation, which means the replay describes the tree that actually
 *     exists rather than a transient state that was discarded.
 *   - THE FINGERPRINT. One 64-bit value over the settings and the geometry, so
 *     "identical settings produce an identical tree" is a comparison rather than an
 *     assertion.
 *
 * WHAT IT DOES NOT OWN
 *   No timing. A monotonic clock is a platform facility and this layer has no
 *   platform. The caller times the call.
 */
#ifndef TG_TREE_BUILD_H
#define TG_TREE_BUILD_H

#include "../geom/mesh.h"
#include "../geom/mesh_bvh.h"
#include "../geom/mesh_validate.h"
#include "tree_foliage.h"
#include "tree_graph.h"
#include "tree_growth.h"
#include "tree_mechanics.h"
#include "tree_profile.h"
#include "tree_skin.h"

typedef enum TreeBuildStage {
    BUILD_STAGE_RESOLVE = 0,
    BUILD_STAGE_GROW_SHOOTS,
    BUILD_STAGE_GROW_ROOTS,
    BUILD_STAGE_MECHANICS,
    BUILD_STAGE_SKIN,
    BUILD_STAGE_FOLIAGE,
    BUILD_STAGE_FINALISE,
    BUILD_STAGE_VALIDATE,
    BUILD_STAGE_ACCELERATE,
    BUILD_STAGE_STAGES,
    BUILD_STAGE_COUNT
} TreeBuildStage;

const char *tree_build_stage_name(TreeBuildStage s);

/* Called between passes and, during growth, once per growth step. `fraction` is
 * within the stage, not overall: a caller that wants an overall bar should weight
 * the stages itself, because the weights depend on the tree and pretending
 * otherwise would produce a progress bar that lies. */
typedef void (*TreeBuildProgress)(TreeBuildStage stage, f32 fraction, void *user);

typedef struct TreeBuildOptions {
    bool build_foliage;
    bool build_bvh;
    bool validate_mesh;
    /* Sections the BVH covers. Picking wood only is much cheaper than picking
     * everything and then filtering, and it is what an inspection cursor wants by
     * default. */
    u32  bvh_section_mask;
    u64  validate_scratch_bytes;   /* 0 => the validator's own default          */
    TreeBuildProgress progress;
    void *progress_user;
    GrowthCancel *cancel;
} TreeBuildOptions;

TreeBuildOptions tree_build_default_options(void);

/* One entry per growth step, derived from the finished graph. Cumulative, because
 * the question a replay asks is "what existed at year N", not "what changed". */
typedef struct TreeConstructionStage {
    u16 step;
    u32 organs;           /* organs in existence at the end of this step       */
    u32 living_organs;    /* of those, still alive at the end of the run       */
    u32 axes;
    f32 height_m;         /* tallest living shoot tip at this step             */
    f32 crown_radius_m;
    f32 wood_volume_m3;   /* of the wood that exists by this step              */
} TreeConstructionStage;

typedef struct Tree {
    TreeSettings       settings;
    TreeResolved       resolved;
    TreeGraph          graph;
    Mesh               mesh;
    MeshBvh            bvh;

    GrowthResult       growth;
    MechanicsResult    mechanics;
    SkinResult         skin;
    FoliageResult      foliage;
    MeshValidateReport validation;

    bool mesh_valid;         /* validation ran AND passed                     */
    bool mesh_validated;     /* validation ran at all                         */
    bool cancelled;
    TreeBuildStage failed_stage;

    TreeConstructionStage *stage;
    u32                    stage_count;
    u64                    stage_bytes;

    /* Folds the settings, the resolved individual and the finished geometry into
     * one value. Equality is a strong statement: two trees with the same
     * fingerprint have the same vertices, in the same order, with the same organ
     * attribution. */
    u64 fingerprint;
} Tree;

/* Builds everything. On failure the partially built tree is returned with
 * `failed_stage` set, and tree_free must still be called: a caller that has to
 * distinguish "nothing was allocated" from "some of it was" will get it wrong. */
TgResult tree_build(const TreeSettings *settings, const TreeBuildOptions *options,
                    Tree *out);

void tree_free(Tree *t);

/* The stage entry covering a given growth step, or NULL. */
const TreeConstructionStage *tree_stage_at(const Tree *t, u32 step);

#endif /* TG_TREE_BUILD_H */
