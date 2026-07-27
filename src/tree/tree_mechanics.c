#include "tree_mechanics.h"
#include "tree_foliage.h"

#include "../core/log.h"

#include <string.h>

#define TM_SUB "tree_mechanics"

/* Standard gravity. Named rather than inlined because it appears in a moment
 * calculation whose units must be checkable by inspection. */
#define TM_GRAVITY 9.80665f

/* Largest rotation any single segment may contribute. Small-deflection beam
 * theory is applied per segment and accumulated as finite rotations, so the
 * total deflection may be large; but a single segment rotating more than this
 * means the linear theory has been pushed past where it means anything, and
 * allowing it produces the exploding-branch failure mode. Clamping is REPORTED,
 * not silent. */
#define TM_MAX_SEGMENT_ROTATION 0.20f

/* ------------------------------------------------------------------------- */
/* Foliage                                                                   */
/* ------------------------------------------------------------------------- */

bool tree_mechanics_bears_foliage(const Organ *o, const TreeResolved *r,
                                  u16 final_step) {
    u32 age_steps;

    TG_CHECK(o != NULL && r != NULL);
    if (!organ_type_is_segment((OrganType)o->type)) { return false; }
    if (o->type == ORGAN_ROOT_SEGMENT) { return false; }
    if ((o->flags & ORGAN_FLAG_DEAD) != 0) { return false; }
    if (r->foliage_density <= 0.0f) { return false; } /* winter, deciduous */

    age_steps = (final_step > o->created_step)
                  ? (u32)(final_step - o->created_step) : 0u;

    /* Two independent ways to bear foliage, and BOTH are needed.
     *
     * The obvious criterion is cambial age: a deciduous tree leafs out on the
     * current season's extension, an evergreen retains several needle age
     * classes. Used alone it is badly wrong, and measurably so: most shoots in a
     * mature crown have stopped extending (they hit the crown envelope or ran out
     * of resource years ago), so almost nothing qualified and a mature oak came
     * out with 7 m^2 of leaf area instead of hundreds. With no foliage the pipe
     * model has nothing to calibrate against and the whole mechanics pass
     * degenerates -- a 250-year tree deflected 21 m.
     *
     * The missing half is that a shoot which has stopped extending still breaks
     * buds and produces leaves on short shoots every year. Every living TERMINAL
     * shoot bears foliage regardless of when it was formed, which is also why a
     * real crown's foliage sits in a shell on its outer surface. */
    if (age_steps <= (u32)(r->profile->deciduous ? 1u : 5u)
                     * tg_max_u32(r->profile->flushes_per_year, 1u)) {
        return true;
    }
    return (o->flags & ORGAN_FLAG_TERMINAL) != 0;
}

f32 tree_mechanics_segment_leaf_area(const Organ *o, const TreeResolved *r,
                                     u16 final_step) {
    const TreeProfile *p;
    f32 per_metre, area_each;

    TG_CHECK(o != NULL && r != NULL);
    if (!tree_mechanics_bears_foliage(o, r, final_step)) { return 0.0f; }

    p = r->profile;
    per_metre = p->leaves_per_metre_of_shoot;
    if (!(per_metre > 0.0f)) { return 0.0f; }

    /* ONE definition of a leaf's area, integrated from the blade the foliage pass
     * will actually build, including leaf_scale. Duplicating it here with a stated
     * fill factor put this pass 2.5x above the geometry, so the tree bent under
     * leaves that did not exist. */
    area_each = tree_foliage_unit_area(r);

    return o->length * per_metre * area_each * r->foliage_density;
}

/* ------------------------------------------------------------------------- */
/* Pass 0: identify terminal shoots                                          */
/* ------------------------------------------------------------------------- */

/* Sets ORGAN_FLAG_TERMINAL on every living segment that has no living segment
 * continuation or lateral distal to it.
 *
 * Established here rather than during growth because terminality is not a
 * property a shoot has when it is created -- it acquires and loses it as its
 * neighbours grow and die. Deriving it once, from the finished graph, means there
 * is a single definition; growth previously declared the flag and never set it,
 * which is worse than not having it. */
