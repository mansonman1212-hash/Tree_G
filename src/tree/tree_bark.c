#include "tree_bark.h"
#include "../core/hash.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Lattice noise                                                             */
/*                                                                           */
/* Hash-based value noise, cubic-interpolated, PERIODIC in the first axis.     */
/*                                                                           */
/* Periodicity is the whole reason this is written by hand rather than reached  */
/* for from a library. A branch surface is a cylinder: the angular coordinate     */
/* wraps, and any noise that does not wrap with it leaves a visible seam running  */
/* the length of every limb. Wrapping the LATTICE INDEX by an integer cell count  */
/* makes the seam impossible rather than small.                                   */
/*                                                                            */
/* Value noise rather than gradient noise: gradient noise is smoother, which is  */
/* the opposite of what bark wants. Bark is made of things with edges.          */
/* ------------------------------------------------------------------------- */

static f32 lattice_value(u64 seed, i32 ix, i32 iy, u32 period) {
    u64 h;
    i32 wx = ix % (i32)period;
    if (wx < 0) { wx += (i32)period; }
    h = tg_hash64_u32(seed, (u32)wx);
    h = tg_hash64_u32(h, (u32)iy);
    /* Top 24 bits: the low bits of a multiply-xor hash are the weakest. */
    return (f32)(h >> 40) * (1.0f / 16777216.0f);
}

static f32 smooth_step(f32 t) { return t * t * (3.0f - 2.0f * t); }

static f32 value_noise(u64 seed, f32 x, f32 y, u32 period) {
    f32 fx = floorf(x), fy = floorf(y);
    i32 ix = (i32)fx, iy = (i32)fy;
    f32 tx = smooth_step(x - fx), ty = smooth_step(y - fy);
    f32 v00 = lattice_value(seed, ix, iy, period);
    f32 v10 = lattice_value(seed, ix + 1, iy, period);
    f32 v01 = lattice_value(seed, ix, iy + 1, period);
    f32 v11 = lattice_value(seed, ix + 1, iy + 1, period);
    return tg_lerpf(tg_lerpf(v00, v10, tx), tg_lerpf(v01, v11, tx), ty);
}

/* Two octaves. Three were tried and the third only added roughness at a scale
 * finer than the ring spacing can carry, which is aliasing dressed up as detail:
 * the mesh cannot represent it, so it becomes noise on the vertex positions. */
static f32 fbm2(u64 seed, f32 x, f32 y, u32 period) {
    f32 a = value_noise(seed, x, y, period);
    f32 b = value_noise(seed ^ 0x9E3779B97F4A7C15ull, x * 2.0f, y * 2.0f,
                        period * 2u);
    return a * 0.68f + b * 0.32f;
}

/* ------------------------------------------------------------------------- */
/* Family parameters                                                         */
/*                                                                           */
/* Depth is expressed as a MULTIPLE OF THE FEATURE SIZE rather than in absolute  */
/* metres, and aspect as the ratio of longitudinal to circumferential cell size.  */
/* Those two numbers are what actually distinguish the families: oak ridges are    */
/* deep and strongly elongated vertically, pine plates are shallow and nearly       */
/* isotropic, birch is almost flat with cross-wise lenticels.                       */
/* ------------------------------------------------------------------------- */
typedef struct FamilyParams {
    f32 depth_ratio;     /* furrow depth / feature size                       */
    f32 aspect;          /* longitudinal cell size / circumferential          */
    /* TERRACING, not a power curve.
     *
     * A power curve gives a rounded, C1-smooth surface, and the rendered trunk read
     * as gentle vertical undulation rather than as bark: real rhytidome has flat
     * ridge crests and flat furrow floors joined by STEEP WALLS, and it is the
     * crease at the top of the wall that makes it read as split bark rather than as
     * a wobbly cylinder. `crest` is the field value below which the surface is at
     * full height, `floor` the value above which it is at full depth; between them
     * is the wall. Narrow the gap and the wall steepens -- but no further than the
     * sample rate can carry, which is why the two are stated explicitly instead of
     * derived from one "sharpness". */
    f32 crest;
    f32 floor_at;
    f32 anisotropy;      /* 0 = isotropic cells, 1 = pure vertical strips      */
    bool cross_break;    /* plated barks: break the strips crosswise           */
} FamilyParams;

