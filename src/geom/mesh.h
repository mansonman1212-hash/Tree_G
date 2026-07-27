/* mesh.h -- indexed triangle mesh with per-organ provenance.
 *
 * Layer 1. Portable C17.
 *
 * DESIGN DECISIONS AND WHY
 * ------------------------
 * 1. Explicit indexed triangles are the ONLY representation of visible detail.
 *    Implicit fields, subdivision and volumetric meshing may be used as
 *    construction devices, but what lands here is what the user sees and what
 *    the inspection system can address. There is no shader-only geometry.
 *
 * 2. Every triangle records the organ it belongs to. Organ type, branch order,
 *    growth step, physiological age and parent are NOT duplicated per triangle;
 *    they are looked up from the biological graph through organ_id. Duplicating
 *    them would double the memory and create two sources of truth that could
 *    disagree.
 *
 * 3. Triangles are appended in section order and sections are therefore
 *    contiguous. mesh_begin_section / mesh_end_section enforce this. The payoff
 *    is that draw ranges, validation ranges and construction-replay index spans
 *    are all plain [first, count) intervals with no reordering pass, which in
 *    turn means the triangle IDs the inspection UI reports are stable and equal
 *    to the generation order.
 *
 * 4. The mesh becomes immutable at finalisation. After that, only reads are
 *    permitted; mutating entry points assert. This is the mechanism behind the
 *    "tree never changes during inspection" requirement.
 */
#ifndef TG_MESH_H
#define TG_MESH_H

#include "../core/hash.h"
#include "../core/math3d.h"
#include "../core/mem.h"

/* --------------------------------------------------------------------------
 * Sections
 *
 * A section groups triangles that share validation expectations and render
 * state. The expectations are data, not scattered special cases: see
 * mesh_section_info().
 * -------------------------------------------------------------------------- */
typedef enum MeshSection {
    /* The continuous woody exterior: root flare, structural roots, trunk,
     * every branch order, and twigs. Required watertight and outward-oriented.
     * This single section is what makes the tree read as a solid object. */
    MESH_SECTION_WOOD = 0,

    /* Exposed dead wood inside wounds, breaks and cavities. Part of the outer
     * boundary of the solid, so it participates in the WOOD watertightness
     * check when present; kept separate because its material differs. */
    MESH_SECTION_EXPOSED_WOOD,

    /* Lifted and peeling bark plates. Shells with real thickness and a free
     * edge, so they are deliberately open at the tear line: NOT watertight, and
     * that is a design decision rather than a defect. */
    MESH_SECTION_BARK_FLAKE,

    MESH_SECTION_BUD,
    MESH_SECTION_PETIOLE,

    /* One closed thin solid per leaf: upper surface, lower surface, and a real
     * edge band. Closed manifold per component, but many components. */
    MESH_SECTION_LEAF,

    /* One closed thin solid per needle. */
    MESH_SECTION_NEEDLE,

    /* Fascicle sheath geometry at the base of a needle cluster. */
    MESH_SECTION_FASCICLE,

    /* Internal anatomy revealed only in cutaway mode. Open by construction: it
     * is a cut surface. */
    MESH_SECTION_CUTAWAY,

    MESH_SECTION_COUNT
} MeshSection;

typedef struct MeshSectionInfo {
    const char *name;
    /* Every edge must be shared by exactly two triangles with opposite
     * orientation. */
    bool require_closed_manifold;
    /* Enclosed signed volume must be positive, proving outward winding. Only
     * meaningful when require_closed_manifold is true. */
    bool require_outward_orientation;
    /* Opaque solid: safe to backface-cull. */
    bool backface_cull;
    /* Thin biological tissue: shaded two-sided with a transmission lobe. Never
     * alpha-blended. */
    bool two_sided;
} MeshSectionInfo;

const MeshSectionInfo *mesh_section_info(MeshSection s);

/* --------------------------------------------------------------------------
 * Material categories
 *
 * These select physically meaningful parameter sets. They are NOT texture
 * slots; the first implementation uses no image textures at all.
 * -------------------------------------------------------------------------- */
typedef enum MeshMaterial {
    MESH_MAT_BARK_YOUNG = 0,
    MESH_MAT_BARK_MATURE,
    MESH_MAT_BARK_FURROW,     /* fissure interior: darker, rougher, occluded */
    MESH_MAT_BARK_FLAKE,
    MESH_MAT_WOUNDWOOD,
    MESH_MAT_DEAD_WOOD,
    MESH_MAT_HEARTWOOD,
    MESH_MAT_SAPWOOD,
    MESH_MAT_CAMBIUM,
    MESH_MAT_PITH,
    MESH_MAT_ROOT,
    MESH_MAT_BUD_SCALE,
    MESH_MAT_PETIOLE,
    MESH_MAT_LEAF_UPPER,
    MESH_MAT_LEAF_LOWER,
    MESH_MAT_LEAF_EDGE,
    MESH_MAT_NEEDLE,
    MESH_MAT_COUNT
} MeshMaterial;