static u32 mark_terminal_shoots(TreeGraph *g) {
    u32 i, n = tree_graph_organ_count(g);
    u32 terminal = 0;

    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        o->flags &= ~(u32)ORGAN_FLAG_TERMINAL;
    }
    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        u32 child;
        bool has_living_distal_segment = false;

        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }

        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *c = tree_graph_organ(g, child);
            if ((c->flags & ORGAN_FLAG_DEAD) == 0) {
                if (organ_type_is_segment((OrganType)c->type)) {
                    has_living_distal_segment = true;
                    break;
                }
                /* A lateral axis hangs off a bud, so look one level further. */
                {
                    u32 gc = c->first_child;
                    while (gc != TG_INVALID_ID) {
                        const Organ *gco = tree_graph_organ(g, gc);
                        if ((gco->flags & ORGAN_FLAG_DEAD) == 0 &&
                            organ_type_is_segment((OrganType)gco->type)) {
                            has_living_distal_segment = true;
                            break;
                        }
                        gc = gco->next_sibling;
                    }
                    if (has_living_distal_segment) { break; }
                }
            }
            child = c->next_sibling;
        }
        if (!has_living_distal_segment) {
            o->flags |= ORGAN_FLAG_TERMINAL;
            terminal++;
        }
    }
    return terminal;
}

/* ------------------------------------------------------------------------- */
/* Pass 1: supported foliage area                                            */
/* ------------------------------------------------------------------------- */

static f32 accumulate_leaf_area(TreeGraph *g, const TreeResolved *r,
                                u16 final_step, u32 *out_bearing) {
    u32 i, n = tree_graph_organ_count(g);
    u32 bearing = 0;

    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        f32 own = tree_mechanics_segment_leaf_area(o, r, final_step);
        if (own > 0.0f) { bearing++; }
        o->supported_leaf_area = own;
    }
    /* Descending id is a valid basipetal order (parent id < child id always), so
     * one reverse loop sums every subtree with no recursion and no work stack. */
    for (i = n; i-- > 0;) {
        const Organ *o = tree_graph_organ(g, i);
        if (o->parent == TG_INVALID_ID) { continue; }
        tree_graph_organ_mut(g, o->parent)->supported_leaf_area
            += o->supported_leaf_area;
    }
    if (out_bearing != NULL) { *out_bearing = bearing; }
    return (n > 0) ? tree_graph_organ(g, 0)->supported_leaf_area : 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Pass 2: radii                                                             */
/* ------------------------------------------------------------------------- */

/* Radius from supported foliage area, in the generalised Leonardo / pipe-model
 * form. delta = 2 recovers the classical area-conserving rule; measured values
 * in real trees are above 2, which is why it is a profile parameter. */
static f32 radius_from_area(f32 area, f32 k, f32 inv_delta, f32 min_radius) {
    f32 r;
    if (!(area > 0.0f)) { return min_radius; }
    r = k * powf(area, inv_delta);
    if (!tg_finitef(r)) { return min_radius; }
    return tg_maxf(r, min_radius);
}