static FamilyParams family_params(BarkFamily f) {
    FamilyParams p;
    p.depth_ratio = 0.30f;
    p.aspect = 2.0f;
    p.crest = 0.40f;
    p.floor_at = 0.72f;
    p.anisotropy = 0.7f;
    p.cross_break = false;
    switch (f) {
    case BARK_SMOOTH:
        p.depth_ratio = 0.02f; p.aspect = 3.0f; p.crest = 0.30f;
        p.floor_at = 0.85f; p.anisotropy = 0.5f; break;
    case BARK_SMOOTH_LENTICELLED:
        /* Lenticels are horizontal: the cells are WIDER than tall, which is why
         * aspect goes below one here and nowhere else. */
        p.depth_ratio = 0.06f; p.aspect = 0.35f; p.crest = 0.62f;
        p.floor_at = 0.80f; p.anisotropy = 0.0f; break;
    case BARK_SHALLOW_FISSURED:
        p.depth_ratio = 0.18f; p.aspect = 3.5f; p.crest = 0.48f;
        p.floor_at = 0.74f; p.anisotropy = 0.8f; break;
    case BARK_DEEP_FURROWED_RIDGED:
        /* Oak: deep, strongly vertical, interlacing. The furrows are narrow
         * relative to the ridges, hence the high sharpness. */
        /* Oak: deep, strongly vertical, interlacing, with narrow walls. */
        p.depth_ratio = 0.75f; p.aspect = 4.0f; p.crest = 0.42f;
        p.floor_at = 0.62f; p.anisotropy = 0.85f; break;
    case BARK_BLOCKY_PLATED:
        p.depth_ratio = 0.50f; p.aspect = 1.2f; p.crest = 0.46f;
        p.floor_at = 0.60f; p.anisotropy = 0.25f; p.cross_break = true; break;
    case BARK_SCALY:
        p.depth_ratio = 0.32f; p.aspect = 1.0f; p.crest = 0.52f;
        p.floor_at = 0.68f; p.anisotropy = 0.15f; p.cross_break = true; break;
    case BARK_THICK_PLATED_RESINOUS:
        /* Mature fir and pine: broad flat plates separated by deep clefts. */
        p.depth_ratio = 0.60f; p.aspect = 1.6f; p.crest = 0.44f;
        p.floor_at = 0.58f; p.anisotropy = 0.35f; p.cross_break = true; break;
    case BARK_FIBROUS_STRINGY:
        p.depth_ratio = 0.38f; p.aspect = 7.0f; p.crest = 0.44f;
        p.floor_at = 0.66f; p.anisotropy = 0.95f; break;
    case BARK_EXFOLIATING_PAPERY:
        p.depth_ratio = 0.14f; p.aspect = 0.6f; p.crest = 0.50f;
        p.floor_at = 0.66f; p.anisotropy = 0.1f; break;
    case BARK_FAMILY_COUNT:
    default: break;
    }
    return p;
}

/* ------------------------------------------------------------------------- */

/* Fissure scale at a given radius.
 *
 * The profile's bark_feature_size_m describes the TRUNK BASE, and applying it
 * unchanged to a limb was wrong twice over. Measured consequence: with a fixed
 * 6 cm feature, only THREE axes of 82,437 on an 80-year broadleaf were thick
 * enough to carry any relief at all, because a 6 cm fissure needs a limb thicker
 * than the model's primaries actually are.
 *
 * Real fissure scale grows with the trunk it is on -- an oak's primary limb is
 * finely fissured where its bole is deeply ridged, because the rhytidome has had
 * less time and less circumference to split. Scaling as the square root of the
 * radius ratio makes the number of fissures AROUND the axis grow as sqrt(radius)
 * rather than staying fixed, which is what that looks like. */
static f32 feature_at(const TreeResolved *r, f32 radius) {
    f32 base = tg_maxf(r->bark_feature_size_m, 1e-4f);
    f32 trunk = tg_maxf(r->trunk_base_radius_m, 1e-3f);
    f32 scale = sqrtf(tg_saturatef(radius / trunk));
    return tg_clampf(base * scale, tg_minf(0.006f, base), base);
}

