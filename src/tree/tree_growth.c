#include "tree_growth.h"

#include "../core/log.h"

#include <string.h>

#define TW_SUB "tree_growth"

/* ------------------------------------------------------------------------- */
/* Attraction point cloud                                                    */
/* ------------------------------------------------------------------------- */

/* Points are placed by REJECTION SAMPLING inside the crown envelope, displaced
 * by the resolved crown offset. Rejection rather than an analytic parameterisation
 * because the envelope is a piecewise power profile whose inverse has no closed
 * form, and because rejection keeps the density uniform in VOLUME -- an angular
 * parameterisation would concentrate points near the axis and bias every
 * direction decision toward the trunk.
 *
 * The number of attempts is fixed rather than "until we have N points", so the
 * cloud is a pure function of the seed and cannot vary with float rounding. */
TgResult tree_growth_build_attractors(const TreeResolved *r, AttractorCloud *out) {
    TgRng rng;
    f32 crown_h, crown_r;
    u32 target, attempts, i, written = 0;
    u64 bytes;
    Aabb b = aabb_empty();

    TG_CHECK(out != NULL);
    memset(out, 0, sizeof *out);
    if (r == NULL || r->profile == NULL) { return TG_ERR_INVALID_ARGUMENT; }

    crown_h = r->height_m - r->crown_base_height_m;
    crown_r = r->crown_width_m * 0.5f;
    if (!(crown_h > 0.0f) || !(crown_r > 0.0f)) {
        /* A seedling with no crown yet is legitimate; an empty cloud is a valid
         * result and the growth loop handles it. */
        out->bounds = aabb_empty();
        return TG_OK;
    }

    {
        /* Volume of the bounding cylinder, times the profile density. The
         * envelope occupies roughly half of it, so this over-provisions the
         * attempt count and the accepted count lands near the intended density. */
        f64 cyl = TG_PI * (f64)crown_r * (f64)crown_r * (f64)crown_h;
        f64 want = cyl * (f64)r->profile->attractor_density;
        if (want > 400000.0) { want = 400000.0; }
        target = (u32)want;
        if (target < 64) { target = 64; }
    }
    /* Attempts, NOT a target to reach. Because y is drawn within the live crown
     * and the radius within that height's envelope, essentially every sample is
     * already inside the envelope -- the only rejection is the asymmetric
     * thinning below. An earlier version used 3x attempts on the assumption that
     * the envelope occupies a third of its bounding cylinder, which produced
     * MEASURED densities of 7.1/m^3 against a profile value of 2.5 and pushed the
     * number of attractors inside the influence sphere to ~122, right at the
     * query buffer limit of 128. That would have silently truncated and biased
     * every direction decision toward low-index attractors.
     *
     * The 1.18 factor compensates for the mean thinning acceptance so the
     * realised density matches the profile. */
    attempts = target + target / 6u + 1u;

    if (!tg_ckd_mul_u64((u64)attempts, sizeof(V3), &bytes)) {
        return TG_ERR_OVERFLOW;
    }
    out->points = (V3 *)tg_alloc(bytes);
    if (out->points == NULL) { return TG_ERR_OUT_OF_MEMORY; }
    out->bytes = bytes;

    rng = tg_rng_substream(r->settings.seed, TG_RNG_ATTRACTOR_CLOUD, 0, 0);

    for (i = 0; i < attempts; ++i) {
        f32 y = tg_rng_range(&rng, r->crown_base_height_m, r->height_m);
        f32 env = tree_resolved_envelope_radius(r, y);
        f32 ang, rad;
        V3 p;
        if (!(env > 0.0f)) { continue; }
        ang = tg_rng_f32(&rng) * TG_TAU_F;
        /* sqrt of a uniform gives uniform area density on the disc. */
        rad = sqrtf(tg_rng_f32(&rng)) * env;
        p = v3(cosf(ang) * rad, y, sinf(ang) * rad);

        /* Crown displacement toward light and downwind. Scaled by height within
         * the crown so the displacement grows with distance from the trunk base,
         * which is how a real crown leans out rather than shearing off at the
         * bottom. */
        {
            f32 t = tg_remap01f(y, r->crown_base_height_m, r->height_m);
            f32 shift = r->crown_asymmetry * crown_r * 0.55f * t;
            p = v3_add(p, v3_scale(r->crown_offset_dir, shift));
        }

        /* Asymmetric thinning: the shaded side genuinely holds fewer attractors,
         * so the crown becomes DIRECTIONALLY sparser rather than uniformly noisy.
         * Uniform jitter here would read as noise instead of as light history. */
        {
            V3 radial = v3_norm_or(v3(p.x, 0.0f, p.z), r->crown_offset_dir);
            f32 align = v3_dot(radial, r->crown_offset_dir); /* -1..1 */
            f32 keep = 1.0f - r->crown_asymmetry * 0.65f * (0.5f - 0.5f * align);
            if (!tg_rng_chance(&rng, tg_saturatef(keep))) { continue; }
        }

        out->points[written++] = p;
        b = aabb_add_point(b, p);
    }

    out->count = written;
    out->bounds = b;
    return TG_OK;
}

void tree_growth_free_attractors(AttractorCloud *cloud) {
    if (cloud == NULL) { return; }
    if (cloud->points != NULL) { tg_free(cloud->points, cloud->bytes); }
    memset(cloud, 0, sizeof *cloud);
}

/* ------------------------------------------------------------------------- */
/* Per-shoot growth state                                                    */
/*                                                                           */
/* Held outside the graph: it is scaffolding for the simulation, not part of   */
/* the tree's permanent record. Keeping it separate prevents transient          */
/* bookkeeping from leaking into the fingerprint.                              */
/* ------------------------------------------------------------------------- */
typedef struct ShootState {
    u32  axis;
    bool active;          /* apex still capable of extension                 */
    f32  own_light;       /* light reaching this apex this step               */
    f32  gathered;        /* basipetal sum over this axis and its children    */
    f32  demand;          /* what this apex can actually spend on extension   */
    f32  resource;        /* acropetal allocation this step                   */
    u16  suppressed_steps;
    /* At the crown surface: alive and photosynthesising, but with nowhere left
     * to extend into. This is a PAUSE, not a death.
     *
     * It used to be a permanent stop, which was wrong in a way that only showed
     * once the envelope was scaled to the tree's current size: a lateral that
     * touched the sapling's small crown surface was retired for the remaining
     * seventy years, so the mature crown could only ever be built by shoots born
     * near the end. A real crown-surface shoot becomes a SHORT SHOOT -- it keeps
     * its leaves, holds position, and resumes extending if the crown around it
     * grows outward or a neighbour dies and opens space. */
    bool at_surface;
    bool surface_counted; /* so the diagnostic counts each shoot once         */
    /* Extension earned but not yet worth a ring of geometry.
     *
     * Without this, every living shoot emitted at least one segment per year
     * however little it grew, so the organ count had a hard floor of
     * axes x years -- measured at 480,000 for an eighty-year broadleaf, which no
     * amount of longitudinal coarsening could reduce. A suppressed twig adding
     * five millimetres a year now accumulates until it has a geometric
     * internode's worth, which is what makes the level of detail actually scale. */
    f32  pending_len;
    u32  pending_nodes;
    u32  node_counter;    /* drives phyllotaxis                              */
    u32  first_child_slot;/* linked list of child shoots, for the passes      */
    u32  next_sibling_slot;
} ShootState;

/* ------------------------------------------------------------------------- */
/* Foliage density grid and transmittance                                    */
/*                                                                           */
/* WHY THIS REPLACED NEIGHBOUR COUNTING.                                      */
/*                                                                           */
/* Self-shading was previously estimated by querying every shoot tip within a  */
/* radius and counting those in the light hemisphere. At the densities a real   */
/* skeleton reaches -- around a hundred tips per cubic metre -- that query      */
/* walks several thousand points, and with twenty thousand active shoots over    */
/* eighty steps it is billions of operations. Generation appeared to hang.       */
/*                                                                            */
/* The replacement is both far cheaper and more defensible: rasterise the tips  */
/* into a coarse occupancy grid once per step, then march along the light        */
/* direction accumulating optical depth, Beer-Lambert style. That is O(1) per    */
/* shoot instead of O(neighbours), and it is an actual transmittance estimate    */
/* rather than a proxy: it naturally reproduces the vertical gradient (a low     */
/* interior shoot has the whole crown above it), the directional asymmetry (the  */
/* shaded side has more crown between it and the light) and interior gaps        */
/* (a hole in the canopy lets light through), none of which had to be modelled   */
/* separately.                                                                  */
/*                                                                            */
/* It is an approximation and is not called anything more: a single-ray          */
/* transmittance with a uniform extinction coefficient, no scattering, no        */
/* spectral dependence. */
/* ------------------------------------------------------------------------- */
typedef struct DensityGrid {
    u32 *cells;
    f32 *tau;       /* optical depth from each cell to the sky, swept per step */
    V3   origin;
    f32  cell;
    f32  inv_cell;
    u32  dim[3];
    u64  cell_count;
    u64  bytes;
    u64  tau_bytes;
} DensityGrid;

static void density_destroy(DensityGrid *d) {
    if (d == NULL) { return; }
    if (d->cells != NULL) { tg_free(d->cells, d->bytes); }
    if (d->tau != NULL) { tg_free(d->tau, d->tau_bytes); }
    memset(d, 0, sizeof *d);
}

