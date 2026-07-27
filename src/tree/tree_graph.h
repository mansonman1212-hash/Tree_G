/* tree_graph.h -- the persistent biological graph.
 *
 * Layer 2. Portable C17.
 *
 * This is the authoritative representation of the tree. The mesh is DERIVED from
 * it, never the other way round. The project requires the graph to remain
 * available after meshing for inspection, debugging, reproducibility, education,
 * organ selection and future extension, so it is not discarded once triangles
 * exist.
 *
 * KEY STRUCTURAL INVARIANT: PARENT ID < CHILD ID
 *
 *   Organs are only ever appended, and a child is always created after its
 *   parent. Therefore ascending organ id is a valid ACROPETAL (base to tip)
 *   topological order, and descending id is a valid BASIPETAL (tip to base)
 *   order. The Borchert-Honda style resource passes and the pipe-model radius
 *   pass both need exactly these orders, and this invariant makes them a plain
 *   loop instead of a sort or a recursive walk. It is checked by
 *   tree_graph_validate, so it cannot silently decay.
 *
 * AXES VERSUS ORGANS
 *
 *   An ORGAN is one internode, bud, leaf or scar. An AXIS is a chain of organs
 *   produced by one apical meristem: a trunk, a branch, a root. Axes exist
 *   because almost every botanical rule is expressed per axis (apical control,
 *   set angle, order, rhythmic flushes) and because the meshing stage sweeps
 *   cross-sections along an axis, which requires its organs to be contiguous and
 *   ordered.
 *
 * NO LONG-LIVED POINTERS
 *
 *   Both arrays grow during generation. Everything refers to organs and axes by
 *   u32 id. Accessors bounds-check in debug builds. This is a project-wide rule.
 */
#ifndef TG_TREE_GRAPH_H
#define TG_TREE_GRAPH_H

#include "../core/hash.h"
#include "../core/math3d.h"
#include "../core/mem.h"

typedef enum OrganType {
    ORGAN_NONE = 0,
    /* Structural internodes. Distinguished because they differ in meshing,
     * material and inspection reporting, not merely in a numeric order. */
    ORGAN_TRUNK_SEGMENT,
    ORGAN_BRANCH_SEGMENT,
    ORGAN_TWIG_SEGMENT,
    ORGAN_ROOT_SEGMENT,
    /* Meristems. Retained after they break so that bud-scale scars and the
     * position of past growth remain known. */
    ORGAN_BUD_TERMINAL,
    ORGAN_BUD_AXILLARY,
    /* Foliage. */
    ORGAN_PETIOLE,
    ORGAN_LEAF,
    ORGAN_NEEDLE,
    ORGAN_FASCICLE,
    /* Historical marks. Each records an event, which is what makes the tree read
     * as having a past rather than being freshly authored. */
    ORGAN_LEAF_SCAR,
    ORGAN_BRANCH_SCAR,
    ORGAN_KNOT,
    ORGAN_WOUND,
    ORGAN_TYPE_COUNT
} OrganType;

const char *organ_type_name(OrganType t);
/* True for the internode types that carry a swept woody surface. */
bool organ_type_is_segment(OrganType t);
bool organ_type_is_woody(OrganType t);

typedef enum AxisKind {
    AXIS_ORTHOTROPIC = 0,  /* corrects toward vertical: trunk, leaders        */
    AXIS_PLAGIOTROPIC,     /* holds a set angle: branch tiers, most laterals  */
    AXIS_ROOT,             /* positively gravitropic, independent parameters   */
    AXIS_KIND_COUNT
} AxisKind;

const char *axis_kind_name(AxisKind k);

/* Organ state. Flags rather than an enum because several are simultaneously
 * true: a shoot can be alive, dormant and damaged at once. */