const char *mesh_material_name(MeshMaterial m);

/* Per-vertex flags. */
enum {
    MESH_VFLAG_SEAM        = 1u << 0, /* duplicated for a hard normal split   */
    MESH_VFLAG_FURROW      = 1u << 1, /* inside a bark fissure                */
    MESH_VFLAG_RIDGE       = 1u << 2, /* on a bark ridge crest                */
    MESH_VFLAG_COLLAR      = 1u << 3, /* branch-collar transition             */
    MESH_VFLAG_BARK_RIDGE  = 1u << 4, /* branch-bark ridge crest              */
    MESH_VFLAG_WOUND_MARGIN= 1u << 5, /* rolled woundwood lip                 */
    MESH_VFLAG_FREE_EDGE   = 1u << 6, /* open boundary of a peeling flake     */
    MESH_VFLAG_VEIN        = 1u << 7  /* raised leaf vein                     */
};

/* --------------------------------------------------------------------------
 * Vertex
 *
 * 64 bytes, explicitly padded and statically asserted. The layout is chosen for
 * a single interleaved GPU vertex buffer.
 *
 * `param` is NOT a texture coordinate -- the project uses no image textures. It
 * carries the surface's intrinsic parameterisation:
 *   woody surfaces : (normalised arc length along the axis, angle around it)
 *   leaves         : (blade longitudinal, blade transverse) in [0,1]
 * This is what lets analytic colour fields and debug visualisations follow the
 * grain instead of world space.
 * -------------------------------------------------------------------------- */
typedef struct MeshVertex {
    V3  position;  /* metres, model space                                    */
    V3  normal;    /* unit                                                   */
    V3  tangent;   /* unit, along the axis growth direction (bark grain)     */
    V2  param;     /* intrinsic surface parameters, see above                */
    u32 color;     /* packed RGBA8, per-vertex colour (not a texture)        */
    u32 organ_id;  /* index into the biological graph                        */
    u32 attrib;    /* material(8) | section(8) | flags(16)                   */
    f32 ao;        /* geometric ambient occlusion in [0,1], 1 = unoccluded   */
    /* Growth step at which the organ this vertex belongs to came into existence.
     *
     * This is what makes the construction replay a CLIP TEST rather than a rebuild.
     * The directive requires that the tree can be observed being constructed from
     * nothing; regenerating the mesh for every step of an eighty-step history would
     * cost eighty full generations, whereas carrying the birth step per vertex lets
     * a single static mesh be revealed progressively -- which also keeps the replay
     * honest, because it is provably the SAME geometry at every stage rather than a
     * sequence of separately generated trees.
     *
     * It occupies the slot that was explicit padding, so the stride is unchanged. */
    u32 birth_step;
} MeshVertex;

TG_STATIC_ASSERT(sizeof(MeshVertex) == 64, "MeshVertex must be exactly 64 bytes");
TG_STATIC_ASSERT(offsetof(MeshVertex, position) == 0, "position must be first");

static inline u32 mesh_pack_rgba(f32 r, f32 g, f32 b, f32 a) {
    u32 ri = (u32)(tg_saturatef(r) * 255.0f + 0.5f);
    u32 gi = (u32)(tg_saturatef(g) * 255.0f + 0.5f);
    u32 bi = (u32)(tg_saturatef(b) * 255.0f + 0.5f);
    u32 ai = (u32)(tg_saturatef(a) * 255.0f + 0.5f);
    return ri | (gi << 8) | (bi << 16) | (ai << 24);
}

static inline void mesh_unpack_rgba(u32 c, f32 *r, f32 *g, f32 *b, f32 *a) {
    const f32 inv = 1.0f / 255.0f;
    if (r) { *r = (f32)(c & 0xFFu) * inv; }
    if (g) { *g = (f32)((c >> 8) & 0xFFu) * inv; }
    if (b) { *b = (f32)((c >> 16) & 0xFFu) * inv; }
    if (a) { *a = (f32)((c >> 24) & 0xFFu) * inv; }
}

/* Scales a packed colour's RGB, leaving alpha alone. Used where the geometry
 * itself justifies a darkening -- a furrow floor is genuinely more occluded -- so
 * that the shading term and the geometry cannot disagree. */
