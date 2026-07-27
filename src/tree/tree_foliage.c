#include "tree_foliage.h"
#include "tree_mechanics.h"
#include "../core/log.h"
#include "../core/rng.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Sizes                                                                     */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Insertion angle around the shoot.
 *
 * The same rule the bud placement uses, but indexed from the organ id rather than
 * from a running node counter, because the counter was simulation scaffolding and
 * was deliberately not stored on the organ. The consequence is honest and small:
 * the spiral is continuous within an internode and restarts at each one. At 28
 * leaves per metre of shoot and internodes of 3 to 15 cm that is one to four
 * leaves per internode, so what the rule has to achieve is even distribution
 * AROUND the shoot, which it does; continuing one unbroken helix the length of a
 * branch would require the node index to become part of the permanent record. */
static f32 foliage_angle(const TreeProfile *p, u32 index) {
    switch (p->phyllotaxis) {
    case PHYLLO_OPPOSITE_DECUSSATE:
        return (f32)(index % 2u) * (TG_PI_F * 0.5f)
             + (f32)(index % 4u >= 2u ? 1u : 0u) * TG_PI_F;
    case PHYLLO_WHORLED:
        return (TG_TAU_F / (f32)tg_max_u32(p->whorl_count, 1u)) * (f32)index
             + (f32)index * 0.5f;
    case PHYLLO_SPIRAL_ALTERNATE:
    case TREE_PHYLLO_COUNT:
    default:
        return TG_GOLDEN_ANGLE_F * (f32)index;
    }
}

/* Number of margin lobes or teeth this tessellation can actually resolve.
 *
 * A five-lobed blade sampled at three spanwise stations does not look like an oak
 * leaf; it looks like a splinter. Measured: at three stations the midpoint sample
 * landed in a sinus, and the blade came out with 0.00032 m2 of area against the
 * 0.0050 m2 the model asked for -- a 15x error caused purely by aliasing the
 * margin. The margin frequency is therefore capped by the sample rate, which is
 * the same rule that governs the ring segment count around a branch: detail is
 * dropped, never aliased. */
static f32 margin_cycles(const TreeProfile *p, u32 stations) {
    f32 want;
    f32 resolvable = (f32)(stations - 1u) * 0.5f;
    switch (p->leaf_kind) {
    case LEAF_SIMPLE_LOBED:     want = 5.0f;  break;
    case LEAF_SIMPLE_SERRATE:   want = 16.0f; break;
    case LEAF_PINNATE_COMPOUND: want = 5.0f;  break;
    case LEAF_NEEDLE_SINGLE:
    case LEAF_NEEDLE_FASCICLE:
    case LEAF_SCALE:
    case LEAF_KIND_COUNT:
    default:                    want = 0.0f;  break;
    }
    return tg_minf(want, resolvable);
}

static f32 margin_depth(const TreeProfile *p) {
    switch (p->leaf_kind) {
    case LEAF_SIMPLE_LOBED:     return 0.45f;  /* sinuses cut deep      */
    case LEAF_SIMPLE_SERRATE:   return 0.10f;  /* teeth only            */
    case LEAF_PINNATE_COMPOUND: return 0.82f;  /* nearly to the rachis   */
    case LEAF_NEEDLE_SINGLE:
    case LEAF_NEEDLE_FASCICLE:
    case LEAF_SCALE:
    case LEAF_KIND_COUNT:
    default:                    return 0.0f;
    }
}

/* Half-width of the blade at length fraction v, as a fraction of the widest
 * half-width. Two terms: an asymmetric hump widest below the middle -- a
 * symmetric ellipse reads as a generic leaf, real simple blades are widest in
 * their lower half -- and a margin harmonic. Both are GEOMETRY; none of it is an
 * alpha mask.
 *
 * The floor is not cosmetic. Letting the width reach zero at the base and tip put
 * two coincident vertices there and produced 620 zero-area triangles, which the
 * mesh validator rejected. A tip 2% of the blade's width across is invisible and
 * keeps the shell manifold. */