enum {
    ORGAN_FLAG_ALIVE        = 1u << 0,
    ORGAN_FLAG_DORMANT      = 1u << 1,  /* bud not yet broken                 */
    ORGAN_FLAG_SUPPRESSED   = 1u << 2,  /* living but light-starved           */
    ORGAN_FLAG_DEAD         = 1u << 3,
    ORGAN_FLAG_BARK_RETAINED= 1u << 4,  /* recently dead: bark still on       */
    ORGAN_FLAG_SHED         = 1u << 5,  /* self-pruned: only a scar remains   */
    ORGAN_FLAG_BROKEN       = 1u << 6,  /* mechanical failure, splintered end */
    ORGAN_FLAG_OCCLUDED     = 1u << 7,  /* grown over by the parent           */
    ORGAN_FLAG_TERMINAL     = 1u << 8,  /* apical position on its axis        */
    ORGAN_FLAG_LEADER       = 1u << 9,  /* the tree's main leader             */
    ORGAN_FLAG_EPICORMIC    = 1u << 10, /* from a dormant/adventitious bud    */
    ORGAN_FLAG_REACTION_WOOD= 1u << 11, /* eccentric growth recorded here     */
    ORGAN_FLAG_DAMAGED      = 1u << 12,
    ORGAN_FLAG_CO_DOMINANT  = 1u << 13, /* one of a pair of competing leaders */
    ORGAN_FLAG_INCLUDED_BARK= 1u << 14  /* narrow union with trapped bark     */
};

/* One organ. 128 bytes, statically asserted, deliberately dense: a mature
 * reference tree can hold over a million of these and cache behaviour during the
 * resource passes dominates generation time.
 *
 * The tip position is NOT stored: it is base + direction * length. Storing it
 * would be a second source of truth that could disagree after the mechanics pass
 * bends an axis. */
typedef struct Organ {
    u32 id;
    u32 parent;         /* TG_INVALID_ID for the root organ                   */
    u32 first_child;
    u32 last_child;     /* kept so appending a child is O(1)                  */
    u32 next_sibling;
    u32 axis;           /* owning axis id                                     */

    u16 index_in_axis;
    u8  type;           /* OrganType                                          */
    u8  branch_order;   /* 0 = trunk / primary root                           */
    u16 created_step;   /* growth step in which this organ appeared           */
    u16 death_step;     /* step it died, or 0xFFFF if alive                   */
    u32 flags;

    V3  base;           /* proximal end, world space, metres                  */
    V3  direction;      /* unit, proximal -> distal                           */
    /* Rotation-minimising reference normal at the base, ALWAYS unit and ALWAYS
     * perpendicular to `direction`, for every organ type without exception.
     * Carried in the graph so meshing cannot introduce twist and the educational
     * view can show the true frames. Its meaning never changes between organ
     * types -- an overloaded field here previously produced organs whose frame
     * was perpendicular to their PARENT instead of to themselves. */
    V3  frame_ref;

    f32 length;
    f32 radius_base;
    f32 radius_tip;
    f32 curvature;      /* signed bend applied by the mechanics pass, radians */
    f32 torsion;        /* deliberate twist applied on top of the RMF         */
    /* Reaction-wood eccentricity as a fraction of the radius, 0 to <0.5. The
     * SIDE it applies to is not stored: it follows from the profile's reaction
     * wood type (upper in angiosperms, lower in gymnosperms) and the segment's
     * own direction relative to gravity, so storing it would be a second source
     * of truth that could disagree. */
    f32 eccentricity;
    f32 vigor;          /* resource received this step                        */
    f32 light;          /* accumulated light exposure, 0..1                   */
    f32 supported_leaf_area;  /* m^2 distal to and including this organ       */
    f32 supported_mass;       /* kg distal to and including this organ        */
    f32 physiological_age;    /* years of cambial activity at the base        */

    /* Provenance into the mesh, filled during meshing. Lets the inspection UI
     * go from an organ to its triangles and back. */
    u32 mesh_first_index;
    u32 mesh_index_count;

    /* Attachment placement on the parent's surface. Meaningful only for
     * non-segment organs; zero for segments. Stored explicitly rather than
     * reusing curvature/frame_ref, because a field whose meaning depends on the
     * organ type is a defect waiting to happen. */
    f32 attach_along;   /* 0..1 along the parent internode                    */
    f32 attach_angle;   /* phyllotactic angle in the PARENT's frame, radians  */

    u32 _pad[3];
} Organ;

/* 144 bytes rather than a tighter 132: the struct is kept 16-byte aligned, and
 * the explicit padding is a deliberate place for the next per-organ quantity
 * (bark maturity, wound depth) so adding one does not silently change the size
 * and invalidate every recorded fingerprint without notice. */