static TgResult density_create(DensityGrid *d, Aabb bounds, f32 cell) {
    V3 extent;
    u64 total;

    memset(d, 0, sizeof *d);
    if (aabb_is_empty(bounds)) { bounds = aabb_add_point(aabb_empty(), v3_zero()); }
    bounds = aabb_expand(bounds, cell * 2.0f);
    extent = aabb_extent(bounds);
    d->origin = bounds.mn;
    d->cell = tg_maxf(cell, 1e-3f);
    d->inv_cell = 1.0f / d->cell;
    d->dim[0] = tg_clamp_u32((u32)(extent.x * d->inv_cell) + 1u, 1u, 512u);
    d->dim[1] = tg_clamp_u32((u32)(extent.y * d->inv_cell) + 1u, 1u, 512u);
    d->dim[2] = tg_clamp_u32((u32)(extent.z * d->inv_cell) + 1u, 1u, 512u);
    total = (u64)d->dim[0] * d->dim[1] * d->dim[2];
    if (!tg_ckd_mul_u64(total, sizeof(u32), &d->bytes)) { return TG_ERR_OVERFLOW; }
    d->cells = (u32 *)tg_alloc_zero(d->bytes);
    if (d->cells == NULL) { return TG_ERR_OUT_OF_MEMORY; }
    if (!tg_ckd_mul_u64(total, sizeof(f32), &d->tau_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    d->tau = (f32 *)tg_alloc_zero(d->tau_bytes);
    if (d->tau == NULL) { return TG_ERR_OUT_OF_MEMORY; }
    d->cell_count = total;
    return TG_OK;
}

static void density_clear(DensityGrid *d) {
    if (d->cells != NULL) { memset(d->cells, 0, (size_t)d->bytes); }
    if (d->tau != NULL) { memset(d->tau, 0, (size_t)d->tau_bytes); }
}

static void density_add(DensityGrid *d, V3 p) {
    i32 x, y, z;
    if (d->cells == NULL) { return; }
    x = (i32)floorf((p.x - d->origin.x) * d->inv_cell);
    y = (i32)floorf((p.y - d->origin.y) * d->inv_cell);
    z = (i32)floorf((p.z - d->origin.z) * d->inv_cell);
    if (x < 0 || y < 0 || z < 0) { return; }
    if ((u32)x >= d->dim[0] || (u32)y >= d->dim[1] || (u32)z >= d->dim[2]) {
        return;
    }
    d->cells[((u64)z * d->dim[1] + (u64)y) * d->dim[0] + (u64)x]++;
}


/* Extinction per unit shoot-tip density, m^2 per tip. Calibrated by measurement,
 * not chosen: at this value a shoot buried in a fully occupied crown falls below
 * the profile's light_death_threshold within its suppression tolerance, while a
 * shoot on the crown surface stays comfortably above it. */
#ifndef TG_FOLIAGE_EXTINCTION
#define TG_FOLIAGE_EXTINCTION 0.055f
#endif

/* Index of the cell containing `p`, or U32 max if outside. */
static u32 density_index(const DensityGrid *d, V3 p) {
    i32 x, y, z;
    if (d->cells == NULL) { return 0xFFFFFFFFu; }
    x = (i32)floorf((p.x - d->origin.x) * d->inv_cell);
    y = (i32)floorf((p.y - d->origin.y) * d->inv_cell);
    z = (i32)floorf((p.z - d->origin.z) * d->inv_cell);
    if (x < 0 || y < 0 || z < 0) { return 0xFFFFFFFFu; }
    if ((u32)x >= d->dim[0] || (u32)y >= d->dim[1] || (u32)z >= d->dim[2]) {
        return 0xFFFFFFFFu;
    }
    return (u32)(((u64)z * d->dim[1] + (u64)y) * d->dim[0] + (u64)x);
}

/* OPTICAL DEPTH BY PROPAGATION, NOT BY MARCHING.
 *
 * The light direction is fixed for the whole tree, so the optical depth from a
 * point to the sky is a FIELD over the grid and can be swept once per step
 * instead of integrated once per shoot. This is the shadow-propagation idea from
 * Palubicki et al., "Self-organizing tree models for image synthesis" (2009),
 * applied to a Beer-Lambert extinction rather than their discrete penalty.
 *
 * WHY IT MATTERS, MEASURED: marching per shoot cost 49 grid samples for every
 * live apex on every step. On a 220-year individual -- some 200,000 live apices
 * over 220 steps -- that is on the order of two BILLION samples and generation
 * exceeded two minutes. The sweep costs one pass over the grid, which for the
 * same tree is about 26,000 cells, and turns every light query into a single
 * lookup.
 *
 * The sweep visits cells in order of decreasing depth along the light direction,
 * so tau(p) = tau(p + dir*t) + extinction * density(p) * t always reads a cell
 * that is already final. Iterating along the direction's DOMINANT axis guarantees
 * that ordering for any direction. */
static void density_propagate(DensityGrid *d, V3 dir, f32 extinction) {
    u32 ax = 0;
    f32 mag[3];
    f32 step, cell_volume;
    i32 slice, first, last, delta;
    u32 u, v, du, dv;

    if (d->cells == NULL || d->tau == NULL) { return; }
    mag[0] = tg_absf(dir.x); mag[1] = tg_absf(dir.y); mag[2] = tg_absf(dir.z);
    if (mag[1] >= mag[0] && mag[1] >= mag[2])      { ax = 1; }
    else if (mag[2] >= mag[0] && mag[2] >= mag[1]) { ax = 2; }
    if (!(mag[ax] > 1e-4f)) {
        memset(d->tau, 0, (size_t)d->cell_count * sizeof d->tau[0]);
        return;
    }
    /* Ray length that advances exactly one cell along the dominant axis. */
    step = d->cell / mag[ax];
    cell_volume = d->cell * d->cell * d->cell;

    /* Sweep from the lit boundary inward, so the cell one step toward the light
     * is always already resolved. */
    {
        f32 comp = (ax == 0) ? dir.x : (ax == 1 ? dir.y : dir.z);
        if (comp > 0.0f) { first = (i32)d->dim[ax] - 1; last = -1; delta = -1; }
        else             { first = 0; last = (i32)d->dim[ax]; delta = 1; }
    }
    du = d->dim[(ax + 1u) % 3u];
    dv = d->dim[(ax + 2u) % 3u];

    for (slice = first; slice != last; slice += delta) {
        for (v = 0; v < dv; ++v) {
            for (u = 0; u < du; ++u) {
                u32 idx3[3];
                u32 here, ahead;
                V3 centre;
                f32 tau_ahead;
                idx3[ax] = (u32)slice;
                idx3[(ax + 1u) % 3u] = u;
                idx3[(ax + 2u) % 3u] = v;
                here = (idx3[2] * d->dim[1] + idx3[1]) * d->dim[0] + idx3[0];
                centre = v3_add(d->origin,
                               v3(((f32)idx3[0] + 0.5f) * d->cell,
                                  ((f32)idx3[1] + 0.5f) * d->cell,
                                  ((f32)idx3[2] + 0.5f) * d->cell));
                ahead = density_index(d, v3_add(centre, v3_scale(dir, step)));
                tau_ahead = (ahead == 0xFFFFFFFFu) ? 0.0f : d->tau[ahead];
                d->tau[here] = tau_ahead
                             + extinction * ((f32)d->cells[here] / cell_volume)
                               * step;
            }
        }
    }
}

/* Transmittance reaching `pos`. The lookup deliberately samples ONE STEP along
 * the light direction rather than the containing cell, so a shoot is not shaded
 * by its own tip. */
static f32 density_transmittance(const DensityGrid *d, V3 pos, V3 dir) {
    f32 mag = tg_maxf(tg_maxf(tg_absf(dir.x), tg_absf(dir.y)),
                      tg_absf(dir.z));
    u32 idx;
    if (d->tau == NULL || !(mag > 1e-4f)) { return 1.0f; }
    idx = density_index(d, v3_add(pos, v3_scale(dir, d->cell / mag)));
    if (idx == 0xFFFFFFFFu) { return 1.0f; }
    return expf(-d->tau[idx]);
}

typedef struct GrowthCtx {
    TreeGraph          *graph;
    const TreeResolved *r;
    const TreeProfile  *p;
    TgArray             shoots;      /* ShootState, indexed by axis id        */
    SpatialGrid         attractors;
    AttractorCloud      cloud;
    /* Foliage occupancy, rebuilt each step, used for the transmittance estimate. */
    DensityGrid         density;
    GrowthResult       *result;
    u16                 step;
    /* The tree's CURRENT extent, refreshed every step. Light must be judged
     * against how big the tree is NOW, not against the size it will eventually
     * reach: measuring a one-metre seedling's leader against a sixteen-metre
     * target made the leader read as deeply shaded and killed it in the first
     * few steps. */
    f32                 current_height;
    f32                 current_spread;
    /* Time constant of the height curve, in growth steps. Shared by the
     * extension decay so the growth and resolution models cannot drift apart. */
    f32                 age_decay_k;
    /* The shoot system may not spend the entire organ budget: the root system is
     * generated afterwards and must have room. Without this reservation a large
     * conifer exhausted the ceiling above ground and came out with NO ROOTS AT
     * ALL, which the root-architecture test caught. */
    u32                 shoot_organ_budget;
} GrowthCtx;

static ShootState *shoot_of(GrowthCtx *c, u32 axis) {
    TG_ASSERT(axis < c->shoots.count);
    return &((ShootState *)c->shoots.data)[axis];
}

static TgResult shoot_add(GrowthCtx *c, u32 axis) {
    ShootState s;
    memset(&s, 0, sizeof s);
    s.axis = axis;
    s.active = true;
    s.at_surface = false;
    s.surface_counted = false;
    s.first_child_slot = TG_INVALID_ID;
    s.next_sibling_slot = TG_INVALID_ID;
    /* Axis ids are dense and increasing, so the shoot array stays index-aligned
     * with the axis array by construction. */
    TG_ASSERT(axis == (u32)c->shoots.count);
    return tg_array_push(&c->shoots, &s, NULL);
}

/* ------------------------------------------------------------------------- */
/* Light estimation                                                          */
/*                                                                           */
/* APPROXIMATION, STATED PLAINLY: this is not a radiative transfer solve. It    */
/* combines three geometric terms that between them reproduce the phenomena the  */
/* directive requires -- crown-surface shoots out-competing interior ones, the   */
/* illuminated side out-growing the shaded side, and lower branches being        */
/* progressively starved. It does not compute irradiance in physical units and   */
/* nothing in this project claims it does.                                      */
/* ------------------------------------------------------------------------- */
/* `pos` is the point whose exposure is wanted. Callers pass a point AHEAD of the
 * apex rather than the apex itself -- see the call site for why. */
static f32 estimate_light(GrowthCtx *c, V3 pos, u32 order) {
    const TreeResolved *r = c->r;
    f32 transmittance, neighbour, order_penalty, light;

    /* 1. Transmittance toward the light through the tree's own foliage. This one
     *    term subsumes what used to be three separate heuristics -- a vertical
     *    exposure ramp, a directional side bias and a neighbour count -- and does
     *    so with an actual optical-depth integral rather than proxies. */
    transmittance = density_transmittance(&c->density, pos,
                                          r->settings.environment.light_direction);

    /* 2. Neighbour shading from the surrounding stand. Terms above model the tree
     *    shading itself; canopy closure blocks lateral light while overhead light
     *    still reaches the top, so the penalty is strongest low in the crown and
     *    vanishes at the apex. This is the mechanism that lifts a forest tree's
     *    live crown and cleans its bole. */
    {
        f32 closure = tg_saturatef(r->settings.environment.canopy_closure);
        f32 relative_height = tg_remap01f(pos.y, 0.0f,
                                          tg_maxf(c->current_height, 0.2f));
        f32 exposed_above = relative_height * relative_height;
        neighbour = tg_lerpf(1.0f, tg_lerpf(0.12f, 1.0f, exposed_above), closure);
    }

    /* 3. Higher orders sit deeper inside foliage on average. */
    order_penalty = 1.0f - 0.06f * (f32)tg_min_u32(order, 5);

    /* A floor representing diffuse sky light reaching even a shaded shoot: without
     * it a deep interior shoot receives exactly zero and the whole interior dies,
     * which is not what happens in a real crown. */
    light = (0.10f + 0.90f * transmittance) * neighbour * order_penalty;
    /* Shade tolerance lifts the floor rather than removing the gradient. */
    light = tg_lerpf(light, sqrtf(tg_saturatef(light)), c->p->shade_tolerance);
    return tg_saturatef(light);
}

/* ------------------------------------------------------------------------- */
/* Crown envelope, scaled to the tree the shoot is actually growing on        */
/*                                                                           */
/* The resolved envelope describes the MATURE crown. Testing a shoot against it  */
/* directly asks a two-metre sapling to keep its branches inside a crown whose   */
/* base sits at five metres and whose surface is nine metres out. Measured        */
/* consequence: every lateral below the mature crown base was retired on its      */
/* first node from year one, so the tree grew a bare pole for decades and the      */
/* crown could only ever be assembled at the very top -- which is precisely what  */
/* the first rendered captures showed.                                            */
/*                                                                             */
/* The envelope is therefore scaled SELF-SIMILARLY to the height the tree has     */
/* reached, so a young tree carries a young tree's crown. The clean bole of a      */
/* mature tree is then produced by the mechanism that actually produces it in the  */
/* field -- the lower branches grow, are overtopped, and are shed -- rather than   */
/* by forbidding them to exist.                                                   */
/* ------------------------------------------------------------------------- */
static f32 envelope_now(const GrowthCtx *c, const TreeResolved *r, f32 y) {
    f32 dev = tg_clampf(c->current_height / tg_maxf(r->height_m, 0.01f),
                        0.05f, 1.0f);
    f32 rr = tree_resolved_envelope_radius(r, tg_minf(y / dev, r->height_m)) * dev;
    /* A floor of half the influence radius. Without it the envelope tapers to
     * zero at the very apex and pinches off the leading shoots of a small tree,
     * and no crown -- however young -- is narrower than the space a single shoot
     * competes over. */
    return tg_maxf(rr, c->p->influence_radius_m * 0.5f);
}

/* ------------------------------------------------------------------------- */
/* Resource passes                                                           */
/* ------------------------------------------------------------------------- */

/* Basipetal: gather light from the tips toward the base.
 *
 * Axis ids increase with creation and a child axis is always created after its
 * parent, so descending axis id is a valid basipetal order -- the same invariant
 * the organ graph relies on, reused here. */
static void gather_basipetal(GrowthCtx *c) {
    u32 n = (u32)c->shoots.count;
    u32 i;

    for (i = 0; i < n; ++i) {
        ShootState *s = shoot_of(c, i);
        s->gathered = s->own_light;
    }
    for (i = n; i-- > 0;) {
        const Axis *a = tree_graph_axis(c->graph, i);
        ShootState *s = shoot_of(c, i);
        if (a->parent_axis == TG_INVALID_ID) { continue; }
        shoot_of(c, a->parent_axis)->gathered += s->gathered;
    }
}

/* Builds explicit child lists over the shoot array, in O(n).
 *
 * WHY THIS EXISTS: the acropetal pass needs each axis's children. Finding them by
 * scanning forward for axes whose parent matches is O(n^2), which at a few
 * thousand axes and a few hundred steps is billions of operations -- generation
 * would appear to hang. Iterating DESCENDING while prepending leaves each list in
 * ascending id order, so the pass remains deterministic. */
static void build_child_lists(GrowthCtx *c) {
    u32 n = (u32)c->shoots.count;
    u32 i;

    for (i = 0; i < n; ++i) {
        ShootState *s = shoot_of(c, i);
        s->first_child_slot = TG_INVALID_ID;
        s->next_sibling_slot = TG_INVALID_ID;
    }
    for (i = n; i-- > 0;) {
        const Axis *a = tree_graph_axis(c->graph, i);
        ShootState *s;
        ShootState *parent;
        if (a->parent_axis == TG_INVALID_ID) { continue; }
        s = shoot_of(c, i);
        parent = shoot_of(c, a->parent_axis);
        s->next_sibling_slot = parent->first_child_slot;
        parent->first_child_slot = i;
    }
}

/* Acropetal: distribute a fixed budget from the base outward, splitting at every
 * branching point by the Borchert-Honda partition with the profile's
 * apical-control coefficient.
 *
 * v_apex   = v * (lambda * Q_own)     / D
 * v_child  = v * ((1-lambda) * Q_child) / D
 * D        = lambda * Q_own + (1-lambda) * Q_children_total
 *
 * The consequence, and the reason for using this rather than an equal split:
 * siblings receive genuinely unequal resource in proportion to the light they
 * gather, so a shaded lateral is starved while its illuminated sibling extends.
 * That single mechanism produces apical dominance, suppressed interior shoots and
 * unequal crown sectors without any of them being programmed directly. */
static void distribute_acropetal(GrowthCtx *c, f32 total_budget) {
    u32 n = (u32)c->shoots.count;
    u32 i;
    f32 lambda = tg_clampf(c->r->apical_control, 0.05f, 0.95f);

    for (i = 0; i < n; ++i) { shoot_of(c, i)->resource = 0.0f; }
    if (n == 0) { return; }
    shoot_of(c, 0)->resource = total_budget;

    /* Ascending axis id is a valid acropetal order (a child axis is always
     * created after its parent), so one forward pass suffices. Children are
     * reached through the O(n) child lists, not by scanning. */
    for (i = 0; i < n; ++i) {
        ShootState *s = shoot_of(c, i);
        f32 v = s->resource;
        f32 q_children = 0.0f;
        f32 denom;
        u32 j;

        if (!(v > 0.0f)) { continue; }

        for (j = s->first_child_slot; j != TG_INVALID_ID;
             j = shoot_of(c, j)->next_sibling_slot) {
            q_children += shoot_of(c, j)->gathered;
        }

        denom = lambda * s->demand + (1.0f - lambda) * q_children;
        if (!(denom > 1e-9f)) {
            /* Nothing is gathering light anywhere below here. Keep the resource
             * at the apex rather than dividing by zero. */
            s->resource = v;
            continue;
        }
        s->resource = v * (lambda * s->demand) / denom;
        for (j = s->first_child_slot; j != TG_INVALID_ID;
             j = shoot_of(c, j)->next_sibling_slot) {
            shoot_of(c, j)->resource +=
                v * ((1.0f - lambda) * shoot_of(c, j)->gathered) / denom;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Direction selection                                                       */
/* ------------------------------------------------------------------------- */

/* Space colonization contribution: the normalised sum of unit directions toward
 * unclaimed attractors within the influence radius (Runions/Prusinkiewicz). Zero
 * length when no attractor is in range, in which case the caller falls back to
 * tropism only -- a shoot with nowhere to go does not stop growing, it just stops
 * being steered, which is what produces long exploratory limbs through shade. */
static V3 colonization_direction(GrowthCtx *c, V3 pos, u32 *out_found) {
    /* Sized against the profile densities so that a full influence sphere fits:
     * with density chosen to place ~20-40 attractors inside the influence radius,
     * 128 leaves substantial headroom. If it ever did truncate, the query returns
     * the numerically smallest indices, which is deterministic but would bias
     * direction toward low-index attractors -- hence the headroom rather than a
     * tight fit. */
    u32 buf[128];
    u32 count = 0, total = 0;
    V3 sum = v3_zero();
    u32 i;

    (void)spatial_query_radius(&c->attractors, pos, c->p->influence_radius_m,
                               buf, (u32)TG_COUNTOF(buf), &count, &total);
    if (out_found != NULL) { *out_found = total; }
    for (i = 0; i < count; ++i) {
        V3 d = v3_sub(c->cloud.points[buf[i]], pos);
        f32 len = v3_len(d);
        if (len < 1e-5f) { continue; }
        sum = v3_add(sum, v3_scale(d, 1.0f / len));
    }
    return sum;
}

/* `turn_scale` divides the per-year angular budget among the nodes of one annual
 * shoot, so subdividing a flush does not multiply how far a shoot can turn in a
 * year. */
static V3 next_direction(GrowthCtx *c, const Axis *axis, V3 pos, V3 current,
                         u32 order, TgRng *rng, f32 turn_scale) {
    V3 desired;
    V3 colon;
    u32 found = 0;
    /* A reiterated orthotropic leader corrects toward vertical as strongly as a
     * trunk does, regardless of its branch order -- that is what makes it a
     * leader rather than a steeply ascending branch. */
    f32 grav = (axis->kind == AXIS_ORTHOTROPIC)
                 ? tree_resolved_gravitropism(c->r, 0)
                 : tree_resolved_gravitropism(c->r, order);
    f32 set_angle = axis->set_angle;
    V3 tropism_target;

    /* Gravitropic / set-angle target. An orthotropic axis corrects toward
     * vertical; a plagiotropic axis holds its set angle, measured from vertical,
     * in its own azimuthal plane. A root uses the same machinery with a downward
     * set angle, so there is no separate code path to keep in sync. */
    {
        V3 horizontal = v3_norm_or(v3(current.x, 0.0f, current.z),
                                   v3_any_perpendicular(v3(0, 1, 0)));
        f32 cs = cosf(set_angle);
        f32 sn = sinf(set_angle);
        tropism_target = v3_norm_or(v3_add(v3_scale(v3(0, 1, 0), cs),
                                           v3_scale(horizontal, sn)),
                                    current);
    }

    colon = colonization_direction(c, pos, &found);

    if (v3_len_sq(colon) > 1e-8f) {
        colon = v3_norm_or(colon, current);
        /* Weight competition against tropism. Higher orders are steered more by
         * available space and less by their set angle, which is why fine shoots
         * fill gaps while primaries hold their architecture. */
        {
            f32 w_space = tg_lerpf(0.35f, 0.80f, tg_saturatef((f32)order / 4.0f));
            desired = v3_norm_or(v3_add(v3_scale(colon, w_space),
                                        v3_scale(tropism_target, 1.0f - w_space)),
                                 current);
        }
    } else {
        desired = tropism_target;
    }

    /* Phototropism: bias toward the light direction, strength from the profile
     * and the environment's anisotropy. */
    {
        f32 w = c->p->phototropism * c->r->settings.environment.light_anisotropy;
        if (w > 0.0f) {
            desired = v3_norm_or(
                v3_add(desired,
                       v3_scale(c->r->settings.environment.light_direction, w)),
                desired);
        }
    }

    /* Bounded per-step turn. This is what makes curvature accumulate as a
     * segmented arc built from many small corrections -- the shape real shoots
     * have -- instead of a smooth constant-radius sweep. */
    {
        f32 budget = c->p->max_turn_per_step * tg_lerpf(0.5f, 1.0f, grav)
                   * tg_clampf(turn_scale, 0.02f, 1.0f);
        V3 turned = v3_rotate_toward(current, desired, budget);
        /* Small isotropic wander so two shoots in identical circumstances do not
         * grow identically. Deliberately small: individuality comes from history,
         * not from noise. */
        f32 wander = 0.06f * tg_lerpf(1.0f, 2.0f, tg_saturatef((f32)order / 4.0f))
                   * tg_clampf(turn_scale, 0.02f, 1.0f);
        turned = tg_rng_cone(rng, turned, wander);
        return v3_norm_or(turned, current);
    }
}

/* ------------------------------------------------------------------------- */
/* Bud placement and breaking                                                */
/* ------------------------------------------------------------------------- */

static f32 phyllotactic_angle(const TreeProfile *p, u32 node_index, u32 sub) {
    switch (p->phyllotaxis) {
    case PHYLLO_OPPOSITE_DECUSSATE:
        /* Pairs, each pair rotated 90 degrees from the previous. */
        return (f32)(node_index % 2u) * (TG_PI_F * 0.5f)
             + (f32)sub * TG_PI_F;
    case PHYLLO_WHORLED:
        return (TG_TAU_F / (f32)tg_max_u32(p->whorl_count, 1u)) * (f32)sub
             + (f32)node_index * 0.5f;
    case PHYLLO_SPIRAL_ALTERNATE:
    case TREE_PHYLLO_COUNT:
    default:
        /* Golden angle. Successive nodes are maximally spread, which is what
         * produces the characteristic non-repeating helix. */
        return TG_GOLDEN_ANGLE_F * (f32)node_index + (f32)sub * TG_PI_F;
    }
}

static u32 buds_per_node(const TreeProfile *p, u32 order) {
    /* Massart architecture builds pseudo-whorls on the MAIN AXIS ONLY. Its
     * laterals branch roughly in two rows, not in whorls. Applying whorls at
     * every order produces a combinatorial explosion of axes and, worse, is
     * botanically wrong: the whorl is a property of the rhythmically growing
     * orthotropic trunk. */
    if (p->architecture == TREE_ARCH_MASSART) {
        return order == 0 ? tg_max_u32(p->whorl_count, 3u) : 2u;
    }
    switch (p->phyllotaxis) {
    case PHYLLO_OPPOSITE_DECUSSATE: return 2;
    case PHYLLO_WHORLED:            return tg_max_u32(p->whorl_count, 3u);
    case PHYLLO_SPIRAL_ALTERNATE:
    case TREE_PHYLLO_COUNT:
    default:                        return 1;
    }
}

/* Decides whether a bud on `host` becomes a new axis this step.
 *
 * Massart architecture builds pseudo-whorls: a TIER of laterals appears together
 * at one node, which is what gives a conifer its regular branch layers. Rauh
 * breaks buds individually, giving an irregular decurrent crown. Both fall out of
 * the same function because the profile says which architecture applies. */
/* `acrotony` is the node's position within its own annual shoot: 0 at the base of
 * the increment, 1 at its distal end. */
static bool should_break_bud(GrowthCtx *c, u32 order, f32 light,
                             u32 node_index, f32 acrotony, TgRng *rng) {
    f32 chance;

    if (order + 1u > c->r->max_branch_order) { return false; }
    if (c->graph->organs.count + 8u >= c->shoot_organ_budget) { return false; }

    /* Driven by LIGHT AT THE BUD, not by the resource allocation.
     *
     * Two reasons. First, biologically, bud break is gated by local conditions,
     * while the resource allocation is a whole-tree quantity. Second, and this
     * was measured: the resource budget scales with the number of active shoots,
     * so a resource-driven probability saturated at 1.0 and every single bud
     * broke, producing an exponential blow-up that hit the organ ceiling with a
     * tree barely a metre tall. Light is bounded in [0,1] by construction and
     * cannot saturate the probability. */
    if (c->p->architecture == TREE_ARCH_MASSART && order == 0) {
        /* Rhythmic tier on the trunk. Reliable enough that the regular branch
         * layers remain the defining feature of this architecture, but NOT every
         * whorl position succeeds: at 0.90 the crown carried five surviving
         * primaries at every annual node and self-shading killed most of the
         * tree. Real whorls commonly show gaps and unequal members. */
        chance = 0.62f;
    } else {
        /* Only well-lit shoots branch. Below the mortality threshold nothing
         * breaks at all, which is what keeps the shaded interior sparse. */
        f32 lit = tg_smoothstepf(c->p->light_death_threshold,
                                 c->p->light_death_threshold + 0.45f, light);
        /* Per BUD, and there are now several buds per annual shoot rather than
         * one, so the per-bud rate is correspondingly lower than the per-year
         * rate it replaced. */
        chance = 0.22f * lit;
    }
    /* ACROTONY. Laterals break preferentially from the DISTAL nodes of a shoot,
     * which is why a real branch carries its side branches toward its far end and
     * why the proximal part of each annual increment stays comparatively clean.
     * A flat probability across the shoot spreads side branches evenly and reads
     * as a diagram. The square keeps the bias strong without forbidding the
     * occasional proximal break. */
    chance *= 0.10f + 0.90f * acrotony * acrotony;
    /* Deeper orders branch less: the crown gets finer, not self-similar. */
    chance *= tg_lerpf(1.0f, 0.55f, tg_saturatef((f32)order / 4.0f));
    /* Rhythm: no lateral may break on every node of a flush. */
    if (c->p->flushes_per_year > 1u &&
        (node_index % c->p->flushes_per_year) != 0u) {
        chance *= 0.25f;
    }
    return tg_rng_chance(rng, chance);
}

/* ------------------------------------------------------------------------- */
/* Tip occupancy grid                                                        */
/* ------------------------------------------------------------------------- */

static TgResult rebuild_density(GrowthCtx *c) {
    u32 i, n;
    Aabb bounds = aabb_empty();

    c->current_height = 0.0f;
    c->current_spread = 0.0f;
    n = tree_graph_organ_count(c->graph);

    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(c->graph, i);
        V3 tip;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
        tip = organ_tip(o);
        if (tip.y > c->current_height) { c->current_height = tip.y; }
        {
            f32 rad = v3_len(v3(tip.x, 0.0f, tip.z));
            if (rad > c->current_spread) { c->current_spread = rad; }
        }
        bounds = aabb_add_point(bounds, tip);
    }

    /* The grid is sized to the CURRENT tree and rebuilt when the tree has grown
     * past it, rather than reallocated every step. Cell size is a fraction of the
     * influence radius: fine enough to resolve a crown gap, coarse enough that the
     * march is a few dozen samples. */
    {
        f32 cell = tg_maxf(c->p->influence_radius_m * 0.55f, 0.15f);
        bool need_new = (c->density.cells == NULL);
        if (!need_new) {
            V3 mn = c->density.origin;
            V3 mx = v3_add(mn, v3((f32)c->density.dim[0] * c->density.cell,
                                  (f32)c->density.dim[1] * c->density.cell,
                                  (f32)c->density.dim[2] * c->density.cell));
            if (!aabb_is_empty(bounds) &&
                (bounds.mn.x < mn.x || bounds.mn.y < mn.y || bounds.mn.z < mn.z ||
                 bounds.mx.x > mx.x || bounds.mx.y > mx.y || bounds.mx.z > mx.z)) {
                need_new = true;
            }
        }
        if (need_new) {
            Aabb padded = aabb_expand(aabb_is_empty(bounds)
                                          ? aabb_add_point(aabb_empty(), v3_zero())
                                          : bounds,
                                      tg_maxf(c->r->crown_width_m * 0.35f, 1.0f));
            TgResult res;
            density_destroy(&c->density);
            res = density_create(&c->density, padded, cell);
            if (res != TG_OK) { return res; }
        } else {
            density_clear(&c->density);
        }
    }

    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(c->graph, i);
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
        density_add(&c->density, organ_tip(o));
    }
    /* One sweep now serves every light query this step. The extinction
     * coefficient is per unit tip density, calibrated so a shoot deep inside a
     * fully occupied crown falls below the profile's mortality threshold while a
     * shoot on the crown surface stays well above it. */
    density_propagate(&c->density, c->r->settings.environment.light_direction,
                      TG_FOLIAGE_EXTINCTION);
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Main growth loop                                                          */
/* ------------------------------------------------------------------------- */

static void ctx_destroy(GrowthCtx *c) {
    tg_array_free(&c->shoots);
    spatial_destroy(&c->attractors);
    density_destroy(&c->density);
    tree_growth_free_attractors(&c->cloud);
}

static void record_step(GrowthCtx *c, const GrowthStepStats *st) {
    if (c->result == NULL) { return; }
    if (c->result->step_count >= GROWTH_MAX_RECORDED_STEPS) { return; }
    c->result->step[c->result->step_count++] = *st;
}

TgResult tree_growth_run(TreeGraph *graph, const TreeResolved *resolved,
                         GrowthCancel *cancel, GrowthResult *out_result) {
    GrowthCtx c;
    TreeResolved r_local;
    TgResult r = TG_OK;
    u32 trunk_axis;
    u16 step;

    TG_CHECK(graph != NULL);
    if (resolved == NULL || resolved->profile == NULL) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (tree_graph_organ_count(graph) != 0) {
        TG_LOG_ERRORF(TW_SUB, "growth requires an empty graph");
        return TG_ERR_INVALID_STATE;
    }
    if (graph->finalized) { return TG_ERR_INVALID_STATE; }

    r_local = *resolved;
    memset(&c, 0, sizeof c);
    c.graph = graph;
    c.r = resolved;
    c.p = resolved->profile;
    c.result = out_result;
    if (out_result != NULL) { memset(out_result, 0, sizeof *out_result); }

    /* Same k as the resolved height curve: maturity_age / ln(10), converted from
     * years to growth steps. */
    c.shoot_organ_budget = (u32)((f32)graph->max_organs * 0.85f);
    if (c.shoot_organ_budget < 64u) { c.shoot_organ_budget = graph->max_organs; }

    c.age_decay_k = tg_maxf(c.p->maturity_age_years
                            * (f32)tg_max_u32(c.p->flushes_per_year, 1u)
                            / 2.302585093f, 1.0f);

    r = TG_ARRAY_INIT(&c.shoots, ShootState, 256, "growth.shoots");
    if (r != TG_OK) { goto done; }

    r = tree_growth_build_attractors(resolved, &c.cloud);
    if (r != TG_OK) { goto done; }
    r = spatial_build(&c.attractors, c.cloud.points, c.cloud.count,
                      c.p->influence_radius_m, 1u << 21);
    if (r != TG_OK) { goto done; }
    if (out_result != NULL) { out_result->attractors_initial = c.cloud.count; }

    /* --- the initial axis: a seedling shoot, not a finished trunk --------- */
    {
        V3 start_dir;
        /* The tree leans from the very first internode, biased by the resolved
         * lean. A tree that starts perfectly vertical and is bent later looks
         * bent; one that grew leaning looks leaning. */
        start_dir = v3_norm_or(
            v3_add(v3(0, 1, 0),
                   v3_scale(resolved->lean_direction,
                            tanf(resolved->lean_angle_rad))),
            v3(0, 1, 0));
        r = tree_graph_add_axis(graph, TG_INVALID_ID, TG_INVALID_ID,
                                AXIS_ORTHOTROPIC, 0, 0, 0.0f, v3_zero(),
                                start_dir, &trunk_axis);
        if (r != TG_OK) { goto done; }
        r = shoot_add(&c, trunk_axis);
        if (r != TG_OK) { goto done; }
    }

    /* --- growth steps ----------------------------------------------------- */
    for (step = 0; step < (u16)resolved->growth_steps; ++step) {
        GrowthStepStats st;
        u32 n, i;
        f32 budget;

        if (cancel != NULL && cancel->requested != 0) {
            r = TG_ERR_CANCELLED;
            goto done;
        }

        memset(&st, 0, sizeof st);
        st.step = step;
        c.step = step;

        r = rebuild_density(&c);
        if (r != TG_OK) { goto done; }

        /* 0. CROWN RECESSION, swept over the whole graph.
         *
         * The live crown base rises as the tree grows and the branches left below
         * it are shed. This has to be a sweep rather than a test inside the
         * extension loop, and that was learned by measurement: tested only on
         * ACTIVE shoots it fired 1,041 times on a 118,000-axis tree, because a low
         * branch normally stopped extending years before the crown base reached it
         * and was therefore never looked at again. The rendered tree still carried
         * its crown to within 2 m of the ground against a resolved crown base of
         * 5.26 m.
         *
         * Cost is one pass over the organs per step, the same order as the density
         * rebuild that precedes it, and it only ever finds NEW victims because
         * death is permanent.
         *
         * The trunk is exempt by definition -- it is what the bole is made of --
         * and so are roots. Wood is retained as dead branch rather than deleted,
         * because a shed limb leaves a stub and eventually a branch scar. */
        {
            f32 dev = tg_clampf(c.current_height
                                    / tg_maxf(r_local.height_m, 0.01f),
                                0.05f, 1.0f);
            f32 recess_h = r_local.crown_base_height_m * dev;
            u32 oi, on = tree_graph_organ_count(graph);
            for (oi = 0; oi < on; ++oi) {
                const Organ *o = tree_graph_organ(graph, oi);
                if (o->branch_order == 0u) { continue; }
                if (o->type != ORGAN_BRANCH_SEGMENT
                        && o->type != ORGAN_TWIG_SEGMENT) { continue; }
                if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
                if (o->base.y >= recess_h) { continue; }
                st.shoots_killed += tree_graph_kill_subtree(graph, oi, step,
                                                        ORGAN_FLAG_BARK_RETAINED);
                if (out_result != NULL) { out_result->stopped_by_recession++; }
            }
        }

        /* 1. light for every active apex */
        n = (u32)c.shoots.count;
        for (i = 0; i < n; ++i) {
            ShootState *s = shoot_of(&c, i);
            const Axis *a = tree_graph_axis(graph, i);
            if (!s->active) { s->own_light = 0.0f; continue; }
            /* Re-tested every step against the CURRENT envelope: a shoot held at
             * the crown surface last year is released as soon as the crown grows
             * out past it. */
            s->at_surface = false;
            if (a->order > 0) {
                f32 env = envelope_now(&c, &r_local, a->tip_position.y);
                f32 radial = v3_len(v3(a->tip_position.x, 0.0f,
                                       a->tip_position.z));
                if (radial > env * 1.05f) {
                    s->at_surface = true;
                    if (!s->surface_counted) {
                        s->surface_counted = true;
                        if (out_result != NULL) {
                            out_result->stopped_by_envelope++;
                        }
                    }
                }
            }
            /* Sample the space the shoot is GROWING INTO, not the point it
             * currently occupies.
             *
             * This matters more than it looks. A newly broken bud starts on the
             * parent axis, deep inside the crown, which is the most occluded
             * place in the whole tree. Sampling at the apex therefore condemned
             * every new lateral immediately: mortality reached 60% of all organs
             * and, tellingly, showed NO height bias at all -- proof it was not
             * actually shade driven. Sampling ahead asks the biologically
             * meaningful question: is there light where this shoot is headed. */
            {
                V3 ahead = v3_add(a->tip_position,
                                  v3_scale(a->tip_direction,
                                           c.p->influence_radius_m * 0.6f));
                s->own_light = estimate_light(&c, ahead, a->order);
            }
            /* A shoot pinned at the crown surface makes NO DEMAND on the
             * resource stream: it cannot spend an allocation on extension, so its
             * share passes through the Borchert-Honda partition to its laterals
             * instead of being allocated and discarded. Its leaves still feed the
             * tree, which is why `gathered` uses own_light regardless. */
            s->demand = s->at_surface ? 0.0f : s->own_light;
            st.total_light += s->own_light;
            /* Only extending shoots scale the extension budget. Counting pinned
             * shoots here inflated the budget without adding anywhere to spend
             * it. */
            if (!s->at_surface) { st.active_shoots++; }
        }

        /* 2. resource passes */
        build_child_lists(&c);
        gather_basipetal(&c);
        /* The budget scales WITH the number of active shoots so that the average
         * resource per shoot is scale free: a large crown supports proportionally
         * more extension, and the Borchert-Honda partition then decides who
         * actually gets it. A fixed budget would make every shoot starve as the
         * tree grew, silently halting growth on large trees. */
        budget = 1.0f + 0.75f * (f32)st.active_shoots;
        distribute_acropetal(&c, budget);

        /* 3. extension and bud break. The loop bound is captured BEFORE the
         *    loop so axes created this step are not grown in the same step --
         *    a new shoot must wait for the next flush, as in a real tree. */
        n = (u32)c.shoots.count;
        for (i = 0; i < n; ++i) {
            ShootState *s = shoot_of(&c, i);
            const Axis *axis_snapshot;
            u32 order;
            f32 res;
            V3 pos, dir;
            u32 new_organ;
            TgRng rng_dir, rng_bud;

            if (!s->active) { continue; }
            axis_snapshot = tree_graph_axis(graph, i);
            /* The recession sweep above may have killed this axis's wood. A shoot
             * cannot extend from a dead branch, and without this check it would go
             * on adding live segments to a shed limb. */
            if (axis_snapshot->first_organ != TG_INVALID_ID
                    && (tree_graph_organ(graph, axis_snapshot->first_organ)->flags
                        & ORGAN_FLAG_DEAD) != 0) {
                s->active = false;
                continue;
            }
            order = axis_snapshot->order;
            res = s->resource;
            pos = axis_snapshot->tip_position;
            dir = axis_snapshot->tip_direction;

            /* --- mortality: shade driven, with a tolerance ----------------
             *
             * A shoot gets an establishment grace period. A real shoot emerges
             * from a bud with stored reserves and is not judged on its first
             * day; without this, a lateral born inside the crown is killed
             * before it can reach the light it was heading for. */
            if (axis_snapshot->organ_count < 2u) {
                s->suppressed_steps = 0;
            } else if (s->own_light < c.p->light_death_threshold) {
                s->suppressed_steps++;
                if (s->suppressed_steps > c.p->suppression_tolerance_steps) {
                    if (axis_snapshot->parent_organ != TG_INVALID_ID &&
                        axis_snapshot->first_organ != TG_INVALID_ID) {
                        st.shoots_killed +=
                            tree_graph_kill_subtree(graph,
                                                    axis_snapshot->first_organ,
                                                    step,
                                                    ORGAN_FLAG_BARK_RETAINED);
                    }
                    s->active = false;
                    if (out_result != NULL) { out_result->stopped_by_shade++; }
                    continue;
                }
                /* Suppressed but alive: extends very little. */
                res *= 0.25f;
            } else if (s->suppressed_steps > 0) {
                s->suppressed_steps--;
            }

            /* Pinned at the crown surface: alive, still shading its neighbours,
             * still subject to the mortality test above (which is why this check
             * sits AFTER it -- a low branch left behind by an expanding crown must
             * still be shed once the crown closes over it), but with nowhere to
             * put an internode this year. */
            if (s->at_surface) { continue; }

            /* A shoot with essentially no resource simply does not extend this
             * step. It is not dead: it may recover if a neighbour dies. */
            if (res < 0.010f) {
                if (out_result != NULL) { out_result->starved_steps++; }
                continue;
            }

            if (tree_graph_organ_count(graph) + 4u >= c.shoot_organ_budget) {
                if (out_result != NULL) { out_result->hit_organ_limit = true; }
                s->active = false;
                continue;
            }

            /* CROWN ENVELOPE LIMIT.
             *
             * A shoot stops extending once it has grown past the crown surface,
             * because beyond it there is no unclaimed light. This single rule
             * does most of the work of producing the two categories' silhouettes:
             * with a conical envelope the lower conifer branches run out far and
             * the upper ones stop early, which IS the cone; with a domed envelope
             * the broadleaf fills a rounded crown. Without it, laterals ran on
             * horizontally until the crown was twice as wide as tall and the
             * conifer came out as a bush.
             *
             * Reaching the surface is a PAUSE (`at_surface`, tested above), not a
             * death. Reaching the target HEIGHT is different: the height curve is
             * the tree's realised size, and a leader that passes it has finished.
             * The trunk is exempt from the radial limit -- it lives below the live
             * crown by definition. */
            if (pos.y >= r_local.height_m) {
                s->active = false;
                if (out_result != NULL) { out_result->stopped_by_height++; }
                continue;
            }

            rng_dir = tg_rng_substream(resolved->settings.seed,
                                       TG_RNG_SHOOT_DIRECTION, i, step);
            rng_bud = tg_rng_substream(resolved->settings.seed,
                                       TG_RNG_BUD_FATE, i, step);

            /* ONE FLUSH PRODUCES AN ANNUAL SHOOT OF SEVERAL NODES.
             *
             * The annual extension comes from the profile, which is what keeps
             * the growth model consistent with the height curve; the flush divides
             * it into that order's node count. Each node gets its own bounded
             * direction correction, so one year's growth is a short segmented arc
             * rather than a single straight stick -- and, far more consequentially,
             * every node is a branching opportunity.
             *
             * Treating one internode as one year gave each shoot exactly one
             * chance to branch per year. Measured result: 423 of 825 axes born in
             * the final tenth of the tree's life with a mean length of 0.23 m,
             * which rendered as a bare pole with a tuft on top. */
            {
                u32 nodes_full = tg_clamp_u32(
                    c.p->nodes_per_flush[tg_min_u32(order, 9u)], 1u, 40u);
                f32 annual = tree_resolved_internode_length(resolved, order);
                f32 own_age = (f32)step - (f32)axis_snapshot->created_step;
                f32 k_axis = (order == 0) ? c.age_decay_k
                                          : c.age_decay_k * 0.55f;
                f32 decay = expf(-tg_maxf(own_age, 0.0f) / k_axis);
                f32 vigour = tg_clampf(0.45f + res * 0.55f, 0.30f, 1.5f);
                /* LONG SHOOTS AND SHORT SHOOTS.
                 *
                 * The node COUNT follows vigour; the internode LENGTH does not.
                 * This is the auxoblast/brachyblast distinction and it is not a
                 * refinement -- it was the single largest defect in the whole
                 * skeleton.
                 *
                 * Holding the count at the profile maximum while the annual
                 * increment decayed as exp(-age/k) forced the internode length to
                 * collapse instead. Measured, on an eighty-year broadleaf: order-4
                 * axes averaged 0.351 m of length spread over 41.2 internodes --
                 * 8.5 MILLIMETRES each -- and order-2 axes 1.93 m over 109.9. That
                 * is not a tree, it is a tree finely diced: 1.24 million segments
                 * of which perhaps a quarter carried any shape information, the
                 * organ ceiling exhausted at step 63 of 80, and a tree that
                 * reached 10.7 m of its 18.8 m target and rendered as a bare pole
                 * under a flat pancake of twigs.
                 *
                 * Deriving the count from the increment keeps the internode at the
                 * length the profile actually specifies (annual / nodes_full, i.e.
                 * 1.5 to 4 cm depending on order) and lets a weakening shoot do
                 * what a weakening shoot does: produce fewer, not shorter,
                 * internodes, until it is making one short internode a year. Total
                 * axis length is unchanged -- the same annual increment is divided
                 * by a smaller count -- so the height curve still integrates
                 * correctly. */
                /* THE LEADER IS DRIVEN BY THE RESOLVED HEIGHT CURVE.
                 *
                 * Laterals have no externally specified target, so their extension
                 * is the profile increment modulated by vigour. The trunk does have
                 * one -- the resolved individual's height -- and letting it emerge
                 * from the same product of profile constant, age decay and a
                 * clamped vigour meant the two agreed only by luck. Measured, they
                 * did not: the leader integrated to 16.69 m against a resolved
                 * target of 18.79 m, and it got that close only because vigour sat
                 * pinned at its 1.5 ceiling for most of the tree's life, so the
                 * shortfall was silently sensitive to a clamp.
                 *
                 * Instead the leader tracks the height curve directly: its arc
                 * length after t steps is the resolved height (corrected for lean)
                 * times the same normalised 1-exp(-t/k) the resolution pass used.
                 * The annual increment is then the DIFFERENCE between this step's
                 * target and the length already built, which self-corrects -- a
                 * year lost to suppression is made up later, exactly as a released
                 * tree does, and the final height matches the specification by
                 * construction. Realised height still differs slightly from arc
                 * length because the leader curves and then sags, and that
                 * difference is reported rather than compensated. */
                f32 increment;
                f32 target_internode = annual / (f32)nodes_full;

                if (order == 0u) {
                    f32 k = c.age_decay_k;
                    f32 total = (f32)tg_max_u32(r_local.growth_steps, 1u);
                    f32 span = 1.0f - expf(-total / k);
                    f32 lean_cos = tg_maxf(cosf(r_local.lean_angle_rad), 0.5f);
                    f32 final_len = r_local.height_m / lean_cos;
                    f32 want = (span > 1e-6f)
                                 ? final_len * (1.0f - expf(-((f32)step + 1.0f) / k))
                                       / span
                                 : final_len;
                    increment = tg_maxf(want - axis_snapshot->total_length, 0.0f);
                    /* A single year cannot make up an unlimited backlog. */
                    increment = tg_minf(increment, annual * 2.5f);
                } else {
                    increment = annual * decay * vigour;
                }
                u32 nodes = tg_clamp_u32(
                    (u32)(increment / tg_maxf(target_internode, 1e-6f) + 0.5f),
                    1u, nodes_full);
                f32 internode;
                /* Minimum length of an EMITTED internode. Botanical nodes shorter
                 * than this are grouped into one segment; their buds still sit at
                 * their true fractional positions along it. See
                 * TreeResolved::internode_geometry_scale for why the grouping is
                 * done here rather than by moving the nodes themselves. */
                f32 geom_min = target_internode
                             * tg_maxf(r_local.internode_geometry_scale, 1.0f)
                             * 0.999f;
                u32 node = 0;
                f32 node_len[40];
                bool stopped = false;

                /* Carry this year's extension forward until it is worth a ring.
                 * The trunk is exempt: it tracks the height curve step by step and
                 * its increments always clear the threshold anyway, and holding it
                 * back would put a visible stair in the construction replay. */
                s->pending_len += increment;
                s->pending_nodes += nodes;
                /* An axis ALWAYS emits its first internode. Deferring it left
                 * axes carrying zero organs -- first_organ == TG_INVALID_ID -- and
                 * the graph walk dereferenced them and crashed. A shoot that has
                 * broken from its bud is a real, physically present twig, however
                 * short; the accumulation applies to what it does afterwards. */
                if (order > 0u && axis_snapshot->organ_count > 0u
                        && s->pending_len < geom_min) {
                    continue;
                }
                nodes = tg_clamp_u32(s->pending_nodes, 1u,
                                     (u32)TG_COUNTOF(node_len));
                internode = s->pending_len / (f32)nodes;
                s->pending_len = 0.0f;
                s->pending_nodes = 0u;

                while (node < nodes && !stopped) {
                    u32 group_start = node;
                    u32 group_count = 0;
                    f32 group_len = 0.0f;
                    f32 acrotony;
                    f32 acc;
                    u32 gi;
                    u32 per_node;
                    u32 sub;

                    /* Accumulate botanical nodes until the group is long enough to
                     * be worth its own ring of geometry, or the flush runs out. */
                    while (node < nodes
                           && group_count < (u32)TG_COUNTOF(node_len)) {
                        TgRng rng_len = tg_rng_substream(
                            resolved->settings.seed, TG_RNG_INTERNODE_LENGTH, i,
                            (u32)step * 64u + node);
                        f32 vary = 1.0f + tg_rng_signed(&rng_len)
                                          * c.p->internode_length_spread;
                        f32 nlen = tg_maxf(internode * vary, internode * 0.25f);
                        node_len[group_count++] = nlen;
                        group_len += nlen;
                        node++;
                        if (group_len >= geom_min) { break; }
                    }
                    if (group_count == 0u) { break; }
                    acrotony = (f32)group_start / (f32)nodes;

                    /* Stop conditions are re-checked at EVERY emitted internode,
                     * not once per flush: a shoot reaching the crown surface
                     * mid-year must stop there instead of overshooting by a whole
                     * annual increment. */
                    if (pos.y >= r_local.height_m) {
                        s->active = false;
                        if (out_result != NULL) { out_result->stopped_by_height++; }
                        break;
                    }
                    if (order > 0) {
                        f32 env = envelope_now(&c, &r_local, pos.y);
                        f32 radial = v3_len(v3(pos.x, 0.0f, pos.z));
                        if (radial > env * 1.05f) {
                            /* Surface reached part-way through the year. Stop
                             * here rather than overshoot by a whole increment,
                             * and pin the shoot so next year's test decides
                             * whether the crown has grown out past it. */
                            s->at_surface = true;
                            if (!s->surface_counted) {
                                s->surface_counted = true;
                                if (out_result != NULL) {
                                    out_result->stopped_by_envelope++;
                                }
                            }
                            break;
                        }
                    }
                    if (tree_graph_organ_count(graph) + 4u
                            >= c.shoot_organ_budget) {
                        if (out_result != NULL) { out_result->hit_organ_limit = true; }
                        s->active = false;
                        break;
                    }

                    /* Direction correction per emitted internode, with the year's
                     * angular budget divided in proportion to the share of the
                     * year this internode represents, so the total turn per year
                     * remains what the profile intends however coarsely the year
                     * is tessellated. */
                    dir = next_direction(&c, axis_snapshot, pos, dir, order,
                                         &rng_dir,
                                         (f32)group_count / (f32)nodes);

                    r = tree_graph_add_segment(
                        graph, i,
                        order == 0 ? ORGAN_TRUNK_SEGMENT
                                   : (order >= 3 ? ORGAN_TWIG_SEGMENT
                                                 : ORGAN_BRANCH_SEGMENT),
                        pos, dir, group_len, step, &new_organ);
                    if (r == TG_ERR_LIMIT_EXCEEDED) {
                        if (out_result != NULL) { out_result->hit_organ_limit = true; }
                        s->active = false;
                        r = TG_OK;
                        break;
                    }
                    if (r != TG_OK) { goto done; }
                    st.segments_added++;
                    if (out_result != NULL) {
                        out_result->extension_steps++;
                        out_result->segments_created++;
                        if (order > out_result->max_order_reached) {
                            out_result->max_order_reached = order;
                        }
                    }
                    pos = organ_tip(tree_graph_organ(graph, new_organ));

                    /* Claim the space just occupied, so later shoots must go
                     * elsewhere. */
                    {
                        u32 buf[64];
                        u32 cnt = 0, tot = 0;
                        u32 k;
                        (void)spatial_query_radius(&c.attractors, pos,
                                                   c.p->kill_radius_m, buf,
                                                   (u32)TG_COUNTOF(buf), &cnt,
                                                   &tot);
                        for (k = 0; k < cnt; ++k) {
                            if (spatial_remove(&c.attractors, buf[k]) &&
                                out_result != NULL) {
                                out_result->attractors_consumed++;
                            }
                        }
                    }

                    /* Buds at this node.
                     *
                     * A bud ORGAN is created only when the bud actually breaks. A
                     * mature tree carries on the order of a million leaf scars and
                     * dormant buds; storing a graph organ for each would dominate
                     * memory for no benefit. Dormant buds, bud-scale scars and leaf
                     * scars are surface features, and the bark and foliage passes
                     * can derive their positions from the node index and the
                     * phyllotactic angle without an organ existing. */
                    /* One pass per BOTANICAL node in the group. The emitted
                     * internode may stand for several of them, so buds are placed
                     * at their true fractional positions along it rather than all
                     * being stacked at its distal end. Phyllotaxis advances once
                     * per botanical node, which is what keeps the spiral correct
                     * regardless of how coarsely the year was tessellated. */
                    acc = 0.0f;
                    for (gi = 0; gi < group_count; ++gi) {
                        f32 frac;
                        acc += node_len[gi];
                        frac = (group_len > 1e-9f)
                                 ? tg_clampf(acc / group_len, 0.05f, 1.0f)
                                 : 1.0f;
                        s->node_counter++;
                        /* Position within the ANNUAL SHOOT: 0 at its base, 1 at
                         * its distal end. Unchanged by the grouping. */
                        acrotony = (f32)(group_start + gi + 1u) / (f32)nodes;

                        per_node = buds_per_node(c.p, order);
                        for (sub = 0; sub < per_node; ++sub) {
                            f32 ang = phyllotactic_angle(c.p, s->node_counter, sub);
                            const Organ *host;
                            Frame hf;
                            V3 radial, bud_dir, child_base;
                            u32 bud_id, child_axis;
                            f32 set_angle;
                            AxisKind child_kind = AXIS_PLAGIOTROPIC;
                            bool co_dominant = false;

                            if (!should_break_bud(&c, order, s->own_light,
                                                  s->node_counter, acrotony,
                                                  &rng_bud)) {
                                continue;
                            }

                            host = tree_graph_organ(graph, new_organ);
                            hf.origin = host->base;
                            hf.t = host->direction;
                            hf.n = host->frame_ref;
                            hf.b = v3_cross(hf.t, hf.n);
                            radial = frame_ring_dir(hf, ang);
                            {
                                f32 insert = tree_resolved_branch_angle(resolved,
                                                                        order + 1u);
                                bud_dir = v3_norm_or(
                                    v3_add(v3_scale(host->direction, cosf(insert)),
                                           v3_scale(radial, sinf(insert))),
                                    radial);
                            }

                            r = tree_graph_add_attachment(graph, new_organ,
                                                          ORGAN_BUD_AXILLARY, frac,
                                                          ang, bud_dir,
                                                          c.p->tip_radius_m * 2.0f,
                                                          step, &bud_id);
                            if (r == TG_ERR_LIMIT_EXCEEDED) {
                                if (out_result != NULL) {
                                    out_result->hit_organ_limit = true;
                                }
                                r = TG_OK;
                                break;
                            }
                            if (r != TG_OK) { goto done; }
                            st.buds_placed++;
                            if (out_result != NULL) { out_result->buds_created++; }

                            /* The child starts AT ITS BUD, not at the end of the host
                             * segment. With several nodes to a segment this is the
                             * difference between laterals distributed along a branch
                             * and laterals bunched at its joints. */
                            host = tree_graph_organ(graph, new_organ);
                            child_base = v3_add(host->base,
                                                v3_scale(host->direction,
                                                         host->length * frac));
                            set_angle = tree_resolved_set_angle(resolved, order + 1u);
                            {
                                TgRng rng_ang = tg_rng_substream(
                                    resolved->settings.seed, TG_RNG_BRANCH_ANGLE,
                                    bud_id, 0);
                                set_angle += tg_rng_signed(&rng_ang)
                                    * c.p->branch_angle_spread_deg * TG_DEG2RAD_F;
                            }
                            /* REITERATION: as apical control decays, a well-lit high
                             * bud on a Rauh architecture can produce a second
                             * ORTHOTROPIC leader and the crown begins to fork. Massart
                             * keeps strong control for life and never reiterates. */
                            if (c.p->architecture == TREE_ARCH_RAUH) {
                                f32 weakness = tg_remap01f(resolved->apical_control,
                                                           0.62f, 0.42f);
                                f32 high = tg_remap01f(child_base.y,
                                                       resolved->height_m * 0.35f,
                                                       resolved->height_m * 0.85f);
                                TgRng rr = tg_rng_substream(resolved->settings.seed,
                                                            TG_RNG_BUD_FATE, bud_id,
                                                            7u);
                                if (tg_rng_chance(&rr, 0.55f * weakness * high
                                                      * s->own_light)) {
                                    child_kind = AXIS_ORTHOTROPIC;
                                    set_angle *= 0.25f;
                                    co_dominant = true;
                                }
                            }

                            r = tree_graph_add_axis(graph, i, bud_id, child_kind,
                                                    (u8)(order + 1u), step, set_angle,
                                                    child_base, bud_dir, &child_axis);
                            if (r != TG_OK) { goto done; }
                            if (co_dominant) {
                                tree_graph_axis_mut(graph, child_axis)->flags
                                    |= ORGAN_FLAG_CO_DOMINANT;
                                tree_graph_organ_mut(graph, bud_id)->flags
                                    |= ORGAN_FLAG_CO_DOMINANT;
                            }
                            r = shoot_add(&c, child_axis);
                            if (r != TG_OK) { goto done; }
                            /* REFETCH, AND THIS IS NOT OPTIONAL.
                             *
                             * `s` points into the shoot array and `axis_snapshot`
                             * into the graph's axis array. Both have just had an
                             * element appended and may therefore have been
                             * reallocated and moved. Continuing to use the old
                             * pointers is a use-after-free, and it was a real one:
                             * it segfaulted every conifer, whose whorled nodes
                             * append several axes inside a single internode, and
                             * it had merely been getting away with it while the
                             * loop touched `s` less often between appends. */
                            s = shoot_of(&c, i);
                            axis_snapshot = tree_graph_axis(graph, i);
                            {
                                Organ *bo = tree_graph_organ_mut(graph, bud_id);
                                bo->flags &= ~(u32)ORGAN_FLAG_DORMANT;
                            }
                            st.buds_broken++;
                            if (out_result != NULL) {
                                out_result->buds_broken++;
                                out_result->axes_created++;
                            }
                        }
                    }
                }
                TG_UNUSED(stopped);
            }
        }

        st.attractors_remaining = spatial_live_count(&c.attractors);
        {
            u32 k;
            for (k = 0; k < (u32)c.shoots.count; ++k) {
                const Axis *a = tree_graph_axis(graph, k);
                if (a->tip_position.y > st.tallest_point_m) {
                    st.tallest_point_m = a->tip_position.y;
                }
            }
        }
        record_step(&c, &st);
        if (out_result != NULL) {
            out_result->steps_run = (u32)step + 1u;
            out_result->shoots_killed += st.shoots_killed;
        }

        /* Nothing left growing: stop early rather than spinning through
         * remaining steps doing nothing. */
        if (st.active_shoots == 0) { break; }
    }

    if (out_result != NULL) {
        out_result->axes_created = tree_graph_axis_count(graph);
        if (out_result->step_count >= GROWTH_MAX_RECORDED_STEPS) {
            out_result->hit_step_limit = true;
        }
    }

done:
    ctx_destroy(&c);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Root system                                                               */
/*                                                                           */
/* Roots are NOT mirrored branches. Their direction distribution, taper,        */
/* branching density, curvature and response to obstruction are parameterised   */
/* independently, and their growth is driven by soil resistance and the         */
/* supported above-ground structure rather than by light and space competition.  */
/* ------------------------------------------------------------------------- */

/* Birth step for a root organ.
 *
 * WHY ROOTS NEED THIS AT ALL. The root system is generated in one pass after the
 * shoots, because its scale is derived from the realised crown. That is the right
 * dependency, but it left every root organ stamped with step 0 -- and once the mesh
 * began carrying a per-vertex birth step for the construction replay, the
 * consequence became visible in the record itself: a twelve-year tree reported 141
 * organs already in existence at year 0, which is the whole root plate appearing
 * fully formed beneath a seedling.
 *
 * A root's extension is coupled to the shoot system's: a tree that has grown a
 * quarter of its height has grown roughly a quarter of its root spread, because both
 * are driven by the same carbon. So a root organ's birth step is its fractional
 * position along its own root axis, mapped onto the tree's history. It is an
 * inference from the shoot growth curve rather than a simulated root growth model,
 * and it is labelled as one -- but it is a far better answer than "all of it, in the
 * first year". */
static u16 root_birth_step(const TreeResolved *r, f32 fraction_along, u32 order) {
    f32 total = (f32)tg_max_u32(r->growth_steps, 1u);
    /* Higher-order roots branch off wood that already exists, so they cannot
     * predate it; the order offset keeps a fine rootlet from claiming to be older
     * than the major root it hangs from. */
    f32 t = tg_saturatef(fraction_along) + 0.06f * (f32)order;
    f32 step = tg_saturatef(t) * total;
    if (step > total - 1.0f) { step = total - 1.0f; }
    if (step < 0.0f) { step = 0.0f; }
    return (u16)step;
}

TgResult tree_growth_roots(TreeGraph *graph, const TreeResolved *r,
                           GrowthCancel *cancel, GrowthResult *out_result) {
    const TreeProfile *p;
    u32 trunk_base_organ;
    u32 major, i;
    TgResult res = TG_OK;
    f32 soil;

    TG_CHECK(graph != NULL);
    if (r == NULL || r->profile == NULL) { return TG_ERR_INVALID_ARGUMENT; }
    if (graph->finalized) { return TG_ERR_INVALID_STATE; }
    if (tree_graph_organ_count(graph) == 0) {
        TG_LOG_ERRORF(TW_SUB, "roots require an existing shoot system");
        return TG_ERR_INVALID_STATE;
    }
    p = r->profile;
    soil = tg_saturatef(r->settings.environment.soil_resistance);

    /* Attach to the trunk's first segment so the root collar and the trunk flare
     * are structurally continuous rather than two objects meeting at a plane. */
    {
        const Axis *trunk = tree_graph_axis(graph, graph->trunk_axis);
        if (trunk == NULL || trunk->first_organ == TG_INVALID_ID) {
            return TG_ERR_INVALID_STATE;
        }
        trunk_base_organ = trunk->first_organ;
    }

    major = tg_max_u32(p->major_root_count, 2u);

    for (i = 0; i < major; ++i) {
        u32 axis_id;
        V3 dir;
        V3 pos = v3_zero();
        f32 azimuth;
        f32 descent;
        u32 seg;
        TgRng rng = tg_rng_substream(r->settings.seed, TG_RNG_ROOT_INITIATION,
                                     i, 0);

        if (cancel != NULL && cancel->requested != 0) { return TG_ERR_CANCELLED; }

        /* Major roots are spaced around the base with jitter, not at regular
         * angles: a perfectly even star reads as manufactured. The jitter is
         * bounded so two roots cannot start on top of each other. */
        azimuth = (TG_TAU_F / (f32)major) * (f32)i
                + tg_rng_signed(&rng) * (TG_TAU_F / (f32)major) * 0.30f;

        /* Descent angle from the root architecture. A plate system runs almost
         * horizontally; a taproot system dives. This is the parameter that most
         * distinguishes the two categories' root plates. */
        switch (p->root_architecture) {
        case ROOT_ARCH_PLATE:
            descent = tg_rng_range(&rng, 0.10f, 0.30f); break;
        case ROOT_ARCH_TAPROOT:
            descent = tg_rng_range(&rng, 0.55f, 0.95f); break;
        case ROOT_ARCH_BUTTRESSED:
            descent = tg_rng_range(&rng, 0.08f, 0.25f); break;
        case ROOT_ARCH_HEART:
        case ROOT_ARCH_COUNT:
        default:
            descent = tg_rng_range(&rng, 0.30f, 0.60f); break;
        }
        /* Compacted soil forces roots shallower, which is why hard sites produce
         * wide plates rather than deep systems. */
        descent *= tg_lerpf(1.0f, 0.55f, soil);

        dir = v3_norm_or(v3(cosf(azimuth) * (1.0f - descent),
                            -descent,
                            sinf(azimuth) * (1.0f - descent)),
                          v3(0.0f, -1.0f, 0.0f));

        res = tree_graph_add_axis(graph, graph->trunk_axis, trunk_base_organ,
                                  AXIS_ROOT, 0,
                                  0, TG_PI_F * 0.5f + descent * TG_PI_F * 0.5f,
                                  pos, dir, &axis_id);
        if (res != TG_OK) { return res; }

        /* Extend the major root. Segment count and length come from the realised
         * root spread, so a small tree gets a small root system. */
        {
            f32 spread = tg_maxf(r->root_spread_m, 0.25f);
            f32 seg_len = tg_maxf(spread / 9.0f, 0.03f);
            u32 count = 9;
            u32 k;
            V3 cur = dir;

            for (k = 0; k < count; ++k) {
                TgRng rd = tg_rng_substream(r->settings.seed,
                                            TG_RNG_ROOT_DIRECTION, axis_id, k);
                /* Soil obstruction: roots deviate around stones and compacted
                 * layers. Modelled as a bounded random deflection whose magnitude
                 * scales with soil resistance -- recorded as a historical
                 * influence on shape, not simulated collision. */
                f32 deflect = tg_lerpf(0.05f, 0.30f, soil);
                V3 target;

                /* Roots flatten out with distance: strong initial descent, then
                 * a tendency toward the horizontal as they run out from the
                 * stem. This produces the characteristic shallow sweep. */
                {
                    f32 t = (f32)k / (f32)count;
                    f32 want_descent = descent * (1.0f - 0.75f * t);
                    V3 horiz = v3_norm_or(v3(cur.x, 0.0f, cur.z),
                                          v3(cosf(azimuth), 0.0f, sinf(azimuth)));
                    target = v3_norm_or(
                        v3_add(v3_scale(horiz, 1.0f - want_descent),
                               v3_scale(v3(0, -1, 0), want_descent)), cur);
                }
                cur = v3_rotate_toward(cur, target, 0.25f);
                cur = tg_rng_cone(&rd, cur, deflect);
                cur = v3_norm_or(cur, dir);

                /* Never let a lateral root climb above ground level: a root that
                 * emerges into the air is a defect, not a buttress. Buttresses
                 * are generated as trunk-base geometry, not as rising roots. */
                if (pos.y + cur.y * seg_len > 0.0f) {
                    cur = v3_norm_or(v3(cur.x, -tg_absf(cur.y) - 0.05f, cur.z), cur);
                }

                {
                    f32 len = seg_len * tg_rng_range(&rd, 0.8f, 1.2f);
                    res = tree_graph_add_segment(
                        graph, axis_id, ORGAN_ROOT_SEGMENT, pos, cur, len,
                        root_birth_step(r, (f32)(k + 1u) / (f32)count, 0u), &seg);
                    if (res == TG_ERR_LIMIT_EXCEEDED) {
                        if (out_result != NULL) {
                            out_result->hit_organ_limit = true;
                        }
                        return TG_OK;
                    }
                    if (res != TG_OK) { return res; }
                    if (out_result != NULL) { out_result->segments_created++; }
                    pos = organ_tip(tree_graph_organ(graph, seg));
                }

                /* Secondary roots branch off, more frequently further out, which
                 * is the opposite of the shoot system where branching is densest
                 * near the light. */
                {
                    TgRng rb = tg_rng_substream(r->settings.seed,
                                                TG_RNG_ROOT_BRANCHING, axis_id, k);
                    f32 chance = 0.15f + 0.55f * ((f32)k / (f32)count);
                    if (k >= 2 && tg_rng_chance(&rb, chance)) {
                        u32 child;
                        f32 side = tg_rng_chance(&rb, 0.5f) ? 1.0f : -1.0f;
                        V3 perp = v3_norm_or(v3_cross(cur, v3(0, 1, 0)),
                                             v3(1, 0, 0));
                        V3 cdir = v3_norm_or(
                            v3_add(cur, v3_scale(perp, side * tg_rng_range(&rb,
                                                                    0.6f, 1.3f))),
                            cur);
                        /* Secondary roots also stay at or below grade. */
                        if (cdir.y > -0.02f) {
                            cdir = v3_norm_or(v3(cdir.x, -0.15f, cdir.z), cdir);
                        }
                        res = tree_graph_add_axis(graph, axis_id, seg, AXIS_ROOT,
                                                  1, 0, TG_PI_F * 0.5f, pos, cdir,
                                                  &child);
                        if (res != TG_OK) { return res; }
                        {
                            u32 m;
                            V3 cpos = pos;
                            V3 cd = cdir;
                            u32 csegs = 3u + tg_rng_below(&rb, 4u);
                            for (m = 0; m < csegs; ++m) {
                                TgRng rr = tg_rng_substream(r->settings.seed,
                                                            TG_RNG_ROOT_DIRECTION,
                                                            child, m);
                                f32 clen = seg_len * tg_rng_range(&rr, 0.35f, 0.7f);
                                cd = tg_rng_cone(&rr, cd,
                                                 tg_lerpf(0.10f, 0.40f, soil));
                                if (cpos.y + cd.y * clen > 0.0f) {
                                    cd = v3_norm_or(v3(cd.x, -tg_absf(cd.y) - 0.05f,
                                                       cd.z), cd);
                                }
                                res = tree_graph_add_segment(
                                    graph, child, ORGAN_ROOT_SEGMENT, cpos, cd,
                                    clen,
                                    root_birth_step(r, (f32)(k + 1u) / (f32)count,
                                                    1u),
                                    &seg);
                                if (res == TG_ERR_LIMIT_EXCEEDED) {
                                    if (out_result != NULL) {
                                        out_result->hit_organ_limit = true;
                                    }
                                    return TG_OK;
                                }
                                if (res != TG_OK) { return res; }
                                if (out_result != NULL) {
                                    out_result->segments_created++;
                                }
                                cpos = organ_tip(tree_graph_organ(graph, seg));
                            }
                        }
                    }
                }
            }
        }
    }

    /* A taproot, if the architecture calls for one. Generated separately because
     * it is a distinct organ with distinct behaviour, not just another lateral. */
    if (p->taproot_strength > 0.05f) {
        u32 axis_id, seg;
        V3 pos = v3_zero();
        V3 cur = v3(0.0f, -1.0f, 0.0f);
        f32 depth = tg_maxf(r->root_depth_m, 0.15f) * p->taproot_strength;
        u32 count = 6;
        u32 k;
        f32 seg_len = tg_maxf(depth / (f32)count, 0.02f);

        res = tree_graph_add_axis(graph, graph->trunk_axis, trunk_base_organ,
                                  AXIS_ROOT, 0, 0, TG_PI_F, pos, cur, &axis_id);
        if (res != TG_OK) { return res; }
        for (k = 0; k < count; ++k) {
            TgRng rd = tg_rng_substream(r->settings.seed, TG_RNG_ROOT_DIRECTION,
                                        axis_id, k);
            cur = tg_rng_cone(&rd, cur, tg_lerpf(0.06f, 0.22f, soil));
            if (cur.y > -0.4f) {
                cur = v3_norm_or(v3(cur.x, -0.6f, cur.z), v3(0, -1, 0));
            }
            res = tree_graph_add_segment(
                graph, axis_id, ORGAN_ROOT_SEGMENT, pos, cur, seg_len,
                root_birth_step(r, (f32)(k + 1u) / (f32)count, 0u), &seg);
            if (res == TG_ERR_LIMIT_EXCEEDED) {
                if (out_result != NULL) { out_result->hit_organ_limit = true; }
                return TG_OK;
            }
            if (res != TG_OK) { return res; }
            if (out_result != NULL) { out_result->segments_created++; }
            pos = organ_tip(tree_graph_organ(graph, seg));
        }
    }

    if (out_result != NULL) {
        out_result->axes_created = tree_graph_axis_count(graph);
    }
    return TG_OK;
}


/* --------------------------------------------------------------------------
 * Abscission: dead branches fall off.
 * ------------------------------------------------------------------------ */

TgResult tree_growth_shed_dead_wood(TreeGraph *graph,
                                    const TreeResolved *resolved,
                                    u16 final_step, GrowthResult *out_result) {
    u32 n, i, shed = 0;
    u8 *blocked;
    u64 bytes;

    TG_CHECK(graph != NULL && resolved != NULL && resolved->profile != NULL);
    n = tree_graph_organ_count(graph);
    if (out_result != NULL) { out_result->dead_organs_shed = 0; }
    if (n == 0u) { return TG_OK; }

    /* `blocked[i]` means organ i must stay, because something below it is staying.
     * A dead branch cannot drop while a living twig, or a dead one still within its
     * own retention, is attached further out: what falls is the whole distal piece
     * or nothing.
     *
     * One DESCENDING sweep computes it without recursion, because growth appends
     * organs in creation order and a child's id therefore always exceeds its
     * parent's. That invariant is asserted in the graph validator, so relying on it
     * here is relying on something checked rather than something assumed. */
    bytes = (u64)n * sizeof(u8);
    blocked = (u8 *)tg_alloc_zero(bytes);
    if (blocked == NULL) {
        TG_LOG_ERRORF("tree_growth",
                      "abscission needs %llu bytes and could not get them",
                      (unsigned long long)bytes);
        return TG_ERR_OUT_OF_MEMORY;
    }

    for (i = n; i-- > 0;) {
        Organ *o = tree_graph_organ_mut(graph, i);
        bool shed_this = false;

        if (o->type == ORGAN_ROOT_SEGMENT) {
            /* Roots are underground. Nothing weathers them off a standing tree,
             * and a root that vanished would leave the plate visibly incomplete in
             * the cutaway view. Kept, and holds its parent on. */
        } else if (!organ_type_is_segment((OrganType)o->type)) {
            /* A bud carries no tube, so there is nothing about it to shed. What
             * matters is whether it holds its branch on, and the answer depends on
             * whether it is alive:
             *
             *   living  -- a live bud means the wood under it is still attached, so
             *              it blocks. Epicormic buds sit on old wood for decades.
             *   dead    -- transparent. If a dead bud blocked its parent, then every
             *              shoot segment that ever bore a bud would be pinned in
             *              place for ever and abscission would never fire at all.
             *              That is not a subtlety: nearly every segment bears one. */
            if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
        } else if ((o->flags & ORGAN_FLAG_DEAD) == 0) {
            /* Living wood. Stays, and holds everything below it on. */
        } else if (blocked[i]) {
            /* Dead, but something below it is staying. */
        } else {
            f32 radius = 0.5f * (o->radius_base + o->radius_tip);
            f32 retention = tree_dead_branch_retention(resolved->profile, radius);
            /* death_step is 0xFFFF for the living, which this branch has already
             * excluded; guard anyway rather than trust a flag and a field to agree. */
            f32 years_dead = (o->death_step <= final_step)
                               ? (f32)(final_step - o->death_step) : 0.0f;
            shed_this = (years_dead > retention);
        }

        if (shed_this) {
            o->flags |= ORGAN_FLAG_SHED;
            shed++;
        } else if (o->parent != TG_INVALID_ID) {
            blocked[o->parent] = 1u;
        }
    }

    tg_free(blocked, bytes);
    if (out_result != NULL) { out_result->dead_organs_shed = shed; }
    if (shed > 0u) {
        TG_LOG_INFOF("tree_growth",
                     "%u dead organs have dropped off; retention at 5 mm is "
                     "%.1f years for this profile",
                     shed, (double)resolved->profile->dead_branch_retention_years);
    }
    return TG_OK;
}