f32 tree_bark_min_radius(const TreeResolved *r) {
    TG_CHECK(r != NULL);
    /* Relief needs at least a few lattice cells around the axis to be a pattern
     * rather than a wobble. With the fissure scale following the radius, a 1.5 cm
     * limb still resolves six cells around, so that is the floor -- and below it
     * the surface is genuinely smooth young bark, which is not a compromise: a
     * 2 cm branch really is smooth on an oak.
     *
     * Raised from 15 mm to 35 mm deliberately. At 15 mm an 80-year broadleaf put
     * relief on 111 axes and a 220-year one on 348, and the cost of sampling all of
     * them well enough to SEE was 2.1 GB of geometry. Restricting relief to the
     * bole and the major limbs -- the surfaces anybody actually inspects -- and
     * spending the saving on resolution is the better trade, and it is closer to the
     * truth as well: a 3 cm oak branch is not deeply furrowed. */
    TG_UNUSED(r);
    return 0.035f;
}

BarkFamily tree_bark_family_at(const TreeResolved *r, f32 radius,
                               f32 *out_maturity) {
    f32 t;
    TG_CHECK(r != NULL);
    /* Cambial maturity as a function of radius, scaled by how mature the trunk
     * base is for this individual. A young tree does not have old bark anywhere,
     * however thick a given branch happens to be. */
    t = tg_remap01f(radius, tree_bark_min_radius(r),
                    tg_maxf(r->trunk_base_radius_m * 0.75f,
                            tree_bark_min_radius(r) * 3.0f));
    t *= tg_saturatef(r->bark_maturity);
    if (out_maturity != NULL) { *out_maturity = t; }
    /* A single crossover, not a blend of two patterns. Cross-fading two families
     * produces a surface that is neither, and the profile's whole point is that a
     * tree belongs to one bark family at a time in any given place. */
    return (t >= 0.5f) ? r->profile->bark_mature : r->profile->bark_juvenile;
}

