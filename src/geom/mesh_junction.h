/* mesh_junction.h -- welded branch unions built from a local implicit field.
 *
 * Layer 1. Portable C17.
 *
 * WHAT THIS SOLVES
 *   A branch union is grown tissue, not two intersecting cylinders. Until this
 *   module existed, every union in the tree was two closed tubes pushed through
 *   one another -- the "plumbing" appearance that docs/research.md rejects
 *   explicitly, and the largest single realism defect remaining in the geometry.
 *
 * THE METHOD, AND WHY THIS ONE
 *   docs/research.md section 3 fixes the construction: evaluate a smooth-union
 *   scalar field over the participating tapered generalised cylinders, extract a
 *   manifold surface patch, and stitch it to the limb cross-section rings with
 *   matching ring topology. The implicit field is a construction device only; what
 *   is committed is explicit indexed triangles, available to the inspection system
 *   like any other surface.
 *
 *   Two alternatives were rejected there and remain rejected: boolean CSG on
 *   triangle meshes, which produces near-degenerate slivers at grazing unions and
 *   destroys the ring correspondence that bark flow needs; and pushing one cylinder
 *   through another, which is what this replaces.
 *
 * MARCHING TETRAHEDRA, NOT MARCHING CUBES
 *   This is the one design choice made here rather than in the research document,
 *   and it is made against the risk this module exists to close. Risk R1 in
 *   docs/architecture.md is that junction meshing produces cracks or NON-MANIFOLD
 *   EDGES at high-valence unions. Marching cubes has ambiguous face and interior
 *   cases whose mishandling is the textbook cause of exactly that, and correct
 *   handling needs a 256-entry table that is long enough to mistype silently.
 *
 *   A tetrahedral decomposition has four cases -- nothing, one triangle, two
 *   triangles, nothing -- no ambiguity to resolve, and no table. Every extracted
 *   vertex lies on a tetrahedron EDGE and is shared by every tetrahedron using that
 *   edge, so each interior edge of the output is used by exactly two triangles by
 *   construction rather than by argument. The cost is roughly twice the triangles of
 *   a marching-cubes patch, which is paid only at the unions coarse enough to be
 *   worth welding.
 *
 *   Kuhn's decomposition is used, which cuts every cube the same way, so
 *   neighbouring cubes agree on the diagonal of their shared face. That agreement is
 *   what makes the patch watertight ACROSS cells; a decomposition chosen per cube
 *   would crack along every cell boundary.
 *
 * WHAT THE CALLER OWNS
 *   This module does not know what a tree is. The caller supplies the limbs meeting
 *   at the union -- each a direction, a radius and an already-emitted boundary ring
 *   of mesh vertices -- and receives a welded patch joining them. The parent below
 *   the union and the parent above it are two ordinary limbs; there is no special
 *   case for "the parent", which is what lets one code path serve a simple union and
 *   a six-child conifer whorl.
 */
#ifndef TG_MESH_JUNCTION_H
#define TG_MESH_JUNCTION_H

#include "mesh.h"

/* Hard ceiling on limbs at one union. A conifer whorl is five laterals plus the
 * leader below and above, so seven is real; the adversarial fixture the risk
 * register demands is a six-child union, which is eight. */
#define MESH_JUNCTION_MAX_LIMBS 12

typedef struct JunctionLimb {
    /* Unit, pointing AWAY from the union along the limb's axis. */
    V3  dir;
    /* Radius where this limb leaves the union, and where it is cut. A cone is
     * fitted between the two so the field tapers as the limb does; a cylinder
     * would make a tapering limb bulge at the cut. */
    f32 radius_inner;
    f32 radius_outer;
    /* The boundary ring the patch must be sewn to: mesh vertex indices in
     * consistent order around the limb. Ownership stays with the caller. */
    const u32 *ring;
    u32        ring_count;
    u32        organ_id;
} JunctionLimb;

typedef struct JunctionSpec {
    V3  centre;
    /* Distance from the centre at which every limb is cut, which is also where the
     * caller must have placed its boundary ring. One distance for all limbs, not
     * one each: the cut planes then form a convex region containing the ball of that
     * radius, so no limb's plane can slice another limb short of its own ring. */
    f32 limb_length;
    /* Smooth-union radius. This is what produces the branch collar -- the fillet is
     * the collar, not a decoration added afterwards. */
    f32 blend;
    /* How far SHORT of limb_length the implicit patch is cut, leaving a band between
     * the patch edge and the caller's ring for the seam to occupy.
     *
     * This is not a fudge factor, it is load bearing. With the patch cut exactly at
     * the ring plane, the two loops being sewn lie on the same circle in the same
     * plane, so the seam is a zero-width annulus: every triangle in it is a sliver
     * whose normal is undefined, and the first implementation produced 62
     * inconsistent windings and 20 boundary edges from precisely that. Giving the
     * band real width makes every seam triangle a well-conditioned frustum face.
     * Zero means "use a sensible fraction of limb_length". */
    f32 seam_width;
    /* Cells per axis over the extraction box. Clamped to a sane range. */
    u32 grid;
    u32 material;      /* MeshMaterial for the patch                          */
    u32 organ_id;      /* triangle attribution for the patch itself           */
    u32 colour;        /* packed RGBA                                         */
    u16 birth_step;
    const JunctionLimb *limb;
    u32 limb_count;
} JunctionSpec;