static void assign_radii(TreeGraph *g, const TreeResolved *res,
                         MechanicsResult *out) {
    const TreeProfile *p = res->profile;
    u32 i, n = tree_graph_organ_count(g);
    f32 inv_delta = 1.0f / p->leonardo_exponent;
    f32 k;
    f32 trunk_area;
    u32 trunk_base_organ = TG_INVALID_ID;

    /* Calibration. The pipe model fixes the SHAPE of the radius distribution but
     * not its scale; k sets the scale so that the trunk base lands on the size
     * the resolved individual asked for. Without this the radial model and the
     * height/age model would be two unrelated descriptions of the same tree. */
    {
        const Axis *trunk = tree_graph_axis(g, g->trunk_axis);
        if (trunk != NULL && trunk->first_organ != TG_INVALID_ID) {
            trunk_base_organ = trunk->first_organ;
        }
    }
    trunk_area = (trunk_base_organ != TG_INVALID_ID)
                   ? tree_graph_organ(g, trunk_base_organ)->supported_leaf_area
                   : 0.0f;

    out->trunk_base_radius_target = res->trunk_base_radius_m;
    if (trunk_area > 1e-6f) {
        k = res->trunk_base_radius_m / powf(trunk_area, inv_delta);
    } else {
        /* A tree with no foliage at all -- a deciduous profile in winter, or a
         * seedling whose shoots all died. The pipe model has nothing to work
         * from, so fall back to a geometric taper keyed to the tip radius. */
        k = 0.0f;
    }
    if (!tg_finitef(k) || k < 0.0f) { k = 0.0f; }
    out->pipe_calibration_k = k;

    out->max_radius_m = 0.0f;
    out->min_radius_m = 3.402823466e38f;

    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        f32 area_base, area_tip, r_base, r_tip;
        u32 child;

        if (!organ_type_is_segment((OrganType)o->type)) {
            o->radius_base = 0.0f;
            o->radius_tip = 0.0f;
            continue;
        }

        /* Area supported at the BASE includes this segment's own foliage; at the
         * TIP it does not, so the tip area is exactly the children's total. That
         * single distinction makes every segment taper the right way round with no
         * separate taper rule, and it is derived from the cached subtree sums
         * rather than recomputed, so the two cannot drift apart. */
        area_base = o->supported_leaf_area;
        area_tip = 0.0f;
        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *c = tree_graph_organ(g, child);
            if (organ_type_is_segment((OrganType)c->type)) {
                area_tip += c->supported_leaf_area;
            }
            child = c->next_sibling;
        }
        if (area_tip > area_base) { area_tip = area_base; }

        if (k > 0.0f) {
            r_base = radius_from_area(area_base, k, inv_delta, p->tip_radius_m);
            r_tip = radius_from_area(area_tip, k, inv_delta, p->tip_radius_m);
        } else {
            /* Foliage-free fallback: taper from an estimated base by branch
             * order, which at least preserves the parent-thicker-than-child
             * invariant instead of leaving every radius at the tip value. */
            f32 scale = powf(0.45f, (f32)o->branch_order);
            r_base = tg_maxf(res->trunk_base_radius_m * scale, p->tip_radius_m);
            r_tip = tg_maxf(r_base * 0.9f, p->tip_radius_m);
        }

        /* Roots are not sized by foliage: they are sized by what they anchor and
         * conduct. Taking the radius of the trunk base and dividing it among the
         * major roots keeps the root collar continuous with the flare instead of
         * meeting it at a step. */
        if (o->type == ORGAN_ROOT_SEGMENT) {
            f32 share = 1.0f / sqrtf((f32)tg_max_u32(p->major_root_count, 2u));
            f32 depth_t = tg_saturatef(v3_len(v3(o->base.x, 0.0f, o->base.z))
                                       / tg_maxf(res->root_spread_m, 0.1f));
            f32 order_scale = powf(0.5f, (f32)o->branch_order);
            r_base = tg_maxf(res->trunk_base_radius_m * share * order_scale
                             * (1.0f - 0.75f * depth_t), p->tip_radius_m);
            r_tip = tg_maxf(r_base * 0.88f, p->tip_radius_m);
        }

        /* BASAL FLARE. Radius swells toward ground level, which is what makes a
         * trunk look anchored rather than inserted into a plane. The decay length
         * is proportional to the trunk radius, so the flare scales with the tree
         * instead of being a fixed height. */
        if (o->type == ORGAN_TRUNK_SEGMENT) {
            f32 decay = tg_maxf(res->trunk_base_radius_m
                                * p->basal_flare_height_ratio, 0.05f);
            f32 fb = 1.0f + p->basal_flare_factor * expf(-o->base.y / decay);
            f32 ft = 1.0f + p->basal_flare_factor
                     * expf(-(o->base.y + o->length) / decay);
            r_base *= fb;
            r_tip *= ft;
        }

        /* Hard invariant: never wider at the tip than at the base. */
        if (r_tip > r_base) { r_tip = r_base; }

        o->radius_base = r_base;
        o->radius_tip = r_tip;
        if (r_base > out->max_radius_m) { out->max_radius_m = r_base; }
        if (r_tip < out->min_radius_m) { out->min_radius_m = r_tip; }
    }

    /* Enforce the parent-not-thinner-than-child invariant explicitly.
     *
     * The pipe model satisfies it wherever the area sums are exact, but the flare,
     * the root override and the tip-radius floor are all applied afterwards and
     * can break it locally. Fixing it here, acropetally, is cheaper and far more
     * reliable than hoping the earlier steps never interact badly -- and the graph
     * validator would reject the tree if they did. */
    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        const Organ *par;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->parent == TG_INVALID_ID) { continue; }
        par = tree_graph_organ(g, o->parent);
        /* A lateral axis's first segment hangs off a BUD, not directly off the
         * host segment, so a naive parent check would skip exactly the unions
         * where the invariant matters most. Step through the intervening
         * non-segment organ to find the real woody parent. */
        if (!organ_type_is_segment((OrganType)par->type)) {
            if (par->parent == TG_INVALID_ID) { continue; }
            par = tree_graph_organ(g, par->parent);
            if (!organ_type_is_segment((OrganType)par->type)) { continue; }
        }
        if (o->radius_base > par->radius_base) {
            o->radius_base = par->radius_base;
            if (o->radius_tip > o->radius_base) { o->radius_tip = o->radius_base; }
        }
    }

    if (trunk_base_organ != TG_INVALID_ID) {
        out->trunk_base_radius_m =
            tree_graph_organ(g, trunk_base_organ)->radius_base;
    }
    if (out->min_radius_m > out->max_radius_m) { out->min_radius_m = 0.0f; }
}