static inline u32 mesh_scale_rgba(u32 c, f32 k) {
    u32 r = (u32)(tg_saturatef((f32)(c & 0xFFu) * (1.0f / 255.0f) * k) * 255.0f
                  + 0.5f);
    u32 g = (u32)(tg_saturatef((f32)((c >> 8) & 0xFFu) * (1.0f / 255.0f) * k)
                  * 255.0f + 0.5f);
    u32 b = (u32)(tg_saturatef((f32)((c >> 16) & 0xFFu) * (1.0f / 255.0f) * k)
                  * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (c & 0xFF000000u);
}

static inline u32 mesh_pack_attrib(MeshMaterial m, MeshSection s, u32 flags) {
    return ((u32)m & 0xFFu) | (((u32)s & 0xFFu) << 8) | ((flags & 0xFFFFu) << 16);
}
static inline MeshMaterial mesh_attrib_material(u32 a) {
    return (MeshMaterial)(a & 0xFFu);
}
static inline MeshSection mesh_attrib_section(u32 a) {
    return (MeshSection)((a >> 8) & 0xFFu);
}
static inline u32 mesh_attrib_flags(u32 a) { return (a >> 16) & 0xFFFFu; }

/* --------------------------------------------------------------------------
 * Mesh
 * -------------------------------------------------------------------------- */
/* Contiguous spans owned by one section. Contiguity is guaranteed by the
 * begin/end section discipline, which is what makes these plain intervals
 * instead of an indirection table. */
typedef struct MeshSectionSpan {
    u32 first_vertex;
    u32 vertex_count;
    u32 first_index;
    u32 index_count;
    u32 first_triangle;
    u32 triangle_count;
} MeshSectionSpan;

typedef struct Mesh {
    TgArray vertices; /* MeshVertex                                          */
    TgArray indices;  /* u32, 3 per triangle                                 */
    TgArray tri_organ;/* u32, one per triangle: authoritative provenance     */

    MeshSectionSpan span[MESH_SECTION_COUNT];
    bool            section_used[MESH_SECTION_COUNT];

    Aabb bounds;

    /* Section currently open for appending, or MESH_SECTION_COUNT if none. */
    MeshSection open_section;
    bool        finalized;

    /* Hard ceilings. Exceeding them is a reported, recoverable generation
     * failure, never an abort or a silent truncation. */
    u64 max_vertices;
    u64 max_triangles;
} Mesh;

/* Default ceilings. Sized so that the interleaved vertex buffer plus indices
 * stay within the arena's single-allocation cap with headroom. */
#define MESH_DEFAULT_MAX_VERTICES  ((u64)40000000)
#define MESH_DEFAULT_MAX_TRIANGLES ((u64)60000000)

TgResult mesh_init(Mesh *m, u64 reserve_vertices, u64 reserve_triangles);
void     mesh_destroy(Mesh *m);
/* Drops all content but keeps allocated capacity, so a regeneration does not
 * have to re-grow from nothing. */
void     mesh_reset(Mesh *m);

static inline u64 mesh_vertex_count(const Mesh *m) { return m->vertices.count; }
static inline u64 mesh_index_count(const Mesh *m) { return m->indices.count; }
static inline u64 mesh_triangle_count(const Mesh *m) { return m->tri_organ.count; }

static inline const MeshVertex *mesh_vertices(const Mesh *m) {
    return (const MeshVertex *)m->vertices.data;
}
static inline const u32 *mesh_indices(const Mesh *m) {
    return (const u32 *)m->indices.data;
}
static inline const u32 *mesh_tri_organs(const Mesh *m) {
    return (const u32 *)m->tri_organ.data;
}

/* Mutable vertex access during generation only. Asserts if the mesh is
 * finalised, which is how bark displacement is prevented from running after the
 * geometry has been frozen. */
MeshVertex *mesh_vertex_mut(Mesh *m, u64 index);

/* --------------------------------------------------------------------------
 * Section-scoped appending
 * -------------------------------------------------------------------------- */
TgResult mesh_begin_section(Mesh *m, MeshSection s);
TgResult mesh_end_section(Mesh *m);

/* Appends vertices, returning the index of the first. */
TgResult mesh_add_vertices(Mesh *m, const MeshVertex *v, u64 count, u32 *out_first);
TgResult mesh_add_vertex(Mesh *m, const MeshVertex *v, u32 *out_index);

/* Appends one triangle. Indices are absolute vertex indices and are validated
 * against the current vertex count immediately, so an out-of-range index is
 * caught at the point of the mistake rather than during validation. */
TgResult mesh_add_triangle(Mesh *m, u32 a, u32 b, u32 c, u32 organ_id);

/* Appends a quad as two triangles with consistent winding (a,b,c) + (a,c,d).
 * Cross-section stitching is the overwhelmingly common case, and doing it here
 * removes the most likely place to get winding wrong. */
TgResult mesh_add_quad(Mesh *m, u32 a, u32 b, u32 c, u32 d, u32 organ_id);

/* Stitches two equal-length rings of vertex indices into a tube wall.
 * `ring_a` is the proximal (basal) ring, `ring_b` the distal ring. With the
 * project's CCW-front convention and rings generated counter-clockwise about
 * the frame tangent, the emitted triangles face outward. */
TgResult mesh_stitch_rings(Mesh *m, const u32 *ring_a, const u32 *ring_b,
                           u32 ring_size, u32 organ_id);

/* Caps a ring with a triangle fan around `center`. `outward` selects the
 * winding: true emits triangles whose normals point along the ring's own
 * winding axis (distal tip cap), false reverses them (basal cap). */
TgResult mesh_cap_ring(Mesh *m, const u32 *ring, u32 ring_size, u32 center,
                       bool outward, u32 organ_id);

/* --------------------------------------------------------------------------
 * Finalisation
 * -------------------------------------------------------------------------- */

/* Recomputes bounds, releases surplus capacity, and marks the mesh immutable.
 * Fails if a section is still open. */
TgResult mesh_finalize(Mesh *m);

/* Angle-weighted vertex normals over the given section, computed from the actual
 * triangles. Angle weighting rather than area weighting is deliberate and is
 * justified in the implementation: it is invariant to how quads are split, which
 * matters because nearly all woody geometry here is quad-stitched rings and area
 * weighting produces normal error aligned with the diagonal.
 *
 * `hard_angle` in radians: adjacent faces meeting at a sharper angle should not
 * share smoothing, which is what keeps bark fracture edges crisp instead of
 * smearing them into a soft bulge.
 *
 * Division of responsibility: the generator owns topology and must already have
 * duplicated any vertex that needs a hard split (marking it MESH_VFLAG_SEAM);
 * this function only averages. It therefore does not silently fix a missing
 * split -- it counts the corners where a face normal deviates from its averaged
 * vertex normal by more than `hard_angle` and reports the count through
 * `out_hard_corner_violations`, so a generator bug surfaces as a number instead
 * of as a soft bulge on what should be a bark fracture. Pass NULL to ignore. */
TgResult mesh_compute_normals(Mesh *m, MeshSection s, f32 hard_angle,
                              u32 *out_hard_corner_violations);

/* Tangents from the intrinsic parameterisation, orthogonalised against the
 * final normals. Degenerate corners fall back to a deterministic perpendicular
 * rather than producing a zero or NaN tangent. */
TgResult mesh_compute_tangents(Mesh *m, MeshSection s);

/* --------------------------------------------------------------------------
 * Queries and fingerprints
 * -------------------------------------------------------------------------- */
typedef struct MeshTriangle {
    u32 index[3];    /* index-buffer values                                 */
    V3  position[3];
    V3  normal;      /* geometric face normal, unit                         */
    f32 area;
    u32 organ_id;
    MeshSection section;
    MeshMaterial material;
} MeshTriangle;

/* Fills a fully resolved triangle record for the inspection UI. Returns false
 * for an out-of-range triangle id. */
bool mesh_get_triangle(const Mesh *m, u64 tri_id, MeshTriangle *out);

/* Which section a triangle belongs to, derived from the recorded ranges. */
MeshSection mesh_triangle_section(const Mesh *m, u64 tri_id);

Aabb mesh_section_bounds(const Mesh *m, MeshSection s);

/* Geometry fingerprint used by the static-object regression test.
 *
 * Deliberately folds in ONLY data that must never change during inspection:
 * positions, normals, tangents, parameters, indices, per-triangle organ ids,
 * section ranges and bounds. Per-vertex AO is included because it is baked
 * geometry-derived data. Nothing view-dependent is included, because nothing
 * view-dependent is stored. */
TgFingerprint mesh_fingerprint(const Mesh *m);

/* Human-readable counts for the statistics panel. Every number here is real;
 * none is estimated. */
typedef struct MeshStats {
    u64 vertices;
    u64 triangles;
    u64 section_triangles[MESH_SECTION_COUNT];
    u64 cpu_bytes;
    u64 gpu_bytes_estimate;
    Aabb bounds;
} MeshStats;

MeshStats mesh_stats(const Mesh *m);

#endif /* TG_MESH_H */
