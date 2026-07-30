#include "mesh_junction.h"

#include "../core/log.h"
#include "../core/mem.h"

#include <math.h>
#include <string.h>

#define MJ_SUB "mesh_junction"

#define MJ_GRID_MIN 8u
#define MJ_GRID_MAX 64u
#define MJ_NO_CAP   0xFFu

/* ------------------------------------------------------------------------- */
/* The field                                                                 */
/*                                                                           */
/* Each limb is an EXACT round-cone distance function: a cone between two spheres,  */
/* one of radius_inner at the union centre and one of radius_outer at the cut.      */
/* Exact rather than approximate because the smooth minimum below is only a fillet   */
/* of the correct shape if what it blends really are distances -- feed it a          */
/* non-metric field and the collar's size stops corresponding to the blend radius.  */
/*                                                                           */
/* Spheres at both ends rather than flat caps: the inner sphere is what makes every  */
/* limb overlap solidly at the centre, so the union is one connected solid however   */
/* the limbs are arranged. A flat-capped cone whose cap plane missed the centre       */
/* would leave two limbs joined only by the blend, and a high-valence whorl would     */
/* come apart.                                                                      */
/* ------------------------------------------------------------------------- */
static f32 sd_round_cone(V3 p, V3 a, V3 b, f32 ra, f32 rb) {
    V3 ba = v3_sub(b, a);
    V3 pa = v3_sub(p, a);
    f32 l2 = v3_dot(ba, ba);
    f32 rr = ra - rb;
    f32 a2 = l2 - rr * rr;
    f32 il2, y, z, x2, y2, z2, k;

    if (!(l2 > 1e-12f)) {
        /* Degenerate axis: the cone collapses to a sphere. */
        return v3_len(pa) - tg_maxf(ra, rb);
    }
    il2 = 1.0f / l2;
    y = v3_dot(pa, ba);
    z = y - l2;
    {
        V3 q = v3_sub(v3_scale(pa, l2), v3_scale(ba, y));
        x2 = v3_dot(q, q);
    }
    y2 = y * y * l2;
    z2 = z * z * l2;
    k = ((rr < 0.0f) ? -1.0f : (rr > 0.0f ? 1.0f : 0.0f)) * rr * rr * x2;

    if (((z < 0.0f) ? -1.0f : 1.0f) * a2 * z2 > k) {
        return sqrtf(x2 + z2) * il2 - rb;
    }
    if (((y < 0.0f) ? -1.0f : 1.0f) * a2 * y2 < k) {
        return sqrtf(x2 + y2) * il2 - ra;
    }
    return (sqrtf(tg_maxf(x2 * a2 * il2, 0.0f)) + y * rr) * il2 - ra;
}

/* Quadratic polynomial smooth minimum.
 *
 * This is the collar. The fillet radius is the blend parameter, and the blend
 * parameter is set by the caller from the child radius, which is why a scaffold
 * limb gets a long shoulder and a twig gets almost none -- the same relation the
 * parametric collar in tree_skin uses, so the two agree where they meet. */
static f32 smooth_min(f32 a, f32 b, f32 k) {
    f32 h;
    if (!(k > 1e-6f)) { return tg_minf(a, b); }
    h = tg_saturatef(0.5f + 0.5f * (b - a) / k);
    return tg_lerpf(b, a, h) - k * h * (1.0f - h);
}

/* The smooth union alone, without the cut planes. Needed separately because a
 * vertex is classified as lying on a cut plane by comparing the two. */
static f32 field_smooth(const JunctionSpec *s, V3 p) {
    f32 d = 1.0e9f;
    u32 i;
    for (i = 0; i < s->limb_count; ++i) {
        const JunctionLimb *l = &s->limb[i];
        V3 b = v3_add(s->centre, v3_scale(l->dir, s->limb_length));
        f32 di = sd_round_cone(p, s->centre, b, l->radius_inner, l->radius_outer);
        d = (i == 0u) ? di : smooth_min(d, di, s->blend);
    }
    return d;
}

/* Where the implicit patch is cut along a limb: short of the caller's ring by the
 * seam width, so the sewn band has real width. */
static f32 cut_distance(const JunctionSpec *s) {
    f32 w = (s->seam_width > 0.0f) ? s->seam_width : s->limb_length * 0.18f;
    w = tg_clampf(w, s->limb_length * 0.04f, s->limb_length * 0.60f);
    return s->limb_length - w;
}

/* Signed distance to the outside of the clipping BALL. Positive outside.
 *
 * A ball, not one half-space per limb. Half-spaces were tried first and they fail at
 * exactly the valence this module exists to survive: the planes of two adjacent limbs
 * intersect, and where the blended web between those limbs is still solid at that
 * corner the clipped surface acquires a plane-plane edge that belongs to neither
 * limb's exit. Measured on the fixtures: three and four children produced a
 * non-manifold edge and a stray second component, five children produced nine
 * boundary loops for seven limbs and left two unsewn.
 *
 * A sphere has no corners, so that failure class does not exist. It is also what
 * docs/research.md describes -- a local field around the union -- rather than a
 * per-limb construction. */
static f32 clip_ball(const JunctionSpec *s, V3 p) {
    return v3_len(v3_sub(p, s->centre)) - cut_distance(s);
}

f32 mesh_junction_min_limb_length(const JunctionSpec *s) {
    f32 need = 0.0f;
    u32 i, j;
    TG_CHECK(s != NULL);
    for (i = 0; i < s->limb_count; ++i) {
        for (j = i + 1u; j < s->limb_count; ++j) {
            /* At radius R the two axes are 2 * R * sin(theta/2) apart. Their
             * surfaces are clear of one another once that exceeds the SUM of the two
             * radii, and the fillet stops bridging them once it exceeds that sum
             * plus the blend radius. Solved for R.
             *
             * The first form of this used the MEAN radius rather than the sum and
             * came out about 25% too strict, which rejected the ordinary union it was
             * written to admit -- a 0.55 radius ratio at 49 degrees. The geometry is
             * not a matter of taste: the chord between the two axes has to clear both
             * tubes, so it is the sum. */
            f32 c = tg_clampf(v3_dot(s->limb[i].dir, s->limb[j].dir), -1.0f, 1.0f);
            f32 half = acosf(c) * 0.5f;
            f32 sep = s->limb[i].radius_outer + s->limb[j].radius_outer + s->blend;
            f32 sh = sinf(half);
            f32 need_ij;
            if (sh < 1e-3f) { return 1.0e9f; }   /* parallel: never separates    */
            need_ij = sep / (2.0f * sh);
            if (need_ij > need) { need = need_ij; }
        }
    }
    /* The separation has to hold at the CLIP radius, which is short of limb_length
     * by the seam band, so the requirement on limb_length is larger by that band. */
    if (s->seam_width > 0.0f) { return need + s->seam_width; }
    return need / 0.82f;
}

u32 mesh_junction_min_grid(const JunctionSpec *s) {
    f32 min_r = 1.0e9f;
    f32 cell_needed, span;
    u32 i, g;
    TG_CHECK(s != NULL);
    for (i = 0; i < s->limb_count; ++i) {
        min_r = tg_minf(min_r, tg_minf(s->limb[i].radius_inner,
                                       s->limb[i].radius_outer));
    }
    if (!(min_r > 0.0f) || !(s->limb_length > 0.0f)) { return MJ_GRID_MIN; }
    /* Three cells across the thinnest limb's RADIUS. Fewer and the tube's wall is
     * decided by a single sample, which is where the stray components came from. */
    cell_needed = min_r / 3.0f;
    span = 2.0f * cut_distance(s) * 1.12f;
    g = (u32)(span / cell_needed + 0.999f);
    return tg_clamp_u32(g, MJ_GRID_MIN, MJ_GRID_MAX);
}