/* ------------------------------------------------------------------------- */
/* Pass 3: supported mass                                                    */
/* ------------------------------------------------------------------------- */

static void accumulate_mass(TreeGraph *g, const TreeResolved *res,
                            MechanicsResult *out) {
    const TreeProfile *p = res->profile;
    u32 i, n = tree_graph_organ_count(g);
    f64 wood_total = 0.0, foliage_total = 0.0;

    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        f32 own_wood = 0.0f;

        if (organ_type_is_segment((OrganType)o->type)) {
            /* Volume of a conical frustum: (pi h / 3)(a^2 + ab + b^2). Using the
             * frustum rather than a cylinder at the mean radius matters at the
             * trunk base where taper is strong. */
            f32 a = o->radius_base, b = o->radius_tip;
            f32 vol = (TG_PI_F * o->length / 3.0f) * (a * a + a * b + b * b);
            own_wood = vol * p->wood_density_kgm3;
            wood_total += (f64)own_wood;
        }
        o->supported_mass = own_wood;
    }

    /* Foliage mass is derived from the SAME cached leaf-area figures the geometry
     * will use, so the tree's mass and its appearance cannot disagree: a segment's
     * own leaf area is its subtree total minus its children's subtree totals. */
    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        f32 children_area = 0.0f;
        u32 child;
        f32 own_area;
        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *c = tree_graph_organ(g, child);
            children_area += c->supported_leaf_area;
            child = c->next_sibling;
        }
        own_area = o->supported_leaf_area - children_area;
        if (own_area < 0.0f) { own_area = 0.0f; }
        o->supported_mass += own_area * p->foliage_density_kgm2;
        foliage_total += (f64)(own_area * p->foliage_density_kgm2);
    }

    for (i = n; i-- > 0;) {
        const Organ *o = tree_graph_organ(g, i);
        if (o->parent == TG_INVALID_ID) { continue; }
        tree_graph_organ_mut(g, o->parent)->supported_mass += o->supported_mass;
    }

    out->total_wood_mass_kg = (f32)wood_total;
    out->total_foliage_mass_kg = (f32)foliage_total;
}

/* ------------------------------------------------------------------------- */
/* Pass 4: static deflection and reaction wood                               */
/* ------------------------------------------------------------------------- */