typedef struct JunctionResult {
    u32 vertices_added;
    u32 triangles_added;
    u32 patch_triangles;      /* from the implicit surface                    */
    u32 seam_triangles;       /* from sewing the patch to the limb rings      */
    u32 loops_found;          /* open boundary loops on the extracted patch   */
    u32 loops_sewn;
    /* Loops that found no limb to sew to, or limb rings that no loop reached.
     * Either is a crack, so both are reported rather than being allowed to pass
     * as a successful build. */
    u32 loops_unmatched;
    u32 limbs_unmatched;
    u32 field_samples;
    u32 grid_used;
    bool degenerate;          /* the field produced no surface at all         */
    /* The limbs are still fused with one another at the clip radius, so there is no
     * separate exit per limb to sew to. Reported rather than papered over, because
     * the alternative is a ring left unreachable -- a hole the size of a branch. */
    bool not_separable;
    /* The grid had to be clamped below what the thinnest limb needs. */
    bool under_resolved;
    /* Per-limb diagnostics. A limb that attracted two loops, or a seam whose
     * triangle count differs from the sum of the two loop lengths, is a defect that
     * shows here as a number instead of as a validation failure with no explanation. */
    u32 loops_per_limb[MESH_JUNCTION_MAX_LIMBS];
    u32 seam_expected;
    /* Vertices that landed on a position already occupied and were merged. Expected
     * to be zero; a non-zero count is a real geometric degeneracy worth knowing
     * about, which is why it is reported rather than absorbed. */
    u32 coincident_vertices_merged;
    /* The extracted boundary was still pinched after every retry, so nothing was
     * emitted. The caller must fall back to unwelded tubes for this union. */
    bool pinched;
    u32  retries;
    u32  ring_duplicates;
    u32  diagonal_reuse;
} JunctionResult;

/* Appends the welded junction to `m`, which must be inside an open section.
 *
 * Returns TG_ERR_INVALID_ARGUMENT for a spec that cannot describe a union (fewer
 * than two limbs, a non-finite direction, a non-positive radius or length). Returns
 * TG_OK with `degenerate` set when the field is valid but encloses nothing, which a
 * caller should treat as "fall back to unwelded tubes" rather than as a failure.
 */
TgResult mesh_junction_build(Mesh *m, const JunctionSpec *spec,
                             JunctionResult *out);

/* PRECONDITIONS THE CALLER MUST SATISFY, computed rather than guessed.
 *
 * Both of these were found by measurement, not by reasoning ahead: a union whose
 * child leaves at a shallow angle, or whose child is nearly as thick as its parent,
 * produced a whole unsewn ring -- sixteen boundary edges, one per ring vertex --
 * because the two limbs had not yet parted company at the radius where the patch was
 * cut. A union meshed on too coarse a grid produced a stray second component for the
 * same underlying reason: the geometry it was asked to resolve was finer than a cell.
 *
 * Neither is a defect that can be fixed inside the extraction. They are conditions on
 * the request, so they are exposed as such: ask for the minimum, and either meet it
 * or do not weld this union.
 */

/* Smallest limb_length at which every pair of limbs has separated, so each has its
 * own exit through the clip sphere. Grows without bound as two limbs approach
 * parallel, which is correct: a shoot leaving its parent at ten degrees really is
 * fused to it for a long way. */
f32 mesh_junction_min_limb_length(const JunctionSpec *spec);

/* Smallest grid that resolves the thinnest limb at the requested limb_length. */
u32 mesh_junction_min_grid(const JunctionSpec *spec);

/* The implicit field itself, exposed so the tests can assert properties of the
 * surface independently of how it was tessellated -- that the fillet is where the
 * collar should be, that the field is negative inside every limb, and that it is
 * smooth across the union. Negative inside. */
f32 mesh_junction_field(const JunctionSpec *spec, V3 p);

#endif /* TG_MESH_JUNCTION_H */