f32 mesh_junction_field(const JunctionSpec *s, V3 p) {
    TG_CHECK(s != NULL);
    /* Intersection of the smooth union with the clipping ball, so the solid ends on
     * a sphere just short of the rings the caller has already emitted. */
    return tg_maxf(field_smooth(s, p), clip_ball(s, p));
}

/* Is this triangle part of a flat cap on a cut plane?
 *
 * Decided from the triangle's CENTROID rather than by voting on which field term
 * dominated at each of its vertices. Per-vertex voting was tried and it classified
 * the cap rim inconsistently -- the vertex sits exactly where the two terms cross,
 * so rounding decides -- which left ragged islands of cap surviving as sixteen
 * separate inverted closed components. One test on one point, on the other hand,
 * either drops the triangle or keeps it.
 *
 * A cap triangle lies ON a cut plane and STRICTLY INSIDE the limb. The second half
 * matters: without it the rim, which also lies on the plane, would be dropped and
 * the loop that has to be sewn would go with it. */
static bool is_clip_face(const JunctionSpec *s, V3 centroid) {
    /* WHICH TERM IS ACTIVE, not how close the centroid is to the sphere.
     *
     * The surface satisfies max(smooth, clip) = 0, so at any point on it either
     * smooth is the zero and clip is negative -- a tube wall inside the ball -- or
     * clip is the zero and smooth is negative -- a face of the clipping sphere. The
     * larger term is therefore exactly the one that put the surface there, and the
     * comparison needs no tolerance at all.
     *
     * A band test was tried first: drop triangles whose centroid lies within a
     * fraction of a cell of the sphere. It was ragged, because the sphere cuts the
     * grid obliquely and the fraction is right for some orientations and wrong for
     * others. Ragged dropping leaves isolated holes, isolated holes give a boundary
     * vertex two outgoing edges, and the loop walk then fragments: measured, twelve
     * boundary loops on a three-limb union and twenty-nine closed components. A
     * partition by active term cannot be ragged -- it is a partition. */
    return clip_ball(s, centroid) > field_smooth(s, centroid);
}

/* Surface normal, from the SMOOTH UNION only -- never from the clipped field.
 *
 * The clip ball is a construction device: it decides where the patch ends and hands
 * the limb over to the caller's tube. It is not part of the surface. Taking the
 * gradient of max(smooth, clip) means that at the patch's boundary, where the clip
 * term is the active one, the "normal" comes out as the SPHERE's radial direction --
 * which points along the limb axis, nearly perpendicular to the actual surface.
 *
 * Nothing about the topology notices this. The mesh is watertight, manifold,
 * consistently wound and correct in volume either way, and the first version passed
 * every one of those checks. What it looked like was a dark sawtooth band around every
 * seam, alternating between the patch's wrong normals and the ring's correct radial
 * ones. It took a rendered capture to see it, which is why gate 8 of docs/testing.md
 * exists. */
static V3 field_gradient(const JunctionSpec *s, V3 p, f32 h) {
    f32 dx = field_smooth(s, v3(p.x + h, p.y, p.z))
           - field_smooth(s, v3(p.x - h, p.y, p.z));
    f32 dy = field_smooth(s, v3(p.x, p.y + h, p.z))
           - field_smooth(s, v3(p.x, p.y - h, p.z));
    f32 dz = field_smooth(s, v3(p.x, p.y, p.z + h))
           - field_smooth(s, v3(p.x, p.y, p.z - h));
    return v3_norm_or(v3(dx, dy, dz), v3(0.0f, 1.0f, 0.0f));
}

/* ------------------------------------------------------------------------- */
/* Edge-keyed vertex table                                                   */
/*                                                                           */
/* Open addressing on a power-of-two table. This is what makes the patch manifold:  */
/* a vertex is identified by the GRID EDGE it lies on, so every tetrahedron that     */
/* shares that edge finds the same vertex, and no two coincident vertices can be      */
/* emitted. Welding after the fact by position would have to guess a tolerance, and   */
/* on a surface with 0.2 mm features a guessed tolerance either fails to weld or      */
/* welds real detail away.                                                            */
/* ------------------------------------------------------------------------- */
typedef struct EdgeTable {
    u64 *key;      /* 0 = empty; otherwise 1 + packed edge                    */
    u32 *value;    /* local vertex index                                      */
    u32  mask;
    u64  bytes_key;
    u64  bytes_val;
} EdgeTable;