/* Deflection angle of one tapered segment under the load distal to it.
 *
 * Euler-Bernoulli, small deflection, applied PER SEGMENT and accumulated as
 * finite rotations, so the total deflection of a long branch may be large while
 * each individual step stays inside the theory's validity.
 *
 *   theta = M * L / (E * I),   I = pi r^4 / 4 for a circular section
 *
 * The moment arm is the horizontal distance from this segment's base to the
 * centroid of the load, because only the horizontal offset of a vertical weight
 * produces bending. A vertical member therefore bends by zero, which is correct
 * and is asserted by the tests. */
static f32 segment_deflection(const Organ *o, f32 modulus_pa, V3 load_centroid) {
    f32 r_eff, inertia, arm, moment, theta;

    /* Effective radius for stiffness: the mean of base and tip is a closer
     * approximation for a taper than either end alone. */
    r_eff = 0.5f * (o->radius_base + o->radius_tip);
    if (!(r_eff > 0.0f)) { return 0.0f; }
    inertia = 0.25f * TG_PI_F * r_eff * r_eff * r_eff * r_eff;
    if (!(inertia > 1e-20f)) { return 0.0f; }

    {
        V3 d = v3_sub(load_centroid, o->base);
        arm = v3_len(v3(d.x, 0.0f, d.z));
    }
    moment = o->supported_mass * TM_GRAVITY * arm;
    theta = moment * o->length / (modulus_pa * inertia);
    if (!tg_finitef(theta) || theta < 0.0f) { return 0.0f; }
    return theta;
}