TG_STATIC_ASSERT(sizeof(Organ) == 144, "Organ must be exactly 144 bytes");

typedef struct Axis {
    u32 id;
    u32 parent_axis;    /* TG_INVALID_ID for the trunk                        */
    u32 parent_organ;   /* organ this axis is inserted on                     */
    u32 first_organ;    /* first organ id of the axis                         */
    u32 last_organ;
    u32 organ_count;
    u8  kind;           /* AxisKind                                           */
    u8  order;          /* branch order                                       */
    u16 created_step;
    u32 flags;          /* same ORGAN_FLAG_* vocabulary where meaningful       */
    f32 total_length;
    f32 set_angle;      /* radians from vertical this axis tends toward        */
    V3  tip_direction;  /* current growth direction of the apical meristem     */
    V3  tip_position;
    f32 accumulated_light;
    u32 _pad;
} Axis;

/* Not a round number by design: Axis is not GPU-visible and is allocated in the
 * hundreds of thousands at most, so packing it further would buy nothing. The
 * assertion exists to catch an ACCIDENTAL layout change, not to enforce a size. */
TG_STATIC_ASSERT(sizeof(Axis) == 72, "Axis layout changed unexpectedly");

typedef struct TreeGraph {
    TgArray organs;  /* Organ */
    TgArray axes;    /* Axis  */
    u32 trunk_axis;
    u32 root_axis_first; /* first root axis id, TG_INVALID_ID if none         */
    u32 max_organs;      /* hard ceiling; exceeding it is a reported failure  */
    bool finalized;
} TreeGraph;

TgResult tree_graph_init(TreeGraph *g, u32 reserve_organs, u32 max_organs);
void     tree_graph_destroy(TreeGraph *g);
void     tree_graph_reset(TreeGraph *g);
/* Marks the graph immutable. Every mutating entry point refuses afterwards,
 * which is half of the "the tree never changes during inspection" guarantee. */
void     tree_graph_finalize(TreeGraph *g);

static inline u32 tree_graph_organ_count(const TreeGraph *g) {
    return (u32)g->organs.count;
}
static inline u32 tree_graph_axis_count(const TreeGraph *g) {
    return (u32)g->axes.count;
}

/* Read accessors. Return NULL for TG_INVALID_ID so that walking to a missing
 * parent is a checkable condition rather than undefined behaviour. */
const Organ *tree_graph_organ(const TreeGraph *g, u32 id);
const Axis  *tree_graph_axis(const TreeGraph *g, u32 id);
/* Mutable access during generation only; asserts if finalised. */
Organ *tree_graph_organ_mut(TreeGraph *g, u32 id);
Axis  *tree_graph_axis_mut(TreeGraph *g, u32 id);

/* Distal end of an organ, derived rather than stored. */
V3 organ_tip(const Organ *o);

/* --------------------------------------------------------------------------
 * Construction
 * -------------------------------------------------------------------------- */

/* Creates an axis. `parent_organ` may be TG_INVALID_ID for the trunk. */
TgResult tree_graph_add_axis(TreeGraph *g, u32 parent_axis, u32 parent_organ,
                             AxisKind kind, u8 order, u16 step, f32 set_angle,
                             V3 tip_position, V3 tip_direction, u32 *out_axis);

/* Appends a segment to the END of an axis, extending it distally.
 *
 * The RMF reference normal is propagated from the previous segment of the same
 * axis using the double-reflection method, so the whole axis carries a
 * twist-free frame by construction. For the first segment of an axis, the frame
 * is seeded from the parent organ's frame, which keeps bark grain continuous
 * across a branch union instead of restarting arbitrarily. */
TgResult tree_graph_add_segment(TreeGraph *g, u32 axis, OrganType type,
                               V3 base, V3 direction, f32 length, u16 step,
                               u32 *out_organ);

/* Attaches a non-segment organ (bud, leaf, scar, knot, wound) to a parent
 * organ. `local_angle` is the phyllotactic angle around the parent axis, used
 * later to place the organ on the parent's surface. */
TgResult tree_graph_add_attachment(TreeGraph *g, u32 parent_organ, OrganType type,
                                  f32 along, f32 local_angle, V3 direction,
                                  f32 length, u16 step, u32 *out_organ);

/* --------------------------------------------------------------------------
 * Traversal
 * -------------------------------------------------------------------------- */