static f32 blade_half_width(const TreeProfile *p, f32 v, f32 cycles) {
    f32 base = powf(sinf(TG_PI_F * tg_saturatef(v)), 0.5f) * (1.0f - 0.30f * v);
    f32 depth = margin_depth(p);
    f32 lobe = 1.0f;
    if (cycles > 0.0f && depth > 0.0f) {
        /* A NOTCH, not a cosine. A plain cosine spends half its period below the
         * mean, so the blade loses nearly half its area and comes out looking
         * gnawed: measured fill of its bounding rectangle was 0.25 against the
         * 0.6 a real lobed blade shows. Raising the trough to a power concentrates
         * the loss into narrow sinuses between broad lobes, which is both the
         * correct shape and the correct area. */
        f32 notch = 0.5f * (1.0f - cosf(TG_TAU_F * cycles * v));
        f32 sharp = (p->leaf_kind == LEAF_PINNATE_COMPOUND) ? 0.9f : 2.2f;
        lobe = 1.0f - depth * powf(notch, sharp);
    }
    return tg_maxf(base * lobe, 0.02f);
}

u32 tree_foliage_stations(const TreeResolved *r) {
    TG_CHECK(r != NULL);
    return tg_clamp_u32(r->leaf_triangle_budget / 9u, 3u, 32u);
}

f32 tree_foliage_unit_area(const TreeResolved *r) {
    const TreeProfile *p;
    f32 area;

    TG_CHECK(r != NULL && r->profile != NULL);
    p = r->profile;
    if (p->category == TREE_CATEGORY_CONIFER) {
        area = p->needle_length_m * p->needle_width_m;
    } else {
        /* INTEGRATED FROM THE BLADE THAT WILL ACTUALLY BE BUILT.
         *
         * This used to be the bounding rectangle times a stated fill factor of
         * 0.65. The generated blade measured 0.25 of its rectangle, so the
         * mechanics pass was bending the tree under two and a half times the
         * foliage the geometry contained. Integrating the same half-width function
         * the mesh uses, at the same station count, means the two cannot disagree:
         * the shape can be changed freely and the mass follows it. */
        u32 st = tree_foliage_stations(r);
        f32 cycles = margin_cycles(p, st);
        f32 half_w = p->leaf_length_m * p->leaf_width_ratio * 0.5f;
        f32 dv = 1.0f / (f32)(st - 1u);
        f32 sum = 0.0f;
        u32 i;
        for (i = 0; i + 1u < st; ++i) {
            f32 a0 = blade_half_width(p, (f32)i * dv, cycles);
            f32 a1 = blade_half_width(p, (f32)(i + 1u) * dv, cycles);
            sum += (a0 + a1) * dv;   /* trapezoid of FULL width 2*half */
        }
        area = sum * half_w * p->leaf_length_m;
    }
    return area * r->leaf_scale * r->leaf_scale;
}