static void apply_deflection(TreeGraph *g, const TreeResolved *res,
                             MechanicsResult *out) {
    const TreeProfile *p = res->profile;
    u32 i, n = tree_graph_organ_count(g);
    Quat *accum;
    V3 *orig_base;
    V3 *centroid;
    u64 quat_bytes, base_bytes;

    if (n == 0) { return; }
    if (!tg_ckd_mul_u64(n, sizeof(Quat), &quat_bytes) ||
        !tg_ckd_mul_u64(n, sizeof(V3), &base_bytes)) {
        return;
    }
    accum = (Quat *)tg_alloc(quat_bytes);
    orig_base = (V3 *)tg_alloc(base_bytes);
    centroid = (V3 *)tg_alloc(base_bytes);
    if (accum == NULL || orig_base == NULL || centroid == NULL) {
        if (accum != NULL) { tg_free(accum, quat_bytes); }
        if (orig_base != NULL) { tg_free(orig_base, base_bytes); }
        if (centroid != NULL) { tg_free(centroid, base_bytes); }
        TG_LOG_ERRORF(TM_SUB, "deflection scratch allocation failed");
        return;
    }

    for (i = 0; i < n; ++i) {
        accum[i] = quat_identity();
        orig_base[i] = tree_graph_organ(g, i)->base;
    }

    /* Mass-weighted centroid of everything distal to and including each organ,
     * accumulated basipetally.
     *
     * The moment at a segment's base is the distal weight times the HORIZONTAL
     * DISTANCE TO ITS LINE OF ACTION, which passes through the distal centroid --
     * not through the segment's own tip. Using the tip understates the moment on
     * a short segment carrying a long branch, which is exactly the case that
     * matters most on a mature tree. */
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        f32 own = o->supported_mass;
        u32 child;
        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *c = tree_graph_organ(g, child);
            own -= c->supported_mass;
            child = c->next_sibling;
        }
        if (own < 0.0f) { own = 0.0f; }
        /* Own mass acts at the segment's midpoint. */
        centroid[i] = v3_scale(v3_add(o->base,
                                      v3_scale(o->direction, o->length * 0.5f)),
                               own);
    }
    for (i = n; i-- > 0;) {
        const Organ *o = tree_graph_organ(g, i);
        if (o->parent == TG_INVALID_ID) { continue; }
        centroid[o->parent] = v3_add(centroid[o->parent], centroid[i]);
    }
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        if (o->supported_mass > 1e-9f) {
            centroid[i] = v3_scale(centroid[i], 1.0f / o->supported_mass);
        } else {
            centroid[i] = organ_tip(o);
        }
    }

    /* Acropetal: a parent's rotation is known before its children are visited,
     * which is what makes the rigid propagation a single forward pass.
     *
     * COMPOSITION ORDER MATTERS, AND SO DOES WHICH FRAME THE BEND AXIS LIVES IN.
     * Gravity acts on the shape the tree has ALREADY deformed into, so each
     * segment's bend axis must be computed from its direction after the inherited
     * rotation, not from its undeformed direction. Getting this wrong produced a
     * tree that was mostly sagging correctly but where 57 tips rose by up to 8 mm
     * -- not a global sign error, which is why it needed a per-segment comparison
     * against an unbent baseline to find at all.
     *
     * Each local rotation is therefore a WORLD-frame rotation, and the total is
     * their ordered product applied to the undeformed direction, which is exactly
     * rigid-chain kinematics. */
    for (i = 0; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        Quat inherited = quat_identity();
        Quat local = quat_identity();
        V3 dir_after_inherit;

        if (o->parent != TG_INVALID_ID) { inherited = accum[o->parent]; }
        dir_after_inherit = v3_norm_or(quat_rotate(inherited, o->direction),
                                       o->direction);

        if (organ_type_is_segment((OrganType)o->type) &&
            o->type != ORGAN_ROOT_SEGMENT) {
            out->segments_considered++;
            /* Load acts through the distal mass centroid, computed above from the
             * ORIGINAL (pre-bend) geometry so the estimate does not depend on the
             * order segments are processed in. */
            f32 theta = segment_deflection(o, p->wood_modulus_pa, centroid[i]);

            /* Retention: the fraction of the elastic deflection that the tree
             * grows into permanently. The remainder is notionally recovered by
             * reaction wood, which is also why the eccentricity below scales with
             * the same quantity. */
            theta *= p->sag_retention;

            if (theta > TM_MAX_SEGMENT_ROTATION) {
                theta = TM_MAX_SEGMENT_ROTATION;
                out->clamped_rotations++;
                out->hit_rotation_clamp = true;
            }
            /* Never rotate PAST straight down.
             *
             * Rotating "toward down" by a fixed angle overshoots for a segment
             * that already hangs almost vertically, and past 180 degrees the tip
             * starts rising again. On a 250-year tree this fired 833 times and was
             * caught only by the own-bend sign audit. Physically a freely hanging
             * member has no bending moment left to apply, so clamping to the
             * remaining angle is both the fix and the correct statics. */
            {
                f32 remaining = v3_angle_between(dir_after_inherit,
                                                 v3(0.0f, -1.0f, 0.0f));
                if (theta > remaining) { theta = remaining; }
            }
            if (theta > out->max_segment_rotation_rad) {
                out->max_segment_rotation_rad = theta;
            }
            o->curvature = theta;

            /* REACTION WOOD. Eccentricity scales with how hard this segment is
             * working: the ratio of the applied bend to the stability cap is a
             * bounded, dimensionless measure of that. The SIDE is not stored --
             * it follows from the profile's reaction wood type and the segment's
             * orientation, so there is only one source of truth. */
            {
                f32 load_ratio = tg_saturatef(theta / TM_MAX_SEGMENT_ROTATION);
                f32 ecc = p->reaction_eccentricity * load_ratio;
                o->eccentricity = ecc;
                if (ecc > out->max_eccentricity) { out->max_eccentricity = ecc; }
            }

            if (theta > 1e-7f) {
                /* Rotate toward straight down, about an axis taken from the
                 * ALREADY-DEFORMED direction. The axis vanishes for a vertical
                 * member, so a vertical trunk bends by exactly nothing with no
                 * special case needed. */
                V3 down = v3(0.0f, -1.0f, 0.0f);
                V3 axis = v3_cross(dir_after_inherit, down);
                if (v3_len_sq(axis) > 1e-10f) {
                    local = quat_from_axis_angle(v3_norm_or(axis, v3(1, 0, 0)),
                                                 theta);
                }
            }
        }

        /* World-frame composition: inherited first, then this segment's own bend. */
        accum[i] = quat_normalize(quat_mul(local, inherited));
        o->direction = v3_norm_or(quat_rotate(local, dir_after_inherit),
                                  dir_after_inherit);

        /* Sign audit: this segment's own bend must never raise its own tip. */
        if (o->curvature > 1e-6f &&
            o->direction.y > dir_after_inherit.y + 1e-5f) {
            out->own_bend_upward++;
        }

        /* Re-anchor to the parent's NEW tip so the axis stays connected. Without
         * this the rotation would open a gap at every joint, which is exactly the
         * junction crack the mesh validator exists to catch. */
        if (o->parent != TG_INVALID_ID) {
            const Organ *par = tree_graph_organ(g, o->parent);
            if (organ_type_is_segment((OrganType)o->type) &&
                organ_type_is_segment((OrganType)par->type) &&
                o->axis == par->axis) {
                o->base = organ_tip(par);
            } else {
                /* An organ attached partway along its parent, or the first
                 * segment of a new axis: preserve its position ALONG the parent
                 * and let the parent's rotation carry it. */
                V3 offset = v3_sub(orig_base[i], orig_base[o->parent]);
                o->base = v3_add(par->base, quat_rotate(accum[o->parent], offset));
            }
        }
    }

    /* Largest tip displacement, for the report. */
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        f32 d;
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        d = v3_dist(o->base, orig_base[i]);
        if (d > out->max_tip_deflection_m) { out->max_tip_deflection_m = d; }
    }

    tg_free(accum, quat_bytes);
    tg_free(orig_base, base_bytes);
    tg_free(centroid, base_bytes);
}

