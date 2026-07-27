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
    f32  resource;        /* acropetal allocation this step                   */
    u16  suppressed_steps;
    u32  node_counter;    /* drives phyllotaxis                              */
    u32  first_child_slot;/* linked list of child shoots, for the passes      */
    u32  next_sibling_slot;
} ShootState;

typedef struct GrowthCtx {
    TreeGraph          *graph;
    const TreeResolved *r;
    const TreeProfile  *p;
    TgArray             shoots;      /* ShootState, indexed by axis id        */
    SpatialGrid         attractors;
    AttractorCloud      cloud;
    /* Occupancy grid over existing shoot tips, rebuilt each step. Used for the
     * local-density term of the light estimate. */
    TgArray             tip_positions; /* V3                                  */
    SpatialGrid         tips;
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
    f32 vertical, directional, density;

    /* 1. Vertical exposure, measured against the tree's CURRENT height. Foliage
     *    above shades foliage below, so the apex of a tree of any age is by
     *    definition the most exposed point on it. Using the eventual target
     *    height here instead is a genuine modelling error: it makes every shoot
     *    on a young tree read as shaded and self-prunes the leader. */
    vertical = tg_remap01f(pos.y, 0.0f, tg_maxf(c->current_height, 0.2f));
    vertical = 0.25f + 0.75f * vertical;

    /* 2. Directional exposure. How far the shoot sits toward the illuminated
     *    side of the crown. This is what makes asymmetry causal rather than
     *    decorative. */
    {
        V3 radial = v3(pos.x, 0.0f, pos.z);
        f32 dist = v3_len(radial);
        /* Current spread, for the same reason as the height above. */
        f32 env = tg_maxf(c->current_spread, 0.05f);
        f32 outwardness = tg_saturatef(dist / env);
        f32 align = 0.5f;
        if (dist > 1e-4f) {
            align = 0.5f + 0.5f * v3_dot(v3_scale(radial, 1.0f / dist),
                                         r->crown_offset_dir);
        }
        directional = tg_lerpf(1.0f,
                               tg_lerpf(0.35f, 1.0f, align),
                               r->settings.environment.light_anisotropy)
                    /* Interior shoots receive less regardless of side. */
                    * tg_lerpf(0.55f, 1.0f, outwardness);
    }

    /* 3. DIRECTIONAL self-shading. Only foliage that lies BETWEEN the shoot and
     *    the light shades it.
     *
     *    An earlier version counted every neighbouring tip within the influence
     *    radius. That was measured to be wrong in a way that destroyed the model:
     *    a shoot's own parent and siblings are always nearby, so the baseline
     *    occlusion was high everywhere, every interior shoot fell below the
     *    mortality threshold, and the tree never reached a second branch order.
     *    Worse, the mortality that did occur had no height bias -- it was not
     *    shade driven at all.
     *
     *    Counting only occluders in the light hemisphere gives the apex an
     *    unshaded value and the lower interior a heavily shaded one, which is
     *    both physically right and what produces crown lift, interior gaps and
     *    lower-branch death. */
    {
        u32 buf[256];
        u32 count = 0, total = 0;
        u32 occluders = 0;
        f32 radius = tg_maxf(c->p->influence_radius_m * 1.2f, 0.05f);
        f32 inner = radius * 0.15f;
        V3 light_dir = r->settings.environment.light_direction;
        u32 k;

        (void)spatial_query_radius(&c->tips, pos, radius, buf,
                                   (u32)TG_COUNTOF(buf), &count, &total);
        for (k = 0; k < count; ++k) {
            V3 d = v3_sub(((const V3 *)c->tip_positions.data)[buf[k]], pos);
            f32 len = v3_len(d);
            /* Ignore immediate neighbours: those are the shoot's own axis and
             * its siblings at the same node, which do not shade it. */
            if (len < inner) { continue; }
            if (v3_dot(d, light_dir) > 0.30f * len) { occluders++; }
        }
        /* If the query truncated, scale the directional count up in proportion
         * rather than under-reporting occlusion. Stated explicitly because a
         * silent undercount would look like a healthy crown. */
        if (total > count && count > 0) {
            occluders = (u32)((f32)occluders * (f32)total / (f32)count);
        }

        density = 1.0f / (1.0f + 0.16f * (f32)occluders);
        /* Shade tolerance softens the penalty rather than removing it: blending
         * toward sqrt compresses the range while preserving the ordering, so a
         * tolerant profile still suffers from crowding, just less. */
        density = tg_lerpf(density, sqrtf(density), c->p->shade_tolerance);
    }

    /* 4. NEIGHBOUR shading from the surrounding stand.
     *
     * Terms 1-3 model the tree shading itself. Without this fourth term,
     * canopy_closure changed only the crown's GEOMETRY, and a forest tree -- with
     * its narrow, lifted crown -- ended up suffering LESS self-shading than an
     * open-grown one. The measured dead fraction came out lower in the forest
     * than in the open, which is backwards.
     *
     * Neighbours block lateral light while overhead light still reaches the top,
     * so the penalty is strongest low in the crown and vanishes at the apex. This
     * is the mechanism that lifts a forest tree's live crown and cleans its bole. */
    {
        f32 closure = tg_saturatef(r->settings.environment.canopy_closure);
        f32 relative_height = tg_remap01f(pos.y, 0.0f,
                                          tg_maxf(c->current_height, 0.2f));
        f32 exposed_above = relative_height * relative_height;
        f32 neighbour = tg_lerpf(1.0f, tg_lerpf(0.12f, 1.0f, exposed_above),
                                 closure);
        /* Higher orders sit deeper inside foliage on average. */
        f32 order_penalty = 1.0f - 0.06f * (f32)tg_min_u32(order, 5);
        f32 light = vertical * directional * density * neighbour * order_penalty;
        return tg_saturatef(light);
    }
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

        denom = lambda * s->own_light + (1.0f - lambda) * q_children;
        if (!(denom > 1e-9f)) {
            /* Nothing is gathering light anywhere below here. Keep the resource
             * at the apex rather than dividing by zero. */
            s->resource = v;
            continue;
        }
        s->resource = v * (lambda * s->own_light) / denom;
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

static V3 next_direction(GrowthCtx *c, const Axis *axis, V3 pos, V3 current,
                         u32 order, TgRng *rng) {
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
        f32 budget = c->p->max_turn_per_step * tg_lerpf(0.5f, 1.0f, grav);
        V3 turned = v3_rotate_toward(current, desired, budget);
        /* Small isotropic wander so two shoots in identical circumstances do not
         * grow identically. Deliberately small: individuality comes from history,
         * not from noise. */
        f32 wander = 0.06f * tg_lerpf(1.0f, 2.0f, tg_saturatef((f32)order / 4.0f));
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
static bool should_break_bud(GrowthCtx *c, u32 order, f32 light,
                             u32 node_index, TgRng *rng) {
    f32 chance;

    if (order + 1u > c->p->max_branch_order) { return false; }
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
        /* Calibrated by measuring the resulting self-shading mortality: at
         * 0.30 the broadleaf crown became crowded enough that 43% of shoot
         * segments died, which is a stressed tree, not a typical one. */
        chance = 0.26f * lit;
    }
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

static TgResult rebuild_tip_grid(GrowthCtx *c) {
    u32 i, n;

    tg_array_clear(&c->tip_positions);
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
        {
            TgResult r = tg_array_push(&c->tip_positions, &tip, NULL);
            if (r != TG_OK) { return r; }
        }
    }
    spatial_destroy(&c->tips);
    return spatial_build(&c->tips, (const V3 *)c->tip_positions.data,
                         (u32)c->tip_positions.count,
                         tg_maxf(c->p->influence_radius_m * 0.8f, 0.05f),
                         1u << 21);
}

/* ------------------------------------------------------------------------- */
/* Main growth loop                                                          */
/* ------------------------------------------------------------------------- */

static void ctx_destroy(GrowthCtx *c) {
    tg_array_free(&c->shoots);
    tg_array_free(&c->tip_positions);
    spatial_destroy(&c->attractors);
    spatial_destroy(&c->tips);
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
    r = TG_ARRAY_INIT(&c.tip_positions, V3, 1024, "growth.tips");
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

        r = rebuild_tip_grid(&c);
        if (r != TG_OK) { goto done; }

        /* 1. light for every active apex */
        n = (u32)c.shoots.count;
        for (i = 0; i < n; ++i) {
            ShootState *s = shoot_of(&c, i);
            const Axis *a = tree_graph_axis(graph, i);
            if (!s->active) { s->own_light = 0.0f; continue; }
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
            st.total_light += s->own_light;
            st.active_shoots++;
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
                    continue;
                }
                /* Suppressed but alive: extends very little. */
                res *= 0.25f;
            } else if (s->suppressed_steps > 0) {
                s->suppressed_steps--;
            }

            /* A shoot with essentially no resource simply does not extend this
             * step. It is not dead: it may recover if a neighbour dies. */
            if (res < 0.010f) { continue; }

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
             * The trunk is exempt from the radial limit (it lives below the live
             * crown by definition) but is capped in height, which also removes
             * the leader's ability to overshoot the target. */
            if (pos.y >= r_local.height_m) {
                s->active = false;
                continue;
            }
            if (order > 0) {
                f32 env = tree_resolved_envelope_radius(&r_local, pos.y);
                f32 radial = v3_len(v3(pos.x, 0.0f, pos.z));
                /* 5% slack so a shoot may just reach the surface rather than
                 * stopping short of it. */
                if (radial > env * 1.05f) {
                    s->active = false;
                    continue;
                }
            }

            rng_dir = tg_rng_substream(resolved->settings.seed,
                                       TG_RNG_SHOOT_DIRECTION, i, step);
            rng_bud = tg_rng_substream(resolved->settings.seed,
                                       TG_RNG_BUD_FATE, i, step);

            dir = next_direction(&c, axis_snapshot, pos, dir, order, &rng_dir);

            /* Internode length from the profile, modulated by resource. Longer
             * internodes on vigorous shoots and short congested ones on weak
             * shoots is a real and highly visible signal of growing conditions. */
            {
                f32 base = tree_resolved_internode_length(resolved, order);
                TgRng rng_len = tg_rng_substream(resolved->settings.seed,
                                                 TG_RNG_INTERNODE_LENGTH, i, step);
                f32 vary = 1.0f + tg_rng_signed(&rng_len)
                                  * c.p->internode_length_spread;
                /* Annual extension DECLINES WITH AGE, following the derivative of
                 * the height curve rather than staying constant.
                 *
                 * This is what makes the growth model and the resolved height
                 * model agree by construction: with h(t) = H(1 - exp(-t/k)), the
                 * increment is (H/k)exp(-t/k), so summing the leader's internodes
                 * over the whole history reproduces the target height exactly. A
                 * constant internode length made a 60-year tree 29.7 m tall
                 * against a 16.1 m target -- a real inconsistency between two
                 * parts of the same model, not a tuning nuisance. */
                /* The decay clock runs on the AXIS's OWN AGE, not the tree's.
                 *
                 * A branch that breaks in year 40 begins its own juvenile phase
                 * then; it does not inherit the trunk's already-exhausted vigour.
                 * Using the tree's age for laterals was measured to hold the
                 * broadleaf crown to a 5.2 m radius against a 9.6 m envelope,
                 * because every branch born late was extending at the leader's
                 * old-age rate from the moment it appeared. Laterals also mature
                 * faster than the trunk, hence the shorter time constant. */
                f32 own_age = (f32)step - (f32)axis_snapshot->created_step;
                f32 k_axis = (order == 0) ? c.age_decay_k : c.age_decay_k * 0.55f;
                f32 decay = expf(-tg_maxf(own_age, 0.0f) / k_axis);
                /* res ~= 1 is the nominal allocation, so the response is centred
                 * there: a well-supplied shoot extends a full internode and a
                 * starved one a short congested fraction of it. */
                f32 len = base * decay
                        * tg_clampf(0.45f + res * 0.55f, 0.30f, 1.5f) * vary;
                len = tg_maxf(len, base * decay * 0.15f);

                r = tree_graph_add_segment(graph, i,
                                           order == 0 ? ORGAN_TRUNK_SEGMENT
                                                      : (order >= 3
                                                            ? ORGAN_TWIG_SEGMENT
                                                            : ORGAN_BRANCH_SEGMENT),
                                           pos, dir, len, step, &new_organ);
                if (r == TG_ERR_LIMIT_EXCEEDED) {
                    if (out_result != NULL) { out_result->hit_organ_limit = true; }
                    s->active = false;
                    r = TG_OK;
                    continue;
                }
                if (r != TG_OK) { goto done; }
                st.segments_added++;
                if (out_result != NULL) {
                    out_result->segments_created++;
                    if (order > out_result->max_order_reached) {
                        out_result->max_order_reached = order;
                    }
                }
            }

            /* Consume attractors the new tip has reached. This is what stops the
             * crown filling uniformly: space is claimed, and later shoots must go
             * elsewhere. */
            {
                V3 tip = organ_tip(tree_graph_organ(graph, new_organ));
                u32 buf[64];
                u32 cnt = 0, tot = 0;
                u32 k;
                (void)spatial_query_radius(&c.attractors, tip, c.p->kill_radius_m,
                                           buf, (u32)TG_COUNTOF(buf), &cnt, &tot);
                for (k = 0; k < cnt; ++k) {
                    if (spatial_remove(&c.attractors, buf[k]) &&
                        out_result != NULL) {
                        out_result->attractors_consumed++;
                    }
                }
            }

            /* --- nodes, buds, and bud break ------------------------------- */
            {
                u32 per_node = buds_per_node(c.p, order);
                u32 sub;
                s->node_counter++;
                for (sub = 0; sub < per_node; ++sub) {
                    f32 ang = phyllotactic_angle(c.p, s->node_counter, sub);
                    const Organ *host = tree_graph_organ(graph, new_organ);
                    Frame hf;
                    V3 radial, bud_dir;
                    u32 bud_id;

                    hf.origin = host->base;
                    hf.t = host->direction;
                    hf.n = host->frame_ref;
                    hf.b = v3_cross(hf.t, hf.n);
                    radial = frame_ring_dir(hf, ang);

                    /* The bud points outward and distally at the profile's
                     * insertion angle for the NEXT order -- the angle is a
                     * property of the union, so it is set here where the bud is
                     * created rather than when it breaks. */
                    {
                        f32 insert = tree_resolved_branch_angle(resolved,
                                                                order + 1u);
                        bud_dir = v3_norm_or(
                            v3_add(v3_scale(host->direction, cosf(insert)),
                                   v3_scale(radial, sinf(insert))),
                            radial);
                    }

                    r = tree_graph_add_attachment(graph, new_organ,
                                                  ORGAN_BUD_AXILLARY, 0.98f, ang,
                                                  bud_dir, c.p->tip_radius_m * 2.0f,
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

                    if (should_break_bud(&c, order, s->own_light, s->node_counter,
                                         &rng_bud)) {
                        u32 child_axis;
                        V3 child_base = organ_tip(tree_graph_organ(graph,
                                                                   new_organ));
                        f32 set_angle =
                            tree_resolved_set_angle(resolved, order + 1u);
                        TgRng rng_ang = tg_rng_substream(resolved->settings.seed,
                                                         TG_RNG_BRANCH_ANGLE,
                                                         bud_id, 0);
                        /* Spread around the profile's insertion angle: real
                         * unions vary, and identical angles read as a template. */
                        set_angle += tg_rng_signed(&rng_ang)
                                   * c.p->branch_angle_spread_deg * TG_DEG2RAD_F;

                        /* REITERATION: a Rauh architecture becomes DECURRENT as
                         * apical control decays. Once control is weak, a
                         * well-lit bud high in the crown can produce a second
                         * ORTHOTROPIC leader rather than a plagiotropic lateral,
                         * and the crown starts forking. This is the mechanism
                         * behind the difference between an oak's broad forked
                         * crown and a fir's single spire -- Massart keeps strong
                         * control for life and never reiterates.
                         *
                         * Without this the two architectures differed only in
                         * their numbers, not in their structure. */
                        AxisKind child_kind = AXIS_PLAGIOTROPIC;
                        bool co_dominant = false;
                        if (c.p->architecture == TREE_ARCH_RAUH) {
                            f32 weakness = tg_remap01f(resolved->apical_control,
                                                       0.62f, 0.42f);
                            f32 high = tg_remap01f(child_base.y,
                                                   resolved->height_m * 0.35f,
                                                   resolved->height_m * 0.85f);
                            TgRng rr = tg_rng_substream(resolved->settings.seed,
                                                        TG_RNG_BUD_FATE,
                                                        bud_id, 7u);
                            if (tg_rng_chance(&rr, 0.55f * weakness * high
                                                   * s->own_light)) {
                                child_kind = AXIS_ORTHOTROPIC;
                                set_angle *= 0.25f; /* turns toward vertical */
                                co_dominant = true;
                            }
                        }
                        r = tree_graph_add_axis(graph, i, bud_id, child_kind,
                                                (u8)(order + 1u), step,
                                                set_angle, child_base, bud_dir,
                                                &child_axis);
                        if (r != TG_OK) { goto done; }
                        if (co_dominant) {
                            tree_graph_axis_mut(graph, child_axis)->flags |=
                                ORGAN_FLAG_CO_DOMINANT;
                            tree_graph_organ_mut(graph, bud_id)->flags |=
                                ORGAN_FLAG_CO_DOMINANT;
                        }
                        r = shoot_add(&c, child_axis);
                        if (r != TG_OK) { goto done; }
                        /* The bud has broken: it is no longer dormant, and the
                         * record of that transition is what later distinguishes
                         * a bud-scale scar from a live bud. */
                        {
                            Organ *b = tree_graph_organ_mut(graph, bud_id);
                            b->flags &= ~(u32)ORGAN_FLAG_DORMANT;
                        }
                        st.buds_broken++;
                        if (out_result != NULL) {
                            out_result->buds_broken++;
                            out_result->axes_created++;
                        }
                    }
                }
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
                    res = tree_graph_add_segment(graph, axis_id,
                                                 ORGAN_ROOT_SEGMENT, pos, cur,
                                                 len, 0, &seg);
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
                                res = tree_graph_add_segment(graph, child,
                                                             ORGAN_ROOT_SEGMENT,
                                                             cpos, cd, clen, 0,
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
            res = tree_graph_add_segment(graph, axis_id, ORGAN_ROOT_SEGMENT, pos,
                                         cur, seg_len, 0, &seg);
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