/* ------------------------------------------------------------------------- */
/* Blade outline                                                             */
/*                                                                           */
/* A blade is described by its half-width as a function of the length fraction  */
/* v in [0,1]. Three terms, each with a reason:                                 */
/*                                                                            */
/*   base    an asymmetric hump, widest below the middle, tapering to a point    */
/*           at the tip and narrowing to the petiole at the base. A symmetric     */
/*           ellipse reads as a generic leaf shape; real simple blades are widest  */
/*           in their lower half.                                                 */
/*   lobes   a cosine harmonic in v. This is what makes an oak an oak: the sinus   */
/*           cuts are deep enough that the silhouette is unmistakable even at      */
/*           one pixel per lobe.                                                   */
/*   teeth   a much higher-frequency, much smaller harmonic, for serrate margins.  */
/*                                                                            */
/* All three are evaluated as GEOMETRY. None of it is an alpha mask.           */
/* ------------------------------------------------------------------------- */
/* One blade                                                                 */
/*                                                                           */
/* Built as a closed shell: an upper surface, a lower surface, and a rim joining */
/* them. The alternative -- a single-sided sheet -- would be cheaper and is what  */
/* most foliage does, but it has no thickness, so it vanishes edge-on, cannot be  */
/* checked by the closed-manifold validator, and lights identically from both     */
/* sides. A real leaf is a solid with a distinguishable upper and lower face.     */
/*                                                                            */
/* Two deformations are applied, both of them reasons a leaf does not read as a  */
/* flat polygon:                                                                 */
/*   cupping  the blade curls about its midrib, so the surface has curvature       */
/*            across its width and catches light as a trough rather than a plane.  */
/*   droop    the blade bends downward along its length under its own weight.      */
/* ------------------------------------------------------------------------- */
typedef struct BladeParams {
    V3  origin;       /* petiole attachment, world space                     */
    V3  along;        /* unit, base -> tip of the blade                      */
    V3  across;       /* unit, midrib -> margin                             */
    f32 length;
    f32 half_width;   /* at the widest station                              */
    f32 thickness;
    f32 cup;          /* radians of curl about the midrib at the margin     */
    f32 droop;        /* radians of downward bend at the tip                */
    u32 stations;     /* spanwise subdivisions                              */
    u32 organ_id;
    u32 colour;
} BladeParams;