/* ------------------------------------------------------------------------- */
/* Frame rebuild                                                             */
/* ------------------------------------------------------------------------- */

/* Bending changed every direction, so the frames propagated during growth are no
 * longer perpendicular to their own tangents. Every cross-section is built from
 * these frames, so leaving them stale shears the whole surface. This is the kind
 * of defect that produces geometry which looks almost right, which is why the
 * graph validator checks frame orthogonality unconditionally. */
static void rebuild_frames(TreeGraph *g) {
    u32 a, na = tree_graph_axis_count(g);

    for (a = 0; a < na; ++a) {
        const Axis *axis = tree_graph_axis(g, a);
        u32 id;
        bool first = true;
        Frame f;

        memset(&f, 0, sizeof f);
        if (axis->first_organ == TG_INVALID_ID) { continue; }

        /* Walk the axis chain in order, exactly as the validator does. */
        id = axis->first_organ;
        while (id != TG_INVALID_ID && id < tree_graph_organ_count(g)) {
            Organ *o = tree_graph_organ_mut(g, id);
            u32 child, next = TG_INVALID_ID;

            if (first) {
                /* Seed from the parent organ where there is one, so bark grain
                 * stays continuous across the union. */
                if (axis->parent_organ != TG_INVALID_ID) {
                    const Organ *par = tree_graph_organ(g, axis->parent_organ);
                    Frame pf;
                    pf.origin = par->base;
                    pf.t = par->direction;
                    pf.n = par->frame_ref;
                    pf.b = v3_cross(pf.t, pf.n);
                    f = frame_propagate_rmf(pf, o->base, o->direction);
                } else {
                    f = frame_make(o->base, o->direction, v3(1.0f, 0.0f, 0.0f));
                }
                first = false;
            } else {
                f = frame_propagate_rmf(f, o->base, o->direction);
            }
            o->frame_ref = f.n;

            for (child = o->first_child; child != TG_INVALID_ID;) {
                const Organ *c = tree_graph_organ(g, child);
                if (c->axis == a && organ_type_is_segment((OrganType)c->type)) {
                    next = child;
                    break;
                }
                child = c->next_sibling;
            }
            id = next;
        }
    }

    /* Attachments hang off segments and must be re-derived from their host's new
     * frame, or a leaf keeps pointing where the branch used to be. */
    {
        u32 i, n = tree_graph_organ_count(g);
        for (i = 0; i < n; ++i) {
            Organ *o = tree_graph_organ_mut(g, i);
            const Organ *par;
            Frame pf;
            V3 radial;
            if (organ_type_is_segment((OrganType)o->type)) { continue; }
            if (o->parent == TG_INVALID_ID) { continue; }
            par = tree_graph_organ(g, o->parent);
            if (!organ_type_is_segment((OrganType)par->type)) { continue; }
            pf.origin = par->base;
            pf.t = par->direction;
            pf.n = par->frame_ref;
            pf.b = v3_cross(pf.t, pf.n);
            o->base = v3_add(par->base,
                             v3_scale(par->direction,
                                      o->attach_along * par->length));
            radial = frame_ring_dir(pf, o->attach_angle);
            {
                Frame af = frame_make(o->base, o->direction, radial);
                o->frame_ref = af.n;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

TgResult tree_mechanics_run(TreeGraph *graph, const TreeResolved *res,
                            MechanicsResult *out) {
    MechanicsResult local;
    u16 final_step = 0;
    u32 i, n;

    TG_CHECK(graph != NULL);
    if (res == NULL || res->profile == NULL) { return TG_ERR_INVALID_ARGUMENT; }
    if (graph->finalized) { return TG_ERR_INVALID_STATE; }
    n = tree_graph_organ_count(graph);
    if (n == 0) {
        TG_LOG_ERRORF(TM_SUB, "mechanics requires a grown graph");
        return TG_ERR_INVALID_STATE;
    }

    memset(&local, 0, sizeof local);

    /* The final step is whatever the growth actually reached, not the requested
     * step count: growth can stop early when nothing is left alive, and foliage
     * retention must be judged against the real end of the history. */
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(graph, i);
        if (o->created_step > final_step) { final_step = o->created_step; }
    }

    /* Physiological age: years of cambial activity at each segment's base. */
    {
        f32 per_step = 1.0f / (f32)tg_max_u32(res->profile->flushes_per_year, 1u);
        for (i = 0; i < n; ++i) {
            Organ *o = tree_graph_organ_mut(graph, i);
            u32 steps = (final_step > o->created_step)
                          ? (u32)(final_step - o->created_step) : 0u;
            o->physiological_age = (f32)steps * per_step;
        }
    }

    local.terminal_shoots = mark_terminal_shoots(graph);
    local.total_leaf_area_m2 = accumulate_leaf_area(graph, res, final_step,
                                                    &local.foliage_bearing_segments);
    assign_radii(graph, res, &local);
    accumulate_mass(graph, res, &local);
    apply_deflection(graph, res, &local);
    rebuild_frames(graph);

    if (out != NULL) { *out = local; }

    TG_LOG_INFOF(TM_SUB,
                 "radii: trunk base %.4f m (target %.4f), max %.4f, min %.5f, k=%.5f",
                 (double)local.trunk_base_radius_m,
                 (double)local.trunk_base_radius_target,
                 (double)local.max_radius_m, (double)local.min_radius_m,
                 (double)local.pipe_calibration_k);
    TG_LOG_INFOF(TM_SUB,
                 "mass: %.1f kg wood + %.1f kg foliage; leaf area %.1f m^2 on %u segments",
                 (double)local.total_wood_mass_kg,
                 (double)local.total_foliage_mass_kg,
                 (double)local.total_leaf_area_m2, local.foliage_bearing_segments);
    TG_LOG_INFOF(TM_SUB,
                 "deflection: max segment rotation %.4f rad, max displacement %.3f m, "
                 "%u clamped, max eccentricity %.3f",
                 (double)local.max_segment_rotation_rad,
                 (double)local.max_tip_deflection_m, local.clamped_rotations,
                 (double)local.max_eccentricity);

    /* Surface it when the beam model has been pushed outside its validity range
     * instead of quietly clamping and producing a collapsed tree.
     *
     * A high clamp count is a symptom, not the disease: it means branches are too
     * thin for the wood they carry, which in turn means the pipe model was
     * calibrated against too little foliage. See docs/limitations.md -- the known
     * sparse-skeleton gap propagates to here. */
    if (local.clamped_rotations * 100u > tg_max_u32(local.segments_considered, 1u)) {
        TG_LOG_WARNF(TM_SUB,
                     "%u of %u segments hit the rotation clamp (>1%%): the beam "
                     "model is outside its validity range, most likely because "
                     "branch radii are too thin for their supported wood mass",
                     local.clamped_rotations, local.segments_considered);
    }
    return TG_OK;
}
