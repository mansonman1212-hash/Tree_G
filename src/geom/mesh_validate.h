/* mesh_validate.h -- the gate every generated tree must pass.
 *
 * Layer 1. Portable C17.
 *
 * A tree that fails validation is NEVER displayed. The seed and settings are
 * preserved, a specific diagnostic is produced, and the previously valid tree
 * remains live. Silent continuation is not an option.
 *
 * What is checked, and why each check exists:
 *
 *  structural   index/triangle array agreement, in-range indices, section spans
 *               that are contiguous, non-overlapping, and cover every triangle.
 *               Catches bookkeeping errors that would make triangle IDs -- and
 *               therefore the whole inspection UI -- lie.
 *
 *  numeric      finite positions, normals, tangents, parameters; unit-length
 *               normals; AO within range. Catches NaN propagation from a
 *               degenerate growth direction or an exploded mechanics solve
 *               before it reaches the GPU.
 *
 *  geometric    no repeated indices, no zero-area triangles measured against a
 *               scale-relative threshold. An absolute epsilon would wrongly
 *               reject legitimate fine leaf-vein geometry.
 *
 *  topological  every edge of a section declared closed is shared by exactly two
 *               triangles with opposite directions. This single test detects
 *               junction cracks, root-junction cracks, duplicated internal
 *               branch tubes, T-junctions and unintended open ends.
 *
 *  orientation  each closed component encloses positive signed volume, which
 *               proves the winding faces outward. Checked PER COMPONENT, not
 *               summed over the section, so that one inside-out leaf among
 *               thousands cannot be masked by its neighbours.
 *
 *  duplicates   no two triangles with the same vertex set.
 *
 * NOT checked here: general self-intersection, and the tree-level radius and
 * taper invariants. Self-intersection needs the BVH and is reported as
 * unchecked rather than quietly assumed. Radius/taper are properties of the
 * biological graph and are validated by the tree layer.
 */
#ifndef TG_MESH_VALIDATE_H
#define TG_MESH_VALIDATE_H

#include "mesh.h"

#define MESH_VALIDATE_MAX_EXAMPLES 8

typedef enum MeshIssueKind {
    MESH_ISSUE_NONE = 0,
    MESH_ISSUE_INDEX_OUT_OF_RANGE,
    MESH_ISSUE_ARRAY_MISMATCH,
    MESH_ISSUE_SECTION_SPAN,
    MESH_ISSUE_NON_FINITE_POSITION,
    MESH_ISSUE_NON_FINITE_NORMAL,
    MESH_ISSUE_NON_FINITE_TANGENT,
    MESH_ISSUE_NON_FINITE_PARAM,
    MESH_ISSUE_NORMAL_NOT_UNIT,
    MESH_ISSUE_AO_OUT_OF_RANGE,
    MESH_ISSUE_REPEATED_INDEX,
    MESH_ISSUE_ZERO_AREA,
    MESH_ISSUE_BOUNDARY_EDGE,       /* hole in a section required to be closed */
    MESH_ISSUE_NON_MANIFOLD_EDGE,   /* edge shared by more than two triangles  */
    MESH_ISSUE_INCONSISTENT_WINDING,/* both uses of an edge run the same way   */
    MESH_ISSUE_INVERTED_COMPONENT,  /* closed component with negative volume   */
    MESH_ISSUE_DUPLICATE_TRIANGLE,
    MESH_ISSUE_DEGENERATE_BOUNDS,
    MESH_ISSUE_SCRATCH_LIMIT,       /* mesh too large to validate topology     */
    MESH_ISSUE_KIND_COUNT
} MeshIssueKind;

const char *mesh_issue_name(MeshIssueKind k);

typedef struct MeshIssue {
    MeshIssueKind kind;
    MeshSection   section;
    u64           count;     /* how many occurrences of this kind          */
    u64           example[MESH_VALIDATE_MAX_EXAMPLES]; /* ids for triage   */
    u32           example_count;
} MeshIssue;

typedef struct MeshValidateReport {
    bool      passed;
    u32       issue_count;
    MeshIssue issue[MESH_ISSUE_KIND_COUNT * MESH_SECTION_COUNT];

    /* Positive findings, useful even on success. */
    u64 checked_triangles;
    u64 checked_vertices;
    u32 closed_components[MESH_SECTION_COUNT];
    u64 boundary_edges[MESH_SECTION_COUNT];
    f64 enclosed_volume[MESH_SECTION_COUNT]; /* cubic metres, closed sections */
    f64 total_area[MESH_SECTION_COUNT];      /* square metres                */
    f32 min_triangle_area;
    f32 max_triangle_area;
    bool self_intersection_checked;          /* always false for now, honestly */
} MeshValidateReport;

typedef struct MeshValidateOptions {
    /* Triangles whose area is below (scale_relative_area_eps * bounds_diagonal^2)
     * are rejected. Scale-relative so that leaf-vein triangles are not
     * misjudged against trunk-sized geometry. */
    f32 scale_relative_area_eps;
    /* Allowed deviation of |normal| from 1. */
    f32 normal_length_tolerance;
    /* Upper bound on scratch bytes for topology checking. Exceeding it reports
     * MESH_ISSUE_SCRATCH_LIMIT rather than attempting a huge allocation. */
    u64 max_scratch_bytes;
    /* Skip the O(n log n) topology pass. Only for a fast in-loop smoke check;
     * the finalisation gate must never set this. */
    bool skip_topology;
} MeshValidateOptions;

MeshValidateOptions mesh_validate_default_options(void);

/* Runs every applicable check. Returns TG_OK when the mesh passes,
 * TG_ERR_VALIDATION_FAILED when it does not, and a specific error only when
 * validation itself could not be carried out. The report is always filled in. */
TgResult mesh_validate(const Mesh *m, const MeshValidateOptions *opt,
                       MeshValidateReport *report);

/* Writes the report to the log: one line per issue with counts and example ids,
 * plus a summary line. Called on failure by the generation pipeline. */
void mesh_validate_log_report(const MeshValidateReport *report);

#endif /* TG_MESH_VALIDATE_H */