static TgResult blade_emit(Mesh *m, const TreeProfile *p, const BladeParams *bp,
                           f32 *out_area, u32 *out_tris) {
    /* Bounded so this stays on the stack. Higher quality spends its budget on
     * more LEAVES, because leaf count is what makes a crown read, not the
     * smoothness of one blade. */
    enum { MAX_ST = 32 };
    u32 up_l[MAX_ST], up_r[MAX_ST], lo_l[MAX_ST], lo_r[MAX_ST];
    u32 st = tg_clamp_u32(bp->stations, 3u, (u32)MAX_ST);
    f32 cycles = margin_cycles(p, st);
    V3 normal_ref;
    u32 i;
    f32 area = 0.0f;
    u32 tris = 0;
    TgResult r;

    normal_ref = v3_norm_or(v3_cross(bp->across, bp->along), v3(0.0f, 1.0f, 0.0f));

    for (i = 0; i < st; ++i) {
        f32 v = (f32)i / (f32)(st - 1u);
        f32 hw = blade_half_width(p, v, cycles) * bp->half_width;
        /* Droop: the midrib is an arc, not a line. */
        f32 bend = bp->droop * v * v;
        V3 dir_v = v3_norm_or(v3_add(v3_scale(bp->along, cosf(bend)),
                                     v3_scale(normal_ref, -sinf(bend))),
                              bp->along);
        V3 spine = v3_add(bp->origin, v3_scale(dir_v, bp->length * v));
        V3 nrm = v3_norm_or(v3_cross(bp->across, dir_v), normal_ref);
        /* Cupping: the margins lift out of the chord plane, so the blade is a
         * shallow trough and catches light as a curved surface. */
        f32 lift = (1.0f - cosf(bp->cup)) * hw;
        V3 offs = v3_scale(bp->across, hw * cosf(bp->cup));
        V3 up = v3_scale(nrm, bp->thickness * 0.5f);
        V3 rise = v3_scale(nrm, lift);
        V3 pl = v3_add(v3_sub(spine, offs), rise);
        V3 pr = v3_add(v3_add(spine, offs), rise);
        MeshVertex vt;

        memset(&vt, 0, sizeof vt);
        vt.tangent = dir_v;
        vt.organ_id = bp->organ_id;
        vt.color = bp->colour;
        vt.ao = 1.0f;

        vt.normal = nrm;
        vt.attrib = mesh_pack_attrib(MESH_MAT_LEAF_UPPER, MESH_SECTION_LEAF, 0);
        vt.position = v3_add(pl, up);
        vt.param = v2(v, 0.0f);
        r = mesh_add_vertex(m, &vt, &up_l[i]); if (r != TG_OK) { return r; }
        vt.position = v3_add(pr, up);
        vt.param = v2(v, 1.0f);
        r = mesh_add_vertex(m, &vt, &up_r[i]); if (r != TG_OK) { return r; }

        vt.normal = v3_neg(nrm);
        vt.attrib = mesh_pack_attrib(MESH_MAT_LEAF_LOWER, MESH_SECTION_LEAF, 0);
        vt.position = v3_sub(pl, up);
        vt.param = v2(v, 0.0f);
        r = mesh_add_vertex(m, &vt, &lo_l[i]); if (r != TG_OK) { return r; }
        vt.position = v3_sub(pr, up);
        vt.param = v2(v, 1.0f);
        r = mesh_add_vertex(m, &vt, &lo_r[i]); if (r != TG_OK) { return r; }

        if (i > 0) {
            f32 prev = blade_half_width(p, (f32)(i - 1u) / (f32)(st - 1u), cycles)
                     * bp->half_width;
            /* Trapezoid of FULL width (2 x half-width), one-sided area. */
            area += (hw + prev) * (bp->length / (f32)(st - 1u));
        }
    }

    /* The cross-section rim is a simple quadrilateral: up_l, up_r, lo_r, lo_l.
     *
     * An earlier version carried a fifth vertex for a proud midrib, which made the
     * rim a pentagon and the end caps a fan; the caps were stitched inconsistently
     * and produced 906 boundary edges, 1057 non-manifold edges and 1208 inverted
     * components. Midrib and vein RELIEF belongs in a leaf-detail pass that
     * displaces this surface, not in its topology. */
    for (i = 0; i + 1u < st; ++i) {
        /* Winding is outward, which had to be corrected: the first version wound
         * every face inward and the validator reported 151 inverted components on
         * a 151-leaf tree -- one per blade, which is how a systematic winding
         * error announces itself rather than a stray triangle. */
        r = mesh_add_quad(m, up_l[i], up_r[i], up_r[i + 1u], up_l[i + 1u],
                          bp->organ_id);
        if (r != TG_OK) { return r; }
        r = mesh_add_quad(m, lo_r[i], lo_l[i], lo_l[i + 1u], lo_r[i + 1u],
                          bp->organ_id);
        if (r != TG_OK) { return r; }
        r = mesh_add_quad(m, up_l[i + 1u], lo_l[i + 1u], lo_l[i], up_l[i],
                          bp->organ_id);
        if (r != TG_OK) { return r; }
        r = mesh_add_quad(m, up_r[i], lo_r[i], lo_r[i + 1u], up_r[i + 1u],
                          bp->organ_id);
        if (r != TG_OK) { return r; }
        tris += 8u;
    }
    r = mesh_add_quad(m, up_l[0], lo_l[0], lo_r[0], up_r[0], bp->organ_id);
    if (r != TG_OK) { return r; }
    r = mesh_add_quad(m, up_r[st - 1u], lo_r[st - 1u], lo_l[st - 1u],
                      up_l[st - 1u], bp->organ_id);
    if (r != TG_OK) { return r; }
    tris += 4u;

    if (out_area != NULL) { *out_area = area; }
    if (out_tris != NULL) { *out_tris = tris; }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* One needle: a tapered triangular prism with a closed base and a point.     */
/*                                                                           */
/* Three sides rather than four is not a saving for its own sake. A spruce      */
/* needle is quadrangular in section and a fir needle flat, but at 1.6 mm across */
/* the section is below one pixel at any distance where the whole tree is in     */
/* frame, and the count is what matters: three sides buys a third more needles   */
/* for the same triangles, and needle COUNT is the binding constraint on how a   */
/* conifer crown reads. This is recorded as a compromise, not defended as a      */
/* model.                                                                       */
/* ------------------------------------------------------------------------- */
static TgResult needle_emit(Mesh *m, V3 base, V3 dir, V3 ref, f32 length,
                            f32 width, u32 organ_id, u32 colour, u32 *out_tris) {
    Frame f;
    u32 ring[3], tip, cen;
    u32 i;
    MeshVertex vt;
    TgResult r;

    f.origin = base;
    f.t = dir;
    f.n = v3_norm_or(v3_sub(ref, v3_scale(dir, v3_dot(ref, dir))),
                     v3_any_perpendicular(dir));
    f.b = v3_cross(f.t, f.n);

    memset(&vt, 0, sizeof vt);
    vt.organ_id = organ_id;
    vt.color = colour;
    vt.ao = 1.0f;
    vt.tangent = dir;
    vt.attrib = mesh_pack_attrib(MESH_MAT_NEEDLE, MESH_SECTION_NEEDLE, 0);

    for (i = 0; i < 3u; ++i) {
        V3 rd = frame_ring_dir(f, (f32)i / 3.0f * TG_TAU_F);
        vt.position = v3_add(base, v3_scale(rd, width * 0.5f));
        vt.normal = rd;
        vt.param = v2(0.0f, (f32)i / 3.0f);
        r = mesh_add_vertex(m, &vt, &ring[i]); if (r != TG_OK) { return r; }
    }
    vt.position = v3_add(base, v3_scale(dir, length));
    vt.normal = dir;
    vt.param = v2(1.0f, 0.5f);
    r = mesh_add_vertex(m, &vt, &tip); if (r != TG_OK) { return r; }
    vt.position = base;
    vt.normal = v3_neg(dir);
    vt.param = v2(0.0f, 0.5f);
    r = mesh_add_vertex(m, &vt, &cen); if (r != TG_OK) { return r; }

    for (i = 0; i < 3u; ++i) {
        u32 j = (i + 1u) % 3u;
        r = mesh_add_triangle(m, ring[i], ring[j], tip, organ_id);
        if (r != TG_OK) { return r; }
        r = mesh_add_triangle(m, ring[j], ring[i], cen, organ_id);
        if (r != TG_OK) { return r; }
    }
    if (out_tris != NULL) { *out_tris = 6u; }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Petiole: a three-sided tapered stalk from the twig surface to the blade.   */
/*                                                                           */
/* Without it the blade starts at the twig's surface with no visible support,  */
/* which is the single clearest tell of procedural foliage: real leaves stand    */
/* off their shoot on a stalk, and the stalk is why a blade's orientation is     */
/* only loosely related to the shoot's.                                          */
/* ------------------------------------------------------------------------- */
static TgResult petiole_emit(Mesh *m, V3 base, V3 dir, V3 ref, f32 length,
                             f32 radius, u32 organ_id, u32 colour,
                             u32 *out_tris) {
    Frame f;
    u32 a[3], b[3];
    u32 i;
    MeshVertex vt;
    TgResult r;

    f.origin = base;
    f.t = dir;
    f.n = v3_norm_or(v3_sub(ref, v3_scale(dir, v3_dot(ref, dir))),
                     v3_any_perpendicular(dir));
    f.b = v3_cross(f.t, f.n);

    memset(&vt, 0, sizeof vt);
    vt.organ_id = organ_id;
    vt.color = colour;
    vt.ao = 1.0f;
    vt.tangent = dir;
    vt.attrib = mesh_pack_attrib(MESH_MAT_PETIOLE, MESH_SECTION_PETIOLE, 0);

    for (i = 0; i < 3u; ++i) {
        V3 rd = frame_ring_dir(f, (f32)i / 3.0f * TG_TAU_F);
        vt.normal = rd;
        vt.position = v3_add(base, v3_scale(rd, radius));
        vt.param = v2(0.0f, (f32)i / 3.0f);
        r = mesh_add_vertex(m, &vt, &a[i]); if (r != TG_OK) { return r; }
        vt.position = v3_add(v3_add(base, v3_scale(dir, length)),
                             v3_scale(rd, radius * 0.7f));
        vt.param = v2(1.0f, (f32)i / 3.0f);
        r = mesh_add_vertex(m, &vt, &b[i]); if (r != TG_OK) { return r; }
    }
    for (i = 0; i < 3u; ++i) {
        u32 j = (i + 1u) % 3u;
        r = mesh_add_quad(m, a[i], a[j], b[j], b[i], organ_id);
        if (r != TG_OK) { return r; }
    }
    /* Both ends are closed. Leaving the stalk as an open tube contributed 906
     * boundary edges on a 151-leaf tree -- six per petiole -- and the closed
     * section check is only worth having if everything in the section is closed. */
    r = mesh_add_triangle(m, a[2], a[1], a[0], organ_id);
    if (r != TG_OK) { return r; }
    r = mesh_add_triangle(m, b[0], b[1], b[2], organ_id);
    if (r != TG_OK) { return r; }
    if (out_tris != NULL) { *out_tris = 8u; }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Colour                                                                    */
/*                                                                           */
/* Per-vertex, not a texture. Varied on three real gradients: shade leaves are  */
/* darker and bluer than sun leaves, leaves emerging late in the season are      */
/* yellower, and there is a small per-leaf variation so a crown is not one flat  */
/* hue. Autumn colour is driven by the season the settings asked for.            */
/* ------------------------------------------------------------------------- */
static u32 leaf_colour(const TreeResolved *r, f32 light, f32 jitter) {
    f32 red, grn, blu;
    f32 autumn = 0.0f;

    switch (r->settings.season) {
    case SEASON_AUTUMN: autumn = 0.85f; break;
    case SEASON_SPRING: autumn = 0.0f;  break;
    case SEASON_SUMMER:
    case SEASON_WINTER:
    case TREE_SEASON_COUNT:
    default:            autumn = 0.0f;  break;
    }

    if (r->profile->category == TREE_CATEGORY_CONIFER) {
        red = 0.13f; grn = 0.26f; blu = 0.15f;
    } else if (r->settings.season == SEASON_SPRING) {
        red = 0.35f; grn = 0.55f; blu = 0.18f;
    } else {
        red = 0.16f; grn = 0.34f; blu = 0.12f;
    }
    /* Sun leaves are lighter and yellower; shade leaves darker and bluer. */
    red += 0.10f * light;
    grn += 0.14f * light;
    blu += 0.02f * light;
    /* Autumn: chlorophyll withdraws and the carotenoids show. */
    red = tg_lerpf(red, 0.62f, autumn);
    grn = tg_lerpf(grn, 0.34f, autumn);
    blu = tg_lerpf(blu, 0.07f, autumn);

    red *= 1.0f + jitter * 0.16f;
    grn *= 1.0f - jitter * 0.10f;
    blu *= 1.0f + jitter * 0.12f;
    return mesh_pack_rgba(red, grn, blu, 1.0f);
}

/* ------------------------------------------------------------------------- */
/* The pass                                                                  */
/* ------------------------------------------------------------------------- */

TgResult tree_foliage_build(Mesh *mesh, const TreeGraph *graph,
                            const TreeResolved *resolved, u16 final_step,
                            FoliageResult *out) {
    FoliageResult res;
    const TreeProfile *p;
    u32 i, n;
    u64 wanted = 0;
    u64 budget_units;
    u32 unit_tris;
    u32 stations;
    f32 keep_fraction;
    u64 v0, t0;
    bool conifer;
    TgResult r = TG_OK;

    TG_CHECK(mesh != NULL && graph != NULL && resolved != NULL);
    memset(&res, 0, sizeof res);
    p = resolved->profile;
    conifer = (p->category == TREE_CATEGORY_CONIFER);

    /* Tessellation per unit, and therefore how many units the triangle budget
     * buys. A blade of `stations` spanwise steps costs 10 per step plus 6 to
     * close it, plus 6 for its petiole. */
    stations = tg_clamp_u32(resolved->leaf_triangle_budget / 9u, 3u, 32u);
    unit_tris = conifer ? 6u : (8u * (stations - 1u) + 4u + 8u);

    n = tree_graph_organ_count(graph);
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(graph, i);
        if (!tree_mechanics_bears_foliage(o, resolved, final_step)) { continue; }
        res.bearing_segments++;
        wanted += (u64)(o->length * p->leaves_per_metre_of_shoot
                        * resolved->foliage_density + 0.5f);
    }
    res.leaves_wanted = wanted;
    res.triangles_per_leaf = unit_tris;
    res.target_leaf_area_m2 = (f32)wanted * tree_foliage_unit_area(resolved);

    budget_units = resolved->foliage_triangle_budget / (u64)unit_tris;
    if (budget_units > (u64)resolved->max_leaves) {
        budget_units = (u64)resolved->max_leaves;
    }
    keep_fraction = (wanted > 0u && budget_units < wanted)
                      ? (f32)((f64)budget_units / (f64)wanted)
                      : 1.0f;
    if (keep_fraction < 1.0f) {
        res.hit_triangle_budget = true;
        TG_LOG_WARNF("tree_foliage",
               "%llu of %llu foliage units placed (%.1f%%): the geometry budget "
               "for this quality level, not the botany, is the limit",
               (unsigned long long)budget_units, (unsigned long long)wanted,
               (double)(keep_fraction * 100.0f));
    }

    v0 = mesh_vertex_count(mesh);
    t0 = mesh_triangle_count(mesh);

    r = mesh_begin_section(mesh, conifer ? MESH_SECTION_NEEDLE
                                        : MESH_SECTION_LEAF);
    if (r != TG_OK) { return r; }

    for (i = 0; i < n && r == TG_OK; ++i) {
        const Organ *o = tree_graph_organ(graph, i);
        u32 count, k;
        Frame hf;
        f32 light;

        if (!tree_mechanics_bears_foliage(o, resolved, final_step)) { continue; }
        count = (u32)(o->length * p->leaves_per_metre_of_shoot
                      * resolved->foliage_density + 0.5f);
        if (count == 0u) { continue; }

        hf.origin = o->base;
        hf.t = o->direction;
        hf.n = o->frame_ref;
        hf.b = v3_cross(hf.t, hf.n);
        light = tg_saturatef(o->light);

        for (k = 0; k < count; ++k) {
            /* Uniform thinning by a hash of the leaf's identity. A running
             * counter would thin the crown in graph order, which is acropetal, so
             * the budget would be spent on the oldest shoots and the outer crown
             * would come out bald. Hashing is order-independent and spatially
             * even. */
            TgRng rng = tg_rng_substream(resolved->settings.seed,
                                         conifer ? TG_RNG_NEEDLE_PLACEMENT : TG_RNG_LEAF_PLACEMENT, o->id, k);
            f32 along, ang, jitter;
            V3 radial, attach, out_dir, ref;

            if (keep_fraction < 1.0f && !tg_rng_chance(&rng, keep_fraction)) {
                continue;
            }
            along = ((f32)k + 0.5f) / (f32)count;
            ang = foliage_angle(p, o->id * 3u + k);
            radial = frame_ring_dir(hf, ang);
            attach = v3_add(v3_add(o->base, v3_scale(o->direction,
                                                     o->length * along)),
                            v3_scale(radial, o->radius_tip));
            jitter = tg_rng_signed(&rng);

            if (conifer) {
                /* Needles stand out from the shoot, swept forward toward the
                 * tip -- the angle a real conifer shoot shows, and the reason a
                 * spruce twig reads as a bottle-brush rather than a star. */
                f32 sweep = 0.62f + 0.20f * jitter;
                out_dir = v3_norm_or(
                    v3_add(v3_scale(radial, sinf(sweep)),
                           v3_scale(o->direction, cosf(sweep))), radial);
                ref = radial;
                {
                    u32 tris = 0;
                    r = needle_emit(mesh, attach, out_dir, ref,
                                    p->needle_length_m * resolved->leaf_scale
                                        * (1.0f + 0.12f * jitter),
                                    p->needle_width_m * resolved->leaf_scale,
                                    o->id, leaf_colour(resolved, light, jitter),
                                    &tris);
                    if (r != TG_OK) { break; }
                    res.leaves_placed++;
                    res.realised_leaf_area_m2 +=
                        p->needle_length_m * p->needle_width_m
                        * resolved->leaf_scale * resolved->leaf_scale;
                }
            } else {
                BladeParams bp;
                f32 petiole_len = p->leaf_length_m * 0.22f
                                * resolved->leaf_scale;
                f32 area = 0.0f;
                u32 tris = 0;
                V3 blade_base;
                /* The petiole leaves the shoot at a wide angle; the blade then
                 * turns toward the light. Both are needed: without the first the
                 * leaf lies on the twig, without the second every leaf in the
                 * crown points the same way as its twig, which reads as fur. */
                f32 insert = 1.05f + 0.25f * jitter;
                out_dir = v3_norm_or(
                    v3_add(v3_scale(radial, sinf(insert)),
                           v3_scale(o->direction, cosf(insert))), radial);
                r = petiole_emit(mesh, attach, out_dir, o->direction,
                                 petiole_len,
                                 p->leaf_length_m * 0.018f * resolved->leaf_scale,
                                 o->id, mesh_pack_rgba(0.32f, 0.34f, 0.16f, 1.0f),
                                 &tris);
                if (r != TG_OK) { break; }
                res.petioles_placed++;

                blade_base = v3_add(attach, v3_scale(out_dir, petiole_len));
                memset(&bp, 0, sizeof bp);
                bp.origin = blade_base;
                /* Phototropic turn: the blade tilts toward the light direction by
                 * an amount that falls off for shaded leaves, which is why the
                 * outer crown presents flat blades and the interior does not. */
                bp.along = v3_norm_or(
                    v3_add(out_dir,
                           v3_scale(resolved->settings.environment.light_direction,
                                    0.45f * light)),
                    out_dir);
                bp.across = v3_norm_or(v3_cross(bp.along, o->direction),
                                       v3_any_perpendicular(bp.along));
                bp.length = p->leaf_length_m * resolved->leaf_scale
                          * (1.0f + 0.14f * jitter);
                bp.half_width = bp.length * p->leaf_width_ratio * 0.5f;
                bp.thickness = p->leaf_thickness_m;
                bp.cup = 0.30f + 0.12f * jitter;
                bp.droop = 0.35f + 0.20f * jitter;
                bp.stations = stations;
                bp.organ_id = o->id;
                bp.colour = leaf_colour(resolved, light, jitter);
                r = blade_emit(mesh, p, &bp, &area, &tris);
                if (r != TG_OK) { break; }
                res.leaves_placed++;
                res.realised_leaf_area_m2 += area;
            }
        }
        if (r == TG_ERR_LIMIT_EXCEEDED) {
            res.hit_vertex_limit = true;
            r = TG_OK;
            break;
        }
    }

    if (r == TG_OK) { r = mesh_end_section(mesh); }
    else { (void)mesh_end_section(mesh); }

    res.vertices = (u32)(mesh_vertex_count(mesh) - v0);
    res.triangles = (u32)(mesh_triangle_count(mesh) - t0);
    if (out != NULL) { *out = res; }
    return r;
}