/* Acropetal (base to tip) is ascending id; basipetal is descending. Provided as
 * named helpers so call sites document which direction they need, and so a
 * future change of representation has a single place to adapt. */
static inline u32 tree_graph_acropetal_first(const TreeGraph *g) {
    TG_UNUSED(g);
    return 0;
}
static inline u32 tree_graph_acropetal_count(const TreeGraph *g) {
    return tree_graph_organ_count(g);
}

/* Marks an organ dead, and optionally everything distal to it. Returns the
 * number of organs whose state changed. Self-pruning and storm damage both go
 * through here so that the recorded death step is always consistent. */
u32 tree_graph_kill_subtree(TreeGraph *g, u32 organ, u16 step, u32 extra_flags);

/* Sums a per-organ contribution from the tips toward the base in one pass, using
 * the id ordering invariant. `leaf_area_of` supplies the local contribution.
 * This is the primitive behind both the pipe-model radius pass and the
 * supported-mass calculation. */
void tree_graph_accumulate_basipetal(TreeGraph *g,
                                     f32 (*local_leaf_area)(const Organ *, void *),
                                     void *user);

/* --------------------------------------------------------------------------
 * Validation and reporting
 * -------------------------------------------------------------------------- */
typedef enum GraphIssueKind {
    GRAPH_ISSUE_NONE = 0,
    GRAPH_ISSUE_BAD_PARENT_ID,
    GRAPH_ISSUE_PARENT_NOT_BEFORE_CHILD, /* breaks the ordering invariant     */
    GRAPH_ISSUE_CHILD_LIST_BROKEN,
    GRAPH_ISSUE_SIBLING_CYCLE,
    GRAPH_ISSUE_BAD_AXIS_ID,
    GRAPH_ISSUE_AXIS_NOT_CONTIGUOUS,
    GRAPH_ISSUE_AXIS_COUNT_MISMATCH,
    GRAPH_ISSUE_ORDER_NOT_MONOTONE,      /* child order < parent order        */
    GRAPH_ISSUE_NON_FINITE,
    GRAPH_ISSUE_NON_UNIT_DIRECTION,
    GRAPH_ISSUE_NON_POSITIVE_LENGTH,
    GRAPH_ISSUE_NON_POSITIVE_RADIUS,
    GRAPH_ISSUE_RADIUS_INCREASES_DISTALLY,
    GRAPH_ISSUE_CHILD_THICKER_THAN_PARENT,
    GRAPH_ISSUE_DISCONNECTED_SEGMENT,    /* base not at the parent's tip      */
    GRAPH_ISSUE_FRAME_NOT_ORTHOGONAL,
    GRAPH_ISSUE_DEAD_BEFORE_BORN,
    GRAPH_ISSUE_KIND_COUNT
} GraphIssueKind;

const char *graph_issue_name(GraphIssueKind k);

#define GRAPH_VALIDATE_MAX_EXAMPLES 8

typedef struct GraphIssue {
    GraphIssueKind kind;
    u64 count;
    u32 example[GRAPH_VALIDATE_MAX_EXAMPLES];
    u32 example_count;
} GraphIssue;

typedef struct GraphValidateReport {
    bool passed;
    u32  issue_count;
    GraphIssue issue[GRAPH_ISSUE_KIND_COUNT];

    u32 organs;
    u32 axes;
    u32 segments;
    u32 buds;
    u32 leaves;
    u32 dead_organs;
    u32 max_branch_order;
    u32 order_histogram[8];
    f32 total_axis_length;
    f32 max_height;
    f32 min_root_depth;
    Aabb bounds;
} GraphValidateReport;

/* `connection_tolerance` is the largest gap permitted between a segment's base
 * and its parent's tip, in metres. Pass 0 to use a scale-derived default. */
TgResult tree_graph_validate(const TreeGraph *g, f32 connection_tolerance,
                             GraphValidateReport *report);
void     tree_graph_log_report(const GraphValidateReport *report);

/* Folds everything that must be reproducible into a fingerprint: topology,
 * geometry, radii, states, ages and axis structure. Used by the determinism and
 * static-object tests. */
TgFingerprint tree_graph_fingerprint(const TreeGraph *g);

#endif /* TG_TREE_GRAPH_H */