static u64 mix64(u64 x) {
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

static TgResult edge_table_init(EdgeTable *t, u32 capacity_hint) {
    u32 cap = 64u;
    memset(t, 0, sizeof *t);
    while (cap < capacity_hint * 2u && cap < (1u << 22)) { cap <<= 1; }
    if (!tg_ckd_mul_u64(cap, sizeof(u64), &t->bytes_key) ||
        !tg_ckd_mul_u64(cap, sizeof(u32), &t->bytes_val)) {
        return TG_ERR_OVERFLOW;
    }
    t->key = (u64 *)tg_alloc_zero(t->bytes_key);
    t->value = (u32 *)tg_alloc_zero(t->bytes_val);
    if (t->key == NULL || t->value == NULL) {
        if (t->key != NULL) { tg_free(t->key, t->bytes_key); }
        if (t->value != NULL) { tg_free(t->value, t->bytes_val); }
        memset(t, 0, sizeof *t);
        return TG_ERR_OUT_OF_MEMORY;
    }
    t->mask = cap - 1u;
    return TG_OK;
}

static void edge_table_free(EdgeTable *t) {
    if (t->key != NULL) { tg_free(t->key, t->bytes_key); }
    if (t->value != NULL) { tg_free(t->value, t->bytes_val); }
    memset(t, 0, sizeof *t);
}

/* Returns true if found; otherwise records `slot` for insertion. */
static bool edge_table_find(EdgeTable *t, u64 edge, u32 *out_value, u32 *out_slot) {
    u64 k = edge + 1u;
    u32 i = (u32)(mix64(k) & t->mask);
    u32 probes = 0;
    while (t->key[i] != 0u) {
        if (t->key[i] == k) { *out_value = t->value[i]; return true; }
        i = (i + 1u) & t->mask;
        if (++probes > t->mask) { break; }
    }
    *out_slot = i;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Build context                                                             */
/* ------------------------------------------------------------------------- */
typedef struct LocalVert {
    V3  p;
    u32 mesh_index;   /* TG_INVALID_ID until it is committed                  */
} LocalVert;

typedef struct Tri {
    u32 v[3];
    bool keep;
} Tri;

typedef struct JCtx {
    const JunctionSpec *spec;
    Mesh *mesh;
    u32 g;              /* cells per axis                                     */
    V3  origin;         /* grid corner                                        */
    f32 cell;
    f32 *val;           /* (g+1)^3 field samples                              */
    u64  val_bytes;
    LocalVert *vert;
    u32 vert_count, vert_cap;
    u64 vert_bytes;
    Tri *tri;
    u32 tri_count, tri_cap;
    u64 tri_bytes;
    EdgeTable edges;
    /* Position-keyed table over the SAME exact bits the mesh validator welds on.
     *
     * The edge table already guarantees one vertex per grid edge, and no two distinct
     * grid edges of Kuhn's decomposition meet except at a shared endpoint, so in
     * principle no two vertices can coincide. In practice one pair did, and the
     * consequence was invisible until validation: the validator welds by exact
     * position, so the two merged, a boundary loop then contained the same welded
     * vertex twice, its diagonal to the ring was emitted twice, and one edge ended up
     * used by four triangles.
     *
     * Rather than reason about which degenerate configuration produced it, the module
     * now adopts the validator's definition of identity. Whatever the cause, the
     * topology this module analyses is then the topology that will be validated -- and
     * any genuine pinch is caught by its own loop walk and reported, instead of
     * escaping as a silent non-manifold edge. */
    EdgeTable positions;
    u32 coincident_merged;
} JCtx;

static u32 grid_index(const JCtx *c, u32 x, u32 y, u32 z) {
    u32 n = c->g + 1u;
    return (z * n + y) * n + x;
}

static V3 grid_point(const JCtx *c, u32 x, u32 y, u32 z) {
    return v3(c->origin.x + (f32)x * c->cell,
              c->origin.y + (f32)y * c->cell,
              c->origin.z + (f32)z * c->cell);
}

/* Exact bit pattern of a position, folded to 64 bits. Not a tolerance: the
 * validator's weld is bit-exact, so this must be too. */
static u64 position_key(V3 p) {
    u32 bx, by, bz;
    memcpy(&bx, &p.x, sizeof bx);
    memcpy(&by, &p.y, sizeof by);
    memcpy(&bz, &p.z, sizeof bz);
    /* Signed zero folds to positive, matching the fingerprint's rule, so -0.0 and
     * 0.0 are the same point here as they are there. */
    if (bx == 0x80000000u) { bx = 0u; }
    if (by == 0x80000000u) { by = 0u; }
    if (bz == 0x80000000u) { bz = 0u; }
    return mix64(((u64)bx << 32) ^ (u64)by) ^ mix64((u64)bz * 0x9E3779B1ull);
}

static TgResult push_vert(JCtx *c, V3 p, u32 *out) {
    u32 slot = 0;
    if (edge_table_find(&c->positions, position_key(p), out, &slot)) {
        c->coincident_merged++;
        return TG_OK;
    }
    if (c->vert_count >= c->vert_cap) { return TG_ERR_LIMIT_EXCEEDED; }
    if (c->positions.key[slot] == 0u) {
        c->positions.key[slot] = position_key(p) + 1u;
        c->positions.value[slot] = c->vert_count;
    }
    c->vert[c->vert_count].p = p;
    c->vert[c->vert_count].mesh_index = TG_INVALID_ID;
    *out = c->vert_count++;
    return TG_OK;
}

static TgResult push_tri(JCtx *c, u32 a, u32 b, u32 d);

/* Emits a triangle wound so its normal points from INSIDE to OUTSIDE.
 *
 * `outward_ref` is any point on the outside of the surface local to this triangle;
 * for a tetrahedron case it is exactly known -- the lone outside vertex, or the
 * midpoint of the outside pair -- so no field evaluation and no tolerance are
 * involved.
 *
 * The first implementation instead flipped each finished triangle to agree with the
 * field gradient. That is ambiguous exactly where it matters: in the fillet and near
 * the cut planes the gradient turns quickly, and adjacent triangles sharing a
 * diagonal MUST traverse it in opposite directions, which independent per-triangle
 * decisions cannot guarantee. It produced nine inconsistent windings and six
 * inverted components. Deriving the orientation from which side is inside is exact,
 * local, and automatically consistent between neighbouring tetrahedra, because
 * inside and outside are global properties rather than local estimates. */
static TgResult push_tri_oriented(JCtx *c, u32 a, u32 b, u32 d, V3 inside_ref,
                                  V3 outward_ref) {
    V3 p0, p1, p2, nrm;
    if (a == b || b == d || a == d) { return TG_OK; }
    p0 = c->vert[a].p;
    p1 = c->vert[b].p;
    p2 = c->vert[d].p;
    nrm = v3_cross(v3_sub(p1, p0), v3_sub(p2, p0));
    if (v3_dot(nrm, v3_sub(outward_ref, inside_ref)) < 0.0f) {
        u32 t = b; b = d; d = t;
    }
    return push_tri(c, a, b, d);
}

static TgResult push_tri(JCtx *c, u32 a, u32 b, u32 d) {
    if (c->tri_count >= c->tri_cap) { return TG_ERR_LIMIT_EXCEEDED; }
    /* Degenerate triangles are dropped here rather than downstream. A tetrahedron
     * whose field values straddle zero at a shared vertex can produce two
     * coincident edge vertices, and a zero-area triangle is both invisible and a
     * validation failure. */
    if (a == b || b == d || a == d) { return TG_OK; }
    c->tri[c->tri_count].v[0] = a;
    c->tri[c->tri_count].v[1] = b;
    c->tri[c->tri_count].v[2] = d;
    c->tri[c->tri_count].keep = true;
    c->tri_count++;
    return TG_OK;
}

/* Vertex on the grid edge (ia, ib), created once and reused. */
static TgResult edge_vertex(JCtx *c, u32 ia, u32 ib, u32 *out) {
    u64 key;
    u32 slot = 0;
    u32 lo = (ia < ib) ? ia : ib;
    u32 hi = (ia < ib) ? ib : ia;
    f32 fa, fb, t;
    V3 pa, pb;
    u32 n = c->g + 1u;
    TgResult r;

    key = ((u64)lo << 32) | (u64)hi;
    if (edge_table_find(&c->edges, key, out, &slot)) { return TG_OK; }

    fa = c->val[lo];
    fb = c->val[hi];
    pa = grid_point(c, lo % n, (lo / n) % n, lo / (n * n));
    pb = grid_point(c, hi % n, (hi / n) % n, hi / (n * n));
    /* Linear interpolation to the zero crossing. Guarded because two samples of the
     * same sign must never reach here, and equal samples would divide by zero.
     *
     * NEVER placed exactly at either endpoint. When a grid sample sits almost on the
     * surface the interpolant rounds to 0 or 1, putting this vertex exactly on a grid
     * point -- and every other crossing edge meeting that same grid point does the
     * same, producing several distinct vertices at one bit-identical position. The
     * mesh validator welds by exact position, so those merge and the edges through
     * them end up used by four triangles: one non-manifold edge, which is what split
     * a six-limb union into two components.
     *
     * In Kuhn's decomposition no two distinct grid edges cross anywhere except at a
     * shared endpoint, so excluding the endpoints makes coincident vertices
     * impossible rather than unlikely. The cost is a displacement of a thousandth of
     * a cell, tens of microns, on the rare vertex that is affected. */
    t = (fa - fb != 0.0f) ? (fa / (fa - fb)) : 0.5f;
    t = tg_clampf(t, 1.0e-3f, 1.0f - 1.0e-3f);
    r = push_vert(c, v3_lerp(pa, pb, t), out);
    if (r != TG_OK) { return r; }
    /* A full probe chain would mean this edge's vertex is created again on the next
     * visit, silently duplicating it. The table is sized well above the surface's
     * vertex count, so reaching this is a bug rather than a load factor, and it is
     * reported as one. */
    if (c->edges.key[slot] != 0u) { return TG_ERR_LIMIT_EXCEEDED; }
    c->edges.key[slot] = key + 1u;
    c->edges.value[slot] = *out;
    return TG_OK;
}

/* Kuhn's decomposition: the six tetrahedra {0, a, a|b, 7} over the permutations of
 * the three bits. Every cube is cut identically, so a shared face carries the same
 * diagonal in both cubes and the patch cannot crack between cells. */
static const u8 kMjTets[6][4] = {
    { 0u, 1u, 3u, 7u }, { 0u, 1u, 5u, 7u },
    { 0u, 2u, 3u, 7u }, { 0u, 2u, 6u, 7u },
    { 0u, 4u, 5u, 7u }, { 0u, 4u, 6u, 7u }
};

static TgResult march_tet(JCtx *c, const u32 *gi) {
    f32 f[4];
    u32 inside[4], outside[4];
    u32 n_in = 0, n_out = 0;
    u32 k;
    TgResult r;

    for (k = 0; k < 4u; ++k) {
        f[k] = c->val[gi[k]];
        if (f[k] < 0.0f) { inside[n_in++] = k; } else { outside[n_out++] = k; }
    }
    if (n_in == 0u || n_in == 4u) { return TG_OK; }

    if (n_in == 1u || n_in == 3u) {
        /* One vertex on its own side: a single triangle across the three edges
         * that leave it. */
        u32 lone = (n_in == 1u) ? inside[0] : outside[0];
        u32 o[3];
        u32 v[3];
        u32 m = 0;
        for (k = 0; k < 4u; ++k) { if (k != lone) { o[m++] = k; } }
        for (k = 0; k < 3u; ++k) {
            r = edge_vertex(c, gi[lone], gi[o[k]], &v[k]);
            if (r != TG_OK) { return r; }
        }
        {
            u32 n = c->g + 1u;
            V3 p_lone = grid_point(c, gi[lone] % n, (gi[lone] / n) % n,
                                   gi[lone] / (n * n));
            V3 other = v3_scale(v3_add(v3_add(c->vert[v[0]].p, c->vert[v[1]].p),
                                       c->vert[v[2]].p), 1.0f / 3.0f);
            if (n_in == 1u) {
                /* lone vertex is the inside one */
                return push_tri_oriented(c, v[0], v[1], v[2], p_lone, other);
            }
            return push_tri_oriented(c, v[0], v[1], v[2], other, p_lone);
        }
    }

    /* Two on each side: a quadrilateral across four edges, split into two
     * triangles. The corner order matters -- pairing the wrong diagonals gives a
     * bow-tie whose two triangles overlap, which passes an edge-count test and
     * looks like a pinch in the surface. */
    {
        u32 v00, v01, v10, v11;
        r = edge_vertex(c, gi[inside[0]], gi[outside[0]], &v00);
        if (r != TG_OK) { return r; }
        r = edge_vertex(c, gi[inside[0]], gi[outside[1]], &v01);
        if (r != TG_OK) { return r; }
        r = edge_vertex(c, gi[inside[1]], gi[outside[1]], &v11);
        if (r != TG_OK) { return r; }
        r = edge_vertex(c, gi[inside[1]], gi[outside[0]], &v10);
        if (r != TG_OK) { return r; }
        {
            u32 n = c->g + 1u;
            V3 in_mid, out_mid;
            u32 ii0 = gi[inside[0]], ii1 = gi[inside[1]];
            u32 oo0 = gi[outside[0]], oo1 = gi[outside[1]];
            in_mid = v3_scale(v3_add(grid_point(c, ii0 % n, (ii0 / n) % n,
                                                ii0 / (n * n)),
                                     grid_point(c, ii1 % n, (ii1 / n) % n,
                                                ii1 / (n * n))), 0.5f);
            out_mid = v3_scale(v3_add(grid_point(c, oo0 % n, (oo0 / n) % n,
                                                 oo0 / (n * n)),
                                      grid_point(c, oo1 % n, (oo1 / n) % n,
                                                 oo1 / (n * n))), 0.5f);
            r = push_tri_oriented(c, v00, v01, v11, in_mid, out_mid);
            if (r != TG_OK) { return r; }
            return push_tri_oriented(c, v00, v11, v10, in_mid, out_mid);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Boundary loops                                                            */
/* ------------------------------------------------------------------------- */
typedef struct LoopStore {
    u32 *vert;      /* concatenated loops                                     */
    u32 *first;     /* start offset per loop                                  */
    u32 *count;
    u32  loop_count;
    u32  total;
    u32  cap_vert, cap_loop;
    u64  b_vert, b_first, b_count;
} LoopStore;

/* Chains the kept triangles' unpaired directed edges into closed loops.
 *
 * With consistent winding an unpaired edge (a -> b) has exactly one successor
 * leaving b, so the walk is deterministic and needs no search. If the winding were
 * inconsistent the walk would dead-end, which is reported as an unmatched loop
 * rather than silently truncated. */
/* Chains unpaired directed edges into closed loops by walking the TRIANGLE FAN
 * around each boundary vertex.
 *
 * WHY NOT A SUCCESSOR MAP KEYED BY VERTEX. That was the first implementation and it
 * is wrong in a way that is easy to miss: it assumes each boundary vertex has exactly
 * one outgoing boundary edge, which holds only if the boundary is a union of SIMPLE
 * cycles. Marching tetrahedra can produce a surface that pinches to a point, where
 * two boundary loops touch at one vertex and that vertex has out-degree two. The map
 * silently kept one edge, the walk then traced a tail into a cycle rather than a
 * cycle, the resulting list contained one vertex twice, and its diagonal to the ring
 * was emitted twice by the seam -- one edge used by four triangles, on every union
 * with three or more children. The vertices were not coincident and nothing was
 * welded; the boundary graph simply was not what the map assumed.
 *
 * Walking the fan disambiguates correctly at a pinch, because it asks "which boundary
 * edge comes next AROUND THIS TRIANGLE FAN" rather than "which boundary edge leaves
 * this vertex". Two loops through one vertex have two distinct fans and are traced as
 * two loops. */
static bool edge_lookup(EdgeTable *t, u32 a, u32 b, u32 *out) {
    u32 slot = 0;
    return edge_table_find(t, ((u64)a << 32) | (u64)b, out, &slot);
}

static void edge_insert(EdgeTable *t, u32 a, u32 b, u32 value) {
    u32 slot = 0, existing = 0;
    if (edge_table_find(t, ((u64)a << 32) | (u64)b, &existing, &slot)) { return; }
    if (t->key[slot] != 0u) { return; }
    t->key[slot] = (((u64)a << 32) | (u64)b) + 1u;
    t->value[slot] = value;
}

static TgResult extract_loops(JCtx *c, EdgeTable *dir, EdgeTable *seen,
                              LoopStore *ls) {
    u32 i;

    /* Every directed edge, mapped to the triangle that uses it. */
    for (i = 0; i < c->tri_count; ++i) {
        u32 k;
        if (!c->tri[i].keep) { continue; }
        for (k = 0; k < 3u; ++k) {
            edge_insert(dir, c->tri[i].v[k], c->tri[i].v[(k + 1u) % 3u], i);
        }
    }

    for (i = 0; i < c->tri_count; ++i) {
        u32 k;
        if (!c->tri[i].keep) { continue; }
        for (k = 0; k < 3u; ++k) {
            u32 sa = c->tri[i].v[k];
            u32 sb = c->tri[i].v[(k + 1u) % 3u];
            u32 dummy = 0;
            u32 ca, cb, ctri;
            u32 guard = 0;
            if (edge_lookup(dir, sb, sa, &dummy)) { continue; }   /* interior   */
            if (edge_lookup(seen, sa, sb, &dummy)) { continue; }  /* done       */

            if (ls->loop_count >= ls->cap_loop) { return TG_ERR_LIMIT_EXCEEDED; }
            ls->first[ls->loop_count] = ls->total;
            ls->count[ls->loop_count] = 0u;
            ca = sa; cb = sb; ctri = i;
            for (;;) {
                u32 v, t, fan = 0;
                if (ls->total >= ls->cap_vert) { return TG_ERR_LIMIT_EXCEEDED; }
                ls->vert[ls->total++] = ca;
                ls->count[ls->loop_count]++;
                edge_insert(seen, ca, cb, 1u);

                /* Rotate around cb through the fan until a boundary edge leaves it. */
                v = cb;
                t = ctri;
                for (;;) {
                    u32 j, w = TG_INVALID_ID, nt = 0;
                    for (j = 0; j < 3u; ++j) {
                        if (c->tri[t].v[j] == v) {
                            w = c->tri[t].v[(j + 1u) % 3u];
                            break;
                        }
                    }
                    if (w == TG_INVALID_ID) { return TG_ERR_INVALID_STATE; }
                    if (!edge_lookup(dir, w, v, &nt)) {
                        ca = v; cb = w; ctri = t;
                        break;
                    }
                    t = nt;
                    if (++fan > 4096u) { return TG_ERR_INVALID_STATE; }
                }
                if (ca == sa && cb == sb) { break; }
                if (++guard > 1u << 20) { break; }
            }
            ls->loop_count++;
        }
    }
    return TG_OK;
}

typedef struct SeamRing {
    u32 index[256];   /* mesh vertex indices, in CONNECTIVITY order            */
    f32 angle[256];   /* about the limb axis, unwrapped to [0, tau)            */
    u32 count;
} SeamRing;

static void seam_reverse(SeamRing *r) {
    u32 i;
    for (i = 0; i < r->count / 2u; ++i) {
        u32 ti = r->index[i];
        f32 ta = r->angle[i];
        r->index[i] = r->index[r->count - 1u - i];
        r->angle[i] = r->angle[r->count - 1u - i];
        r->index[r->count - 1u - i] = ti;
        r->angle[r->count - 1u - i] = ta;
    }
}

/* Signed area of the loop projected into the limb's cross-section plane. Its sign is
 * the loop's sense about +dir, which is how a loop that runs clockwise is detected
 * and reversed. */
static f32 seam_signed_area(const SeamRing *r, const V3 *proj_n, const V3 *proj_b,
                            const V3 *pos) {
    f32 area = 0.0f;
    u32 i;
    TG_UNUSED(proj_n);
    TG_UNUSED(proj_b);
    for (i = 0; i < r->count; ++i) {
        u32 j = (i + 1u) % r->count;
        area += pos[i].x * pos[j].z - pos[j].x * pos[i].z;
    }
    return area;
}

/* ------------------------------------------------------------------------- */

static void edge_table_clear(EdgeTable *t) {
    if (t->key != NULL) { memset(t->key, 0, (size_t)t->bytes_key); }
    if (t->value != NULL) { memset(t->value, 0, (size_t)t->bytes_val); }
}

/* Is every extracted boundary loop a SIMPLE cycle, and does it map one-to-one onto
 * the limbs?
 *
 * A loop that passes through the same vertex twice is a pinch: the patch's boundary
 * touches itself. The seam cannot sew it, because it would emit the same
 * patch-to-ring diagonal twice and leave one edge used by four triangles -- which is
 * exactly what happened on unions of three or more children, sporadically, depending
 * on how the clip sphere happened to fall across the grid.
 *
 * A pinch cannot be repaired by duplicating the vertex, which would be the usual
 * remedy: the mesh validator welds by exact position, so the duplicate would be
 * welded straight back and the defect would return. It has to be AVOIDED. */
static bool topology_is_sewable(const JCtx *c, const LoopStore *ls,
                                const JunctionSpec *spec, u32 *loop_limb,
                                u32 *mark, u32 mark_stamp_base) {
    u32 i, k;
    u32 limb_used[MESH_JUNCTION_MAX_LIMBS];

    if (ls->loop_count != spec->limb_count) { return false; }
    memset(limb_used, 0, sizeof limb_used);
    for (i = 0; i < ls->loop_count; ++i) {
        u32 stamp = mark_stamp_base + i + 1u;
        V3 centroid = v3_zero();
        u32 best_limb = 0;
        f32 best = -2.0f;
        if (ls->count[i] < 3u) { return false; }
        for (k = 0; k < ls->count[i]; ++k) {
            u32 v = ls->vert[ls->first[i] + k];
            if (mark[v] == stamp) { return false; }   /* pinch: revisited      */
            mark[v] = stamp;
            centroid = v3_add(centroid, c->vert[v].p);
        }
        centroid = v3_scale(centroid, 1.0f / (f32)ls->count[i]);
        {
            V3 dirv = v3_norm_or(v3_sub(centroid, spec->centre),
                                 v3(0.0f, 1.0f, 0.0f));
            u32 li;
            for (li = 0; li < spec->limb_count; ++li) {
                f32 d = v3_dot(dirv, spec->limb[li].dir);
                if (d > best) { best = d; best_limb = li; }
            }
        }
        if (limb_used[best_limb] != 0u) { return false; }  /* two loops, one limb */
        limb_used[best_limb] = 1u;
        loop_limb[i] = best_limb;
    }
    return true;
}

TgResult mesh_junction_build(Mesh *m, const JunctionSpec *spec,
                             JunctionResult *out) {
    JCtx c;
    JunctionResult res;
    EdgeTable dir;
    EdgeTable succ;
    LoopStore ls;
    JunctionSpec local;
    f32 half, max_r = 0.0f, base_seam;
    u32 g, n, i, x, y, z;
    u32 limb_seen[MESH_JUNCTION_MAX_LIMBS];
    u32 loop_limb[MESH_JUNCTION_MAX_LIMBS * 4u];
    u32 *mark = NULL;
    u64 mark_bytes = 0;
    u32 attempt;
    bool sewable = false;
    /* Nudges applied to the seam width, and therefore to the clip radius, when the
     * extracted boundary turns out to be pinched. Deliberately irrational-looking
     * fractions: a pinch is a coincidence between the clip sphere and the sample
     * grid, so the retries must not be commensurate with the cell size or they
     * reproduce it. */
    static const f32 kNudge[6] = { 0.0f, 0.071f, -0.053f, 0.137f, -0.109f, 0.211f };
    TgResult r = TG_OK;

    TG_CHECK(m != NULL && spec != NULL);
    memset(&res, 0, sizeof res);
    memset(&c, 0, sizeof c);
    memset(&dir, 0, sizeof dir);
    memset(&succ, 0, sizeof succ);
    memset(&ls, 0, sizeof ls);
    memset(limb_seen, 0, sizeof limb_seen);
    if (out != NULL) { *out = res; }

    if (spec->limb_count < 2u || spec->limb_count > MESH_JUNCTION_MAX_LIMBS) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (!(spec->limb_length > 0.0f) || !(spec->grid >= 2u)) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < spec->limb_count; ++i) {
        const JunctionLimb *l = &spec->limb[i];
        if (!v3_finite(l->dir) || !(v3_len(l->dir) > 0.5f)) {
            return TG_ERR_INVALID_ARGUMENT;
        }
        if (!(l->radius_inner > 0.0f) || !(l->radius_outer > 0.0f)) {
            return TG_ERR_INVALID_ARGUMENT;
        }
        if (l->ring == NULL || l->ring_count < 3u) {
            return TG_ERR_INVALID_ARGUMENT;
        }
        max_r = tg_maxf(max_r, tg_maxf(l->radius_inner, l->radius_outer));
    }

    /* PRECONDITIONS. Checked here as well as being exposed to the caller, because a
     * caller that ignores them would otherwise get a crack instead of a refusal. */
    if (spec->limb_length < mesh_junction_min_limb_length(spec)) {
        res.not_separable = true;
        if (out != NULL) { *out = res; }
        return TG_OK;
    }

    /* The solid is clipped to the ball, so the box only has to contain that ball
     * with a margin. Sizing it from limb_length plus the largest radius, as the
     * half-space version had to, wasted a third of the grid resolution on empty
     * space outside the region that produces any surface. */
    half = cut_distance(spec) * 1.12f;
    TG_UNUSED(max_r);
    {
        u32 need = mesh_junction_min_grid(spec);
        g = tg_clamp_u32(spec->grid, MJ_GRID_MIN, MJ_GRID_MAX);
        if (g < need) { g = need; }
        if (need >= MJ_GRID_MAX && spec->grid < need) { res.under_resolved = true; }
    }
    n = g + 1u;
    base_seam = (spec->seam_width > 0.0f) ? spec->seam_width
                                         : spec->limb_length * 0.18f;
    local = *spec;
    c.spec = &local;
    c.mesh = m;
    c.g = g;
    res.grid_used = g;
    TG_UNUSED(half);

    /* Allocations, all sized from the grid so none can be exceeded by data. */
    {
        u64 samples = (u64)n * n * n;
        u64 vcap = (u64)n * n * 12u + 256u;   /* generous for a surface patch  */
        u64 tcap = vcap * 3u;
        if (!tg_ckd_mul_u64(samples, sizeof(f32), &c.val_bytes) ||
            !tg_ckd_mul_u64(vcap, sizeof(LocalVert), &c.vert_bytes) ||
            !tg_ckd_mul_u64(tcap, sizeof(Tri), &c.tri_bytes)) {
            return TG_ERR_OVERFLOW;
        }
        c.val = (f32 *)tg_alloc_zero(c.val_bytes);
        c.vert = (LocalVert *)tg_alloc_zero(c.vert_bytes);
        c.tri = (Tri *)tg_alloc_zero(c.tri_bytes);
        if (c.val == NULL || c.vert == NULL || c.tri == NULL) {
            r = TG_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        c.vert_cap = (u32)vcap;
        c.tri_cap = (u32)tcap;
        if (!tg_ckd_mul_u64(vcap, sizeof(u32), &mark_bytes)) {
            r = TG_ERR_OVERFLOW;
            goto cleanup;
        }
        mark = (u32 *)tg_alloc_zero(mark_bytes);
        if (mark == NULL) { r = TG_ERR_OUT_OF_MEMORY; goto cleanup; }
        r = edge_table_init(&c.edges, (u32)vcap);
        if (r != TG_OK) { goto cleanup; }
        r = edge_table_init(&c.positions, (u32)vcap);
        if (r != TG_OK) { goto cleanup; }
        /* The loop store and the two edge tables are reserved once here and reused by
         * every extraction attempt, so a retry costs a memset rather than a
         * reallocation. */
        {
            u64 cap_v = (u64)c.vert_cap + 8u;
            u64 cap_l = (u64)spec->limb_count * 4u + 8u;
            if (!tg_ckd_mul_u64(cap_v, sizeof(u32), &ls.b_vert) ||
                !tg_ckd_mul_u64(cap_l, sizeof(u32), &ls.b_first)) {
                r = TG_ERR_OVERFLOW;
                goto cleanup;
            }
            ls.b_count = ls.b_first;
            ls.vert = (u32 *)tg_alloc_zero(ls.b_vert);
            ls.first = (u32 *)tg_alloc_zero(ls.b_first);
            ls.count = (u32 *)tg_alloc_zero(ls.b_count);
            if (ls.vert == NULL || ls.first == NULL || ls.count == NULL) {
                r = TG_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            ls.cap_vert = (u32)cap_v;
            ls.cap_loop = (u32)cap_l;
        }
        r = edge_table_init(&dir, c.tri_cap * 4u);
        if (r != TG_OK) { goto cleanup; }
        r = edge_table_init(&succ, c.tri_cap * 4u);
        if (r != TG_OK) { goto cleanup; }
    }

    /* Extract, and retry with a nudged clip radius if the boundary comes out
     * pinched. Each attempt is a complete re-extraction; the arrays are reused. */
    for (attempt = 0; attempt < (u32)TG_COUNTOF(kNudge); ++attempt) {
        local = *spec;
        local.seam_width = base_seam * (1.0f + kNudge[attempt]);
        half = cut_distance(&local) * 1.12f;
        c.cell = 2.0f * half / (f32)g;
        c.origin = v3(spec->centre.x - half, spec->centre.y - half,
                      spec->centre.z - half);
        c.vert_count = 0;
        c.tri_count = 0;
        c.coincident_merged = 0;
        ls.loop_count = 0;
        ls.total = 0;
        edge_table_clear(&c.edges);
        edge_table_clear(&c.positions);
        edge_table_clear(&dir);
        edge_table_clear(&succ);

        for (z = 0; z < n; ++z) {
            for (y = 0; y < n; ++y) {
                for (x = 0; x < n; ++x) {
                    c.val[grid_index(&c, x, y, z)] =
                        mesh_junction_field(&local, grid_point(&c, x, y, z));
                }
            }
        }
        res.field_samples = (u32)((u64)n * n * n);

        for (z = 0; z < g; ++z) {
            for (y = 0; y < g; ++y) {
                for (x = 0; x < g; ++x) {
                    u32 corner[8];
                    u32 t;
                    corner[0] = grid_index(&c, x,      y,      z);
                    corner[1] = grid_index(&c, x + 1u, y,      z);
                    corner[2] = grid_index(&c, x,      y + 1u, z);
                    corner[3] = grid_index(&c, x + 1u, y + 1u, z);
                    corner[4] = grid_index(&c, x,      y,      z + 1u);
                    corner[5] = grid_index(&c, x + 1u, y,      z + 1u);
                    corner[6] = grid_index(&c, x,      y + 1u, z + 1u);
                    corner[7] = grid_index(&c, x + 1u, y + 1u, z + 1u);
                    for (t = 0; t < 6u; ++t) {
                        u32 gi[4];
                        gi[0] = corner[kMjTets[t][0]];
                        gi[1] = corner[kMjTets[t][1]];
                        gi[2] = corner[kMjTets[t][2]];
                        gi[3] = corner[kMjTets[t][3]];
                        r = march_tet(&c, gi);
                        if (r != TG_OK) { goto cleanup; }
                    }
                }
            }
        }
        if (c.tri_count == 0u) { res.degenerate = true; r = TG_OK; goto cleanup; }

        /* Drop the clipping-sphere faces: they are where a limb continues, not where
         * the solid ends. */
        for (i = 0; i < c.tri_count; ++i) {
            V3 mid = v3_scale(v3_add(v3_add(c.vert[c.tri[i].v[0]].p,
                                            c.vert[c.tri[i].v[1]].p),
                                     c.vert[c.tri[i].v[2]].p), 1.0f / 3.0f);
            if (is_clip_face(&local, mid)) { c.tri[i].keep = false; }
        }

        r = extract_loops(&c, &dir, &succ, &ls);
        if (r != TG_OK) { goto cleanup; }

        if (topology_is_sewable(&c, &ls, &local, loop_limb, mark,
                                attempt * (MESH_JUNCTION_MAX_LIMBS * 8u))) {
            sewable = true;
            res.retries = attempt;
            break;
        }
    }
    if (!sewable) {
        res.pinched = true;
        res.loops_found = ls.loop_count;
        if (out != NULL) { *out = res; }
        r = TG_OK;
        goto cleanup;
    }

    res.loops_found = ls.loop_count;
    res.coincident_vertices_merged = c.coincident_merged;

    /* COMMIT THE VERTICES, and only now.
     *
     * Nothing is written to the mesh until the extracted topology has been accepted,
     * because a rejected attempt has to be discardable. Committing as the surface was
     * generated, which is what the first version did, made the retry impossible:
     * there is no way to un-append a vertex from a mesh. */
    for (i = 0; i < c.tri_count; ++i) {
        u32 k;
        if (!c.tri[i].keep) { continue; }
        for (k = 0; k < 3u; ++k) {
            LocalVert *lv2 = &c.vert[c.tri[i].v[k]];
            MeshVertex mv;
            u32 idx;
            if (lv2->mesh_index != TG_INVALID_ID) { continue; }
            memset(&mv, 0, sizeof mv);
            mv.position = lv2->p;
            mv.normal = field_gradient(&local, lv2->p, c.cell * 0.35f);
            {
                /* Tangent along whichever limb this point is nearest, so bark grain
                 * flows through the union instead of stopping at it. */
                f32 best = -2.0f;
                V3 t = v3(0.0f, 1.0f, 0.0f);
                u32 li;
                V3 rel = v3_norm_or(v3_sub(lv2->p, spec->centre),
                                    v3(0.0f, 1.0f, 0.0f));
                for (li = 0; li < spec->limb_count; ++li) {
                    f32 d = v3_dot(rel, spec->limb[li].dir);
                    if (d > best) { best = d; t = spec->limb[li].dir; }
                }
                mv.tangent = t;
            }
            mv.param = v2(0.5f, 0.5f);
            mv.color = spec->colour;
            mv.organ_id = spec->organ_id;
            mv.attrib = mesh_pack_attrib((MeshMaterial)spec->material,
                                         MESH_SECTION_WOOD, 0);
            mv.ao = 0.72f;   /* a union is a genuinely occluded pocket         */
            mv.birth_step = spec->birth_step;
            r = mesh_add_vertex(m, &mv, &idx);
            if (r != TG_OK) { goto cleanup; }
            lv2->mesh_index = idx;
            res.vertices_added++;
        }
    }

    /* Commit the patch. */
    for (i = 0; i < c.tri_count; ++i) {
        if (!c.tri[i].keep) { continue; }
        r = mesh_add_triangle(m, c.vert[c.tri[i].v[0]].mesh_index,
                              c.vert[c.tri[i].v[1]].mesh_index,
                              c.vert[c.tri[i].v[2]].mesh_index,
                              spec->organ_id);
        if (r != TG_OK) { goto cleanup; }
        res.patch_triangles++;
    }

    /* Sew each loop to the limb whose cut plane it lies in. */
    for (i = 0; i < ls.loop_count; ++i) {
        u32 lc = ls.count[i];
        const u32 *lv = &ls.vert[ls.first[i]];
        u32 limb = loop_limb[i];
        V3 centroid = v3_zero();
        u32 k;

        if (lc < 3u) { res.loops_unmatched++; continue; }
        for (k = 0; k < lc; ++k) {
            centroid = v3_add(centroid, c.vert[lv[k]].p);
        }
        centroid = v3_scale(centroid, 1.0f / (f32)lc);
        /* The loop-to-limb mapping was established when the topology was accepted,
         * and it is a bijection by construction there. Re-deriving it here would be a
         * second, independent answer to the same question -- exactly the kind of
         * duplication that lets two parts of a pass disagree. */
        if (limb == TG_INVALID_ID) { res.loops_unmatched++; continue; }

        {
            const JunctionLimb *l = &spec->limb[limb];
            V3 pc = centroid;   /* the patch loop's own centre                */
            V3 rc = v3_zero();
            Frame fr;
            SeamRing A, B;
            V3 flatA[256], flatB[256];
            u32 ia, ib;
            const MeshVertex *mv;

            if (lc > 256u || l->ring_count > 256u) {
                res.loops_unmatched++;
                continue;
            }
            fr = frame_make(pc, l->dir, v3(0.0f, 1.0f, 0.0f));
            mv = mesh_vertices(m);
            /* Each loop's angles are measured about its OWN centre. Using one shared
             * centre biases the angles of whichever loop is off-axis, and a limb
             * leaving the sphere obliquely is off-axis by construction. */
            for (k = 0; k < l->ring_count; ++k) {
                rc = v3_add(rc, mv[l->ring[k]].position);
            }
            rc = v3_scale(rc, 1.0f / (f32)l->ring_count);

            /* CONNECTIVITY ORDER, NOT ANGULAR ORDER.
             *
             * The first implementation sorted both loops by angle before sewing. That
             * looks harmless and is not: the strip then joins vertices that are
             * angular neighbours, and the patch's OWN boundary edges -- which follow
             * the loop's traversal order -- are left unpaired wherever the two orders
             * disagree. Measured, that was 26 boundary edges on a single union. The
             * loop is therefore used exactly as extract_loops walked it, so every
             * boundary edge of the patch is consumed by the strip by construction,
             * and the angles are used only to decide which side advances. */
            A.count = 0;
            for (k = 0; k < lc; ++k) {
                V3 d = v3_sub(c.vert[lv[k]].p, pc);
                A.index[A.count] = c.vert[lv[k]].mesh_index;
                flatA[A.count] = v3(v3_dot(d, fr.n), 0.0f, v3_dot(d, fr.b));
                A.count++;
            }
            B.count = 0;
            for (k = 0; k < l->ring_count; ++k) {
                V3 d = v3_sub(mv[l->ring[k]].position, rc);
                B.index[B.count] = l->ring[k];
                flatB[B.count] = v3(v3_dot(d, fr.n), 0.0f, v3_dot(d, fr.b));
                B.count++;
            }
            if (A.count < 3u || B.count < 3u) { res.loops_unmatched++; continue; }

            /* Both loops must run the same way about +dir before they can be merged.
             * A ring supplied clockwise would otherwise be sewn to a patch edge
             * running anticlockwise, producing a surface that crosses itself. */
            if (seam_signed_area(&A, &fr.n, &fr.b, flatA) < 0.0f) {
                seam_reverse(&A);
                { u32 q; for (q = 0; q < A.count / 2u; ++q) {
                    V3 t = flatA[q]; flatA[q] = flatA[A.count - 1u - q];
                    flatA[A.count - 1u - q] = t; } }
            }
            if (seam_signed_area(&B, &fr.n, &fr.b, flatB) < 0.0f) {
                seam_reverse(&B);
                { u32 q; for (q = 0; q < B.count / 2u; ++q) {
                    V3 t = flatB[q]; flatB[q] = flatB[B.count - 1u - q];
                    flatB[B.count - 1u - q] = t; } }
            }
            for (k = 0; k < A.count; ++k) {
                A.angle[k] = atan2f(flatA[k].z, flatA[k].x);
            }
            for (k = 0; k < B.count; ++k) {
                B.angle[k] = atan2f(flatB[k].z, flatB[k].x);
            }
            /* Rotate B so it starts nearest A's start, then express both as angles
             * measured forward from A's start. Without the rotation the merge would
             * begin with the two loops a large angle apart and drag one long sliver
             * all the way round. */
            {
                f32 ref = A.angle[0];
                u32 best = 0;
                f32 best_d = TG_TAU_F;
                for (k = 0; k < B.count; ++k) {
                    f32 d = tg_absf(tg_wrap_pi(B.angle[k] - ref));
                    if (d < best_d) { best_d = d; best = k; }
                }
                if (best > 0u) {
                    SeamRing rot;
                    rot.count = B.count;
                    for (k = 0; k < B.count; ++k) {
                        u32 src = (best + k) % B.count;
                        rot.index[k] = B.index[src];
                        rot.angle[k] = B.angle[src];
                    }
                    B = rot;
                }
                for (k = 0; k < A.count; ++k) {
                    f32 d = A.angle[k] - ref;
                    while (d < 0.0f) { d += TG_TAU_F; }
                    while (d >= TG_TAU_F) { d -= TG_TAU_F; }
                    A.angle[k] = d;
                }
                for (k = 0; k < B.count; ++k) {
                    f32 d = B.angle[k] - ref;
                    while (d < 0.0f) { d += TG_TAU_F; }
                    while (d >= TG_TAU_F) { d -= TG_TAU_F; }
                    B.angle[k] = d;
                }
                A.angle[0] = 0.0f;
            }

            /* Both rings must consist of distinct mesh vertices. A repeat would make
             * the same patch-to-ring diagonal appear twice, and that shows up only as
             * a non-manifold edge much later, so it is checked where it can still be
             * attributed. */
            {
                u32 p2, q2;
                for (p2 = 0; p2 < A.count; ++p2) {
                    for (q2 = p2 + 1u; q2 < A.count; ++q2) {
                        if (A.index[p2] == A.index[q2]) { res.ring_duplicates++; }
                    }
                }
                for (p2 = 0; p2 < B.count; ++p2) {
                    for (q2 = p2 + 1u; q2 < B.count; ++q2) {
                        if (B.index[p2] == B.index[q2]) { res.ring_duplicates++; }
                    }
                }
            }

            /* Merge. Exactly A.count + B.count triangles are emitted, one per edge
             * of each loop, so every edge of both is consumed once and each diagonal
             * twice. That count IS the watertightness argument. */
            /* Self-check: within one seam every patch-to-ring diagonal must be used
             * exactly twice. Counting them here localises a defect that otherwise
             * only appears as a non-manifold edge in a validation report with no
             * indication of which stage produced it. */
            {
                u32 diag_a[600], diag_b[600], diag_n[600];
                u32 diag_count = 0;
                ia = 0; ib = 0;
                while (ia < A.count || ib < B.count) {
                    u32 av = A.index[ia % A.count];
                    u32 bv = B.index[ib % B.count];
                    u32 q2;
                    bool found = false;
                    for (q2 = 0; q2 < diag_count; ++q2) {
                        if (diag_a[q2] == av && diag_b[q2] == bv) {
                            diag_n[q2]++;
                            found = true;
                            break;
                        }
                    }
                    if (!found && diag_count < 600u) {
                        diag_a[diag_count] = av;
                        diag_b[diag_count] = bv;
                        diag_n[diag_count] = 1u;
                        diag_count++;
                    }
                    if (ia >= A.count) { ib++; }
                    else if (ib >= B.count) { ia++; }
                    else if ((u64)ia * B.count <= (u64)ib * A.count) { ia++; }
                    else { ib++; }
                }
                for (ia = 0; ia < diag_count; ++ia) {
                    if (diag_n[ia] > 1u) { res.diagonal_reuse += diag_n[ia] - 1u; }
                }
            }

            ia = 0; ib = 0;
            while (ia < A.count || ib < B.count) {
                u32 a0 = A.index[ia % A.count];
                u32 a1 = A.index[(ia + 1u) % A.count];
                u32 b0 = B.index[ib % B.count];
                u32 b1 = B.index[(ib + 1u) % B.count];
                /* PROPORTIONAL ADVANCE, not by angle.
                 *
                 * Advancing whichever side's next vertex has the smaller angle looks
                 * like the natural rule and it has a failure mode that is easy to
                 * miss: if all of A's angles happen to precede all of B's, every
                 * A-advance is taken first and `ia` saturates while `ib` is still
                 * zero. `a[ia % A.count]` then wraps back to a[0], the pair
                 * (a[0], b[0]) is visited a second time, and the diagonal between
                 * them is emitted twice -- one edge used by four triangles, on a
                 * union that had no duplicate vertices and no pinch anywhere.
                 *
                 * Comparing progress fractions instead interleaves the two sides in
                 * proportion to their vertex counts, so neither can saturate before
                 * the other is finished, and the correspondence becomes one of
                 * arc-length fraction rather than of angle. That is what a
                 * ring-to-ring stitch between different resolutions should be doing
                 * in any case, and it does not care whether a loop's angles happen to
                 * be monotone -- which, for a boundary traced out of an implicit
                 * surface, they are not obliged to be. Integer arithmetic, so there
                 * is nothing to round. */
                bool advance_a;
                if (ia >= A.count) { advance_a = false; }
                else if (ib >= B.count) { advance_a = true; }
                else { advance_a = ((u64)ia * B.count <= (u64)ib * A.count); }
                {
                    /* WINDING IS DERIVED, NOT TESTED PER TRIANGLE.
                     *
                     * Both loops now run anticlockwise about +dir. Loop A is the
                     * patch edge, nearer the union; loop B is the caller's ring,
                     * further along the limb. With phi-hat the anticlockwise tangent
                     * and z-hat along dir, phi_hat x z_hat = r_hat, so
                     * (a_i, a_i+1, b_j) already faces outward and the mirrored case
                     * must be (b_j+1, b_j, a_i). Adjacent triangles share a diagonal
                     * and must traverse it oppositely, which only a derived rule can
                     * guarantee. */
                    u32 t0, t1, t2;
                    if (advance_a) { t0 = a0; t1 = a1; t2 = b0; ia++; }
                    else           { t0 = b1; t1 = b0; t2 = a0; ib++; }
                    if (t0 == t1 || t1 == t2 || t0 == t2) { continue; }
                    r = mesh_add_triangle(m, t0, t1, t2, l->organ_id);
                    if (r != TG_OK) { goto cleanup; }
                    res.seam_triangles++;
                }
            }
            res.loops_sewn++;
            res.seam_expected += A.count + B.count;
            if (limb < MESH_JUNCTION_MAX_LIMBS) {
                limb_seen[limb] = 1u;
                res.loops_per_limb[limb]++;
            }
        }
    }

    for (i = 0; i < spec->limb_count; ++i) {
        if (limb_seen[i] == 0u) { res.limbs_unmatched++; }
    }
    if (res.loops_unmatched > 0u || res.limbs_unmatched > 0u) {
        TG_LOG_WARNF(MJ_SUB,
                     "union with %u limbs left %u loops unsewn and %u limbs "
                     "unreached: that is a crack, not a tolerance",
                     spec->limb_count, res.loops_unmatched, res.limbs_unmatched);
    }
    res.triangles_added = res.patch_triangles + res.seam_triangles;
    res.coincident_vertices_merged = c.coincident_merged;

cleanup:
    if (mark != NULL) { tg_free(mark, mark_bytes); }
    edge_table_free(&c.edges);
    edge_table_free(&c.positions);
    edge_table_free(&dir);
    edge_table_free(&succ);
    if (c.val != NULL) { tg_free(c.val, c.val_bytes); }
    if (c.vert != NULL) { tg_free(c.vert, c.vert_bytes); }
    if (c.tri != NULL) { tg_free(c.tri, c.tri_bytes); }
    if (ls.vert != NULL) { tg_free(ls.vert, ls.b_vert); }
    if (ls.first != NULL) { tg_free(ls.first, ls.b_first); }
    if (ls.count != NULL) { tg_free(ls.count, ls.b_count); }
    if (out != NULL) { *out = res; }
    return r;
}