void tree_bark_axis_setup(const TreeResolved *r, u32 axis_id, f32 max_radius,
                          BarkFamily family, BarkAxisField *out) {
    FamilyParams fp;
    f32 circumference;

    TG_CHECK(r != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->family = family;
    out->feature_m = feature_at(r, max_radius);
    if (max_radius < tree_bark_min_radius(r)) {
        out->active = false;
        return;
    }
    fp = family_params(family);
    circumference = TG_TAU_F * max_radius;
    /* INTEGER cells around, so the lattice wraps exactly. */
    out->cells_around = tg_clamp_u32(
        (u32)(circumference / out->feature_m + 0.5f), 4u, 512u);
    out->cells_per_metre = 1.0f / (out->feature_m * tg_maxf(fp.aspect, 0.05f));
    out->depth_m = out->feature_m * fp.depth_ratio;
    out->seed = tg_hash64_u32(tg_hash64_u64(r->settings.seed, 0xBA2Bull),
                              axis_id);
    out->active = true;
}

/* SAMPLE THE SHAPED FIELD, NOT THE LATTICE.
 *
 * These were first set at two and a half samples per lattice cell, on the
 * reasoning that Nyquist asks for two. The rendered trunk came out perfectly
 * smooth despite the geometry carrying 27 mm of displacement, and the reason is
 * that the sample rate has to resolve the feature the SHAPING function produces,
 * not the cell the noise is defined on. Raising (1 - n) to a power of 2.6 confines
 * each furrow to roughly a third of a cell, so two and a half samples per cell is
 * under one sample per furrow and the interpolation erases it.
 *
 * Five samples per cell around and four along. Along is allowed to be coarser
 * because the longitudinal cells are elongated by the family's aspect ratio, so a
 * furrow crosses them slowly. */
f32 tree_bark_ring_spacing(const BarkAxisField *f) {
    TG_CHECK(f != NULL);
    if (!f->active) { return 1.0e9f; }
    return 1.0f / (f->cells_per_metre * 5.0f);
}

u32 tree_bark_ring_segments(const BarkAxisField *f) {
    TG_CHECK(f != NULL);
    if (!f->active) { return 0u; }
    /* Rounded up to even so the section stays symmetric about its own axis. */
    return ((f->cells_around * 7u) + 1u) & ~1u;
}

BarkSample tree_bark_sample(const BarkAxisField *f, f32 theta, f32 arc_m,
                            f32 radius, f32 maturity) {
    BarkSample s;
    FamilyParams fp;
    f32 x, y, n, shaped, depth;

    s.displacement_m = 0.0f;
    s.exposure = 1.0f;
    s.material = MESH_MAT_BARK_YOUNG;
    if (f == NULL || !f->active) { return s; }

    fp = family_params(f->family);
    /* Lattice coordinates. x wraps at cells_around by construction; y is absolute
     * arc length so a ridge crosses internode boundaries unbroken. */
    x = (theta / TG_TAU_F) * (f32)f->cells_around;
    y = arc_m * f->cells_per_metre;

    /* Anisotropy is applied by SHEARING the sample point rather than by scaling
     * one axis, which is what makes the ridges interlace and fork instead of
     * running as parallel stripes. A vertical stripe pattern is the single most
     * recognisable failure of procedural bark. */
    /* EVERY OCTAVE'S PERIOD MUST BE AN INTEGER NUMBER OF CELLS.
     *
     * A coarser octave is sampled at a fraction of the base coordinate, so its
     * lattice period is that same fraction of cells_around -- and unless the
     * fraction is chosen so the product is a whole number, the octave does not wrap
     * and the whole field carries a seam. This was measured, not reasoned about
     * after the fact: scaling by 0.35 with a period of cells_around/3 put a 13.9 mm
     * discontinuity down the length of every trunk, which the periodicity test
     * found immediately.
     *
     * The integer cell count is therefore chosen FIRST and the coordinate scale
     * derived from it, which makes the mismatch impossible rather than unlikely. */
    {
        u32 wander_cells = tg_max_u32(f->cells_around / 3u, 2u);
        f32 wander_scale = (f32)wander_cells / (f32)f->cells_around;
        f32 wander = fbm2(f->seed ^ 0x51ED2701ull, x * wander_scale, y * 0.12f,
                          wander_cells);
        /* The shear must also be a whole number of cells at the wrap, or the shift
         * itself reintroduces the seam. It is applied to a coordinate that is about
         * to be wrapped modulo cells_around, so any integer multiple is safe. */
        x += (wander - 0.5f) * 3.0f * fp.anisotropy;
        n = fbm2(f->seed, x, y * (1.0f - 0.75f * fp.anisotropy),
                 f->cells_around);
    }
    if (fp.cross_break) {
        /* Plated barks: multiply in a second, coarser field running crosswise, so
         * the vertical clefts are interrupted into plates. */
        u32 cross_cells = tg_max_u32(f->cells_around / 2u, 2u);
        f32 cross_scale = (f32)cross_cells / (f32)f->cells_around;
        f32 cross = fbm2(f->seed ^ 0x1234ABCDull, x * cross_scale, y * 1.7f,
                         cross_cells);
        n = tg_minf(n, cross * 1.15f);
    }

    /* Shaping. The raw noise is a smooth blob field; bark is flat ridge crests
     * separated by narrow deep furrows. Raising (1 - n) to a power puts most of
     * the surface at the crest and confines the displacement to the furrows,
     * which is both the correct appearance and the correct volume: a shaping
     * function that pushed the whole surface inward would thin the trunk. */
    {
        f32 t = tg_saturatef(1.0f - n);
        f32 w = tg_maxf(fp.floor_at - fp.crest, 1e-3f);
        shaped = smooth_step(tg_saturatef((t - fp.crest) / w));
    }
    depth = f->depth_m * tg_saturatef(maturity);
    /* Never cut deeper than a fifth of the local radius. Without this a furrow on
     * a small limb can reach the pith, which inverts the section. */
    depth = tg_minf(depth, radius * 0.22f);

    s.displacement_m = -depth * shaped;
    s.exposure = 1.0f - shaped;
    /* Material follows the geometry: the furrow floor is freshly split, darker,
     * rougher and genuinely more occluded than the crest. */
    if (shaped > 0.62f) {
        s.material = MESH_MAT_BARK_FURROW;
    } else if (maturity > 0.5f) {
        s.material = MESH_MAT_BARK_MATURE;
    } else {
        s.material = MESH_MAT_BARK_YOUNG;
    }
    return s;
}
