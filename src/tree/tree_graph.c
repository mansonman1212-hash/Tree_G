#include "tree_graph.h"

#include "../core/log.h"

#include <stdio.h>
#include <string.h>

#define TG_SUB "tree_graph"

/* ------------------------------------------------------------------------- */
/* Names and type predicates                                                 */
/* ------------------------------------------------------------------------- */

const char *organ_type_name(OrganType t) {
    switch (t) {
    case ORGAN_NONE:            return "none";
    case ORGAN_TRUNK_SEGMENT:   return "trunk_segment";
    case ORGAN_BRANCH_SEGMENT:  return "branch_segment";
    case ORGAN_TWIG_SEGMENT:    return "twig_segment";
    case ORGAN_ROOT_SEGMENT:    return "root_segment";
    case ORGAN_BUD_TERMINAL:    return "bud_terminal";
    case ORGAN_BUD_AXILLARY:    return "bud_axillary";
    case ORGAN_PETIOLE:         return "petiole";
    case ORGAN_LEAF:            return "leaf";
    case ORGAN_NEEDLE:          return "needle";
    case ORGAN_FASCICLE:        return "fascicle";
    case ORGAN_LEAF_SCAR:       return "leaf_scar";
    case ORGAN_BRANCH_SCAR:     return "branch_scar";
    case ORGAN_KNOT:            return "knot";
    case ORGAN_WOUND:           return "wound";
    case ORGAN_TYPE_COUNT:      break;
    }
    return "invalid";
}

bool organ_type_is_segment(OrganType t) {
    return t == ORGAN_TRUNK_SEGMENT || t == ORGAN_BRANCH_SEGMENT ||
           t == ORGAN_TWIG_SEGMENT || t == ORGAN_ROOT_SEGMENT;
}

bool organ_type_is_woody(OrganType t) {
    return organ_type_is_segment(t) || t == ORGAN_KNOT || t == ORGAN_WOUND ||
           t == ORGAN_BRANCH_SCAR;
}

const char *axis_kind_name(AxisKind k) {
    switch (k) {
    case AXIS_ORTHOTROPIC:  return "orthotropic";
    case AXIS_PLAGIOTROPIC: return "plagiotropic";
    case AXIS_ROOT:         return "root";
    case AXIS_KIND_COUNT:   break;
    }
    return "invalid";
}

const char *graph_issue_name(GraphIssueKind k) {
    switch (k) {
    case GRAPH_ISSUE_NONE:                    return "none";
    case GRAPH_ISSUE_BAD_PARENT_ID:           return "bad_parent_id";
    case GRAPH_ISSUE_PARENT_NOT_BEFORE_CHILD: return "parent_not_before_child";
    case GRAPH_ISSUE_CHILD_LIST_BROKEN:       return "child_list_broken";
    case GRAPH_ISSUE_SIBLING_CYCLE:           return "sibling_cycle";
    case GRAPH_ISSUE_BAD_AXIS_ID:             return "bad_axis_id";
    case GRAPH_ISSUE_AXIS_NOT_CONTIGUOUS:     return "axis_not_contiguous";
    case GRAPH_ISSUE_AXIS_COUNT_MISMATCH:     return "axis_count_mismatch";
    case GRAPH_ISSUE_ORDER_NOT_MONOTONE:      return "order_not_monotone";
    case GRAPH_ISSUE_NON_FINITE:              return "non_finite";
    case GRAPH_ISSUE_NON_UNIT_DIRECTION:      return "non_unit_direction";
    case GRAPH_ISSUE_NON_POSITIVE_LENGTH:     return "non_positive_length";
    case GRAPH_ISSUE_NON_POSITIVE_RADIUS:     return "non_positive_radius";
    case GRAPH_ISSUE_RADIUS_INCREASES_DISTALLY:
                                              return "radius_increases_distally";
    case GRAPH_ISSUE_CHILD_THICKER_THAN_PARENT:
                                              return "child_thicker_than_parent";
    case GRAPH_ISSUE_DISCONNECTED_SEGMENT:    return "disconnected_segment";
    case GRAPH_ISSUE_FRAME_NOT_ORTHOGONAL:    return "frame_not_orthogonal";
    case GRAPH_ISSUE_DEAD_BEFORE_BORN:        return "dead_before_born";
    case GRAPH_ISSUE_KIND_COUNT:              break;
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------- */

TgResult tree_graph_init(TreeGraph *g, u32 reserve_organs, u32 max_organs) {
    TgResult r;

    TG_CHECK(g != NULL);
    memset(g, 0, sizeof *g);
    g->trunk_axis = TG_INVALID_ID;
    g->root_axis_first = TG_INVALID_ID;
    g->max_organs = max_organs != 0 ? max_organs : 1500000u;

    r = TG_ARRAY_INIT(&g->organs, Organ, reserve_organs, "graph.organs");
    if (r != TG_OK) { goto fail; }
    r = TG_ARRAY_INIT(&g->axes, Axis, reserve_organs / 8u + 8u, "graph.axes");
    if (r != TG_OK) { goto fail; }
    return TG_OK;

fail:
    tree_graph_destroy(g);
    return r;
}

void tree_graph_destroy(TreeGraph *g) {
    if (g == NULL) { return; }
    tg_array_free(&g->organs);
    tg_array_free(&g->axes);
    memset(g, 0, sizeof *g);
    g->trunk_axis = TG_INVALID_ID;
    g->root_axis_first = TG_INVALID_ID;
}

void tree_graph_reset(TreeGraph *g) {
    TG_CHECK(g != NULL);
    tg_array_clear(&g->organs);
    tg_array_clear(&g->axes);
    g->trunk_axis = TG_INVALID_ID;
    g->root_axis_first = TG_INVALID_ID;
    g->finalized = false;
}

void tree_graph_finalize(TreeGraph *g) {
    TG_CHECK(g != NULL);
    tg_array_shrink_to_fit(&g->organs);
    tg_array_shrink_to_fit(&g->axes);
    g->finalized = true;
}

const Organ *tree_graph_organ(const TreeGraph *g, u32 id) {
    TG_CHECK(g != NULL);
    if (id >= g->organs.count) { return NULL; }
    return &((const Organ *)g->organs.data)[id];
}

const Axis *tree_graph_axis(const TreeGraph *g, u32 id) {
    TG_CHECK(g != NULL);
    if (id >= g->axes.count) { return NULL; }
    return &((const Axis *)g->axes.data)[id];
}

Organ *tree_graph_organ_mut(TreeGraph *g, u32 id) {
    TG_CHECK(g != NULL);
    TG_CHECK_MSG(!g->finalized, "attempted to mutate a finalized tree graph");
    TG_CHECK(id < g->organs.count);
    return &((Organ *)g->organs.data)[id];
}

Axis *tree_graph_axis_mut(TreeGraph *g, u32 id) {
    TG_CHECK(g != NULL);
    TG_CHECK_MSG(!g->finalized, "attempted to mutate a finalized tree graph");
    TG_CHECK(id < g->axes.count);
    return &((Axis *)g->axes.data)[id];
}

V3 organ_tip(const Organ *o) {
    TG_CHECK(o != NULL);
    return v3_add(o->base, v3_scale(o->direction, o->length));
}

/* ------------------------------------------------------------------------- */
/* Construction                                                              */
/* ------------------------------------------------------------------------- */

static void link_child(TreeGraph *g, u32 parent_id, u32 child_id) {
    Organ *parent;
    if (parent_id == TG_INVALID_ID) { return; }
    parent = tree_graph_organ_mut(g, parent_id);
    if (parent->first_child == TG_INVALID_ID) {
        parent->first_child = child_id;
    } else {
        Organ *last = tree_graph_organ_mut(g, parent->last_child);
        last->next_sibling = child_id;
    }
    parent->last_child = child_id;
}

static void organ_init(Organ *o, u32 id, u32 parent, u32 axis, OrganType type,
                       u8 order, u16 step) {
    memset(o, 0, sizeof *o);
    o->id = id;
    o->parent = parent;
    o->first_child = TG_INVALID_ID;
    o->last_child = TG_INVALID_ID;
    o->next_sibling = TG_INVALID_ID;
    o->axis = axis;
    o->type = (u8)type;
    o->branch_order = order;
    o->created_step = step;
    o->death_step = 0xFFFFu;
    o->flags = ORGAN_FLAG_ALIVE;
    o->direction = v3(0.0f, 1.0f, 0.0f);
    o->frame_ref = v3(1.0f, 0.0f, 0.0f);
    o->vigor = 1.0f;
    o->light = 1.0f;
}

TgResult tree_graph_add_axis(TreeGraph *g, u32 parent_axis, u32 parent_organ,
                             AxisKind kind, u8 order, u16 step, f32 set_angle,
                             V3 tip_position, V3 tip_direction, u32 *out_axis) {
    Axis a;
    u64 index;
    TgResult r;

    TG_CHECK(g != NULL);
    if (g->finalized) { return TG_ERR_INVALID_STATE; }
    if ((u32)kind >= (u32)AXIS_KIND_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (parent_axis != TG_INVALID_ID && parent_axis >= g->axes.count) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (parent_organ != TG_INVALID_ID && parent_organ >= g->organs.count) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (!v3_finite(tip_position) || !v3_finite(tip_direction)) {
        return TG_ERR_INVALID_ARGUMENT;
    }

    memset(&a, 0, sizeof a);
    a.parent_axis = parent_axis;
    a.parent_organ = parent_organ;
    a.first_organ = TG_INVALID_ID;
    a.last_organ = TG_INVALID_ID;
    a.organ_count = 0;
    a.kind = (u8)kind;
    a.order = order;
    a.created_step = step;
    a.flags = ORGAN_FLAG_ALIVE;
    a.set_angle = set_angle;
    a.tip_position = tip_position;
    a.tip_direction = v3_norm_or(tip_direction, v3(0.0f, 1.0f, 0.0f));
    a.accumulated_light = 0.0f;

    r = tg_array_push(&g->axes, &a, &index);
    if (r != TG_OK) { return r; }
    ((Axis *)g->axes.data)[index].id = (u32)index;

    if (g->trunk_axis == TG_INVALID_ID && kind == AXIS_ORTHOTROPIC && order == 0) {
        g->trunk_axis = (u32)index;
    }
    if (kind == AXIS_ROOT && g->root_axis_first == TG_INVALID_ID) {
        g->root_axis_first = (u32)index;
    }
    if (out_axis != NULL) { *out_axis = (u32)index; }
    return TG_OK;
}

TgResult tree_graph_add_segment(TreeGraph *g, u32 axis_id, OrganType type,
                               V3 base, V3 direction, f32 length, u16 step,
                               u32 *out_organ) {
    Axis *axis;
    Organ o;
    u64 index;
    TgResult r;
    u32 parent_id;

    TG_CHECK(g != NULL);
    if (g->finalized) { return TG_ERR_INVALID_STATE; }
    if (axis_id >= g->axes.count) { return TG_ERR_INVALID_ARGUMENT; }
    if (!organ_type_is_segment(type)) { return TG_ERR_INVALID_ARGUMENT; }
    if (!v3_finite(base) || !v3_finite(direction)) { return TG_ERR_INVALID_ARGUMENT; }
    if (!(length > 0.0f) || !tg_finitef(length)) { return TG_ERR_INVALID_ARGUMENT; }
    if (g->organs.count >= g->max_organs) {
        TG_LOG_ERRORF(TG_SUB, "organ limit %u reached", g->max_organs);
        return TG_ERR_LIMIT_EXCEEDED;
    }

    axis = tree_graph_axis_mut(g, axis_id);

    /* The parent is the previous segment of this axis if there is one, otherwise
     * the organ the axis was inserted on. This is what makes the id ordering
     * invariant hold: an axis is always created after its parent organ. */
    parent_id = (axis->last_organ != TG_INVALID_ID) ? axis->last_organ
                                                    : axis->parent_organ;
    if (parent_id != TG_INVALID_ID && parent_id >= g->organs.count) {
        return TG_ERR_INVALID_STATE;
    }

    organ_init(&o, 0, parent_id, axis_id, type, axis->order, step);
    o.base = base;
    o.direction = v3_norm_or(direction, v3(0.0f, 1.0f, 0.0f));
    o.length = length;
    o.index_in_axis = (u16)tg_min_u32(axis->organ_count, 0xFFFFu);
    if (axis->kind == AXIS_ROOT) { o.flags |= 0u; }

    /* Frame propagation. The RMF double-reflection step is applied here, once,
     * at organ creation. Doing it here rather than during meshing means the frame
     * is part of the authoritative graph, so the mesh cannot introduce twist and
     * the educational view can show the true frames. */
    {
        Frame prev;
        bool have_prev = false;
        if (axis->last_organ != TG_INVALID_ID) {
            const Organ *p = tree_graph_organ(g, axis->last_organ);
            prev.origin = p->base;
            prev.t = p->direction;
            prev.n = p->frame_ref;
            prev.b = v3_cross(prev.t, prev.n);
            have_prev = true;
        } else if (axis->parent_organ != TG_INVALID_ID) {
            /* Seed from the parent organ so bark grain stays continuous across a
             * branch union instead of restarting at an arbitrary angle. */
            const Organ *p = tree_graph_organ(g, axis->parent_organ);
            prev.origin = p->base;
            prev.t = p->direction;
            prev.n = p->frame_ref;
            prev.b = v3_cross(prev.t, prev.n);
            have_prev = true;
        }
        if (have_prev) {
            Frame next = frame_propagate_rmf(prev, base, o.direction);
            o.frame_ref = next.n;
        } else {
            Frame f = frame_make(base, o.direction, v3(1.0f, 0.0f, 0.0f));
            o.frame_ref = f.n;
        }
    }

    r = tg_array_push(&g->organs, &o, &index);
    if (r != TG_OK) { return r; }
    {
        Organ *stored = &((Organ *)g->organs.data)[index];
        stored->id = (u32)index;
    }

    link_child(g, parent_id, (u32)index);

    /* Re-fetch: the organ array may have reallocated during the push above, so
     * the earlier `axis` pointer into a DIFFERENT array is still fine, but this
     * is the pattern the project mandates -- never hold a pointer across a push
     * into the same array. */
    axis = tree_graph_axis_mut(g, axis_id);
    if (axis->first_organ == TG_INVALID_ID) { axis->first_organ = (u32)index; }
    axis->last_organ = (u32)index;
    axis->organ_count++;
    axis->total_length += length;
    axis->tip_position = v3_add(base, v3_scale(o.direction, length));
    axis->tip_direction = o.direction;

    if (out_organ != NULL) { *out_organ = (u32)index; }
    return TG_OK;
}

TgResult tree_graph_add_attachment(TreeGraph *g, u32 parent_organ, OrganType type,
                                  f32 along, f32 local_angle, V3 direction,
                                  f32 length, u16 step, u32 *out_organ) {
    Organ o;
    u64 index;
    TgResult r;
    const Organ *parent;

    TG_CHECK(g != NULL);
    if (g->finalized) { return TG_ERR_INVALID_STATE; }
    if (parent_organ >= g->organs.count) { return TG_ERR_INVALID_ARGUMENT; }
    if (organ_type_is_segment(type)) { return TG_ERR_INVALID_ARGUMENT; }
    if (!v3_finite(direction) || !tg_finitef(length) || length < 0.0f) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (!tg_finitef(along) || !tg_finitef(local_angle)) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (g->organs.count >= g->max_organs) { return TG_ERR_LIMIT_EXCEEDED; }

    parent = tree_graph_organ(g, parent_organ);
    organ_init(&o, 0, parent_organ, parent->axis, type, parent->branch_order, step);
    /* Attachments live ON the parent's surface. `along` in [0,1] positions them
     * up the internode, and `local_angle` is the phyllotactic angle in the
     * parent's own frame -- which is why the frame had to be stored on the
     * parent rather than recomputed later. */
    {
        Frame pf;
        V3 radial;
        pf.origin = parent->base;
        pf.t = parent->direction;
        pf.n = parent->frame_ref;
        pf.b = v3_cross(pf.t, pf.n);
        o.base = v3_add(parent->base,
                        v3_scale(parent->direction,
                                 tg_saturatef(along) * parent->length));
        /* The outward radial direction at the attachment point. Used as the
         * frame HINT, not as the frame itself: the organ's own frame_ref must be
         * perpendicular to the organ's own direction, like every other organ. */
        radial = frame_ring_dir(pf, local_angle);
        o.direction = v3_norm_or(direction, radial);
        {
            Frame f = frame_make(o.base, o.direction, radial);
            o.frame_ref = f.n;
        }
        o.attach_along = tg_saturatef(along);
        o.attach_angle = local_angle;
    }
    o.length = length;
    o.radius_base = 0.0f;
    o.radius_tip = 0.0f;
    if (type == ORGAN_BUD_TERMINAL || type == ORGAN_BUD_AXILLARY) {
        o.flags |= ORGAN_FLAG_DORMANT;
    }

    r = tg_array_push(&g->organs, &o, &index);
    if (r != TG_OK) { return r; }
    ((Organ *)g->organs.data)[index].id = (u32)index;
    link_child(g, parent_organ, (u32)index);

    if (out_organ != NULL) { *out_organ = (u32)index; }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Mortality                                                                 */
/* ------------------------------------------------------------------------- */

u32 tree_graph_kill_subtree(TreeGraph *g, u32 organ, u16 step, u32 extra_flags) {
    u32 changed = 0;
    u32 i;
    u32 n;

    TG_CHECK(g != NULL);
    if (g->finalized || organ >= g->organs.count) { return 0; }

    /* Mark the root of the subtree, then sweep forward. Because a child's id is
     * always greater than its parent's, a single ascending pass propagates death
     * to the whole subtree without recursion or a work stack -- and cannot
     * overflow the stack on a deep axis. */
    {
        Organ *o = tree_graph_organ_mut(g, organ);
        if ((o->flags & ORGAN_FLAG_DEAD) == 0) {
            o->flags &= ~(u32)ORGAN_FLAG_ALIVE;
            o->flags |= ORGAN_FLAG_DEAD | extra_flags;
            o->death_step = step;
            changed++;
        }
    }

    n = (u32)g->organs.count;
    for (i = organ + 1u; i < n; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        const Organ *p;
        if (o->parent == TG_INVALID_ID) { continue; }
        p = tree_graph_organ(g, o->parent);
        if ((p->flags & ORGAN_FLAG_DEAD) == 0) { continue; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { continue; }
        o->flags &= ~(u32)ORGAN_FLAG_ALIVE;
        o->flags |= ORGAN_FLAG_DEAD | extra_flags;
        o->death_step = step;
        changed++;
    }
    return changed;
}

void tree_graph_accumulate_basipetal(TreeGraph *g,
                                     f32 (*local_leaf_area)(const Organ *, void *),
                                     void *user) {
    u32 i;

    TG_CHECK(g != NULL);
    if (g->finalized) { return; }

    /* Reset, then walk tip-to-base. Descending id order is a valid basipetal
     * order by the ordering invariant, so each organ's children have already
     * been visited when it is reached. */
    for (i = 0; i < (u32)g->organs.count; ++i) {
        Organ *o = tree_graph_organ_mut(g, i);
        o->supported_leaf_area = (local_leaf_area != NULL)
                                   ? local_leaf_area(o, user) : 0.0f;
    }
    for (i = (u32)g->organs.count; i-- > 0;) {
        const Organ *o = tree_graph_organ(g, i);
        f32 area = o->supported_leaf_area;
        if (o->parent == TG_INVALID_ID) { continue; }
        tree_graph_organ_mut(g, o->parent)->supported_leaf_area += area;
    }
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                */
/* ------------------------------------------------------------------------- */

static void record(GraphValidateReport *r, GraphIssueKind k, u32 example) {
    GraphIssue *slot = NULL;
    u32 i;

    r->passed = false;
    for (i = 0; i < r->issue_count; ++i) {
        if (r->issue[i].kind == k) { slot = &r->issue[i]; break; }
    }
    if (slot == NULL) {
        if (r->issue_count >= TG_COUNTOF(r->issue)) { return; }
        slot = &r->issue[r->issue_count++];
        memset(slot, 0, sizeof *slot);
        slot->kind = k;
    }
    slot->count++;
    if (slot->example_count < GRAPH_VALIDATE_MAX_EXAMPLES) {
        slot->example[slot->example_count++] = example;
    }
}

TgResult tree_graph_validate(const TreeGraph *g, f32 connection_tolerance,
                             GraphValidateReport *report) {
    u32 i, n, an;

    TG_CHECK(g != NULL);
    TG_CHECK(report != NULL);
    memset(report, 0, sizeof *report);
    report->passed = true;
    report->bounds = aabb_empty();
    report->min_root_depth = 0.0f;

    n = (u32)g->organs.count;
    an = (u32)g->axes.count;
    report->organs = n;
    report->axes = an;

    if (!(connection_tolerance > 0.0f)) {
        /* Scale-derived default: 1 mm, or a tenth of the smallest segment if
         * that is smaller. Absolute, because a connection gap is a physical
         * distance and the tree is always in metres. */
        connection_tolerance = 1.0e-3f;
    }

    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        OrganType type = (OrganType)o->type;
        bool is_seg = organ_type_is_segment(type);

        if (o->id != i) { record(report, GRAPH_ISSUE_CHILD_LIST_BROKEN, i); }

        /* --- parent and ordering ---------------------------------------- */
        if (o->parent != TG_INVALID_ID) {
            if (o->parent >= n) {
                record(report, GRAPH_ISSUE_BAD_PARENT_ID, i);
            } else {
                const Organ *p = tree_graph_organ(g, o->parent);
                /* THE ordering invariant. Everything basipetal/acropetal
                 * depends on it, so it is checked explicitly rather than
                 * assumed. */
                if (o->parent >= i) {
                    record(report, GRAPH_ISSUE_PARENT_NOT_BEFORE_CHILD, i);
                }
                if (o->branch_order < p->branch_order) {
                    record(report, GRAPH_ISSUE_ORDER_NOT_MONOTONE, i);
                }
                /* Radius invariant: a child may not be thicker than its parent
                 * unless a deformity is explicitly recorded.
                 *
                 * A lateral axis's first segment hangs off a BUD, not directly
                 * off the host segment, so the woody parent has to be found by
                 * stepping through the intervening organ. Checking only direct
                 * segment parents skipped every branch union -- precisely the
                 * places where a child thicker than its parent would be most
                 * visible. */
                const Organ *woody = p;
                if (!organ_type_is_segment((OrganType)woody->type) &&
                    woody->parent != TG_INVALID_ID && woody->parent < n) {
                    woody = tree_graph_organ(g, woody->parent);
                }
                if (is_seg && organ_type_is_segment((OrganType)woody->type) &&
                    o->radius_base > 0.0f && woody->radius_base > 0.0f &&
                    (o->flags & (ORGAN_FLAG_DAMAGED | ORGAN_FLAG_BROKEN)) == 0) {
                    if (o->radius_base > woody->radius_base * 1.02f) {
                        record(report, GRAPH_ISSUE_CHILD_THICKER_THAN_PARENT, i);
                    }
                }
                /* Continuity: a segment continuing an axis must start where its
                 * parent ended. A gap here is exactly what produces a visible
                 * crack at a junction. */
                if (is_seg && o->axis == p->axis &&
                    organ_type_is_segment((OrganType)p->type)) {
                    if (v3_dist(o->base, organ_tip(p)) > connection_tolerance) {
                        record(report, GRAPH_ISSUE_DISCONNECTED_SEGMENT, i);
                    }
                }
            }
        }

        if (o->axis >= an) { record(report, GRAPH_ISSUE_BAD_AXIS_ID, i); }

        /* --- numeric ----------------------------------------------------- */
        if (!v3_finite(o->base) || !v3_finite(o->direction) ||
            !v3_finite(o->frame_ref) || !tg_finitef(o->length) ||
            !tg_finitef(o->radius_base) || !tg_finitef(o->radius_tip) ||
            !tg_finitef(o->vigor) || !tg_finitef(o->light) ||
            !tg_finitef(o->supported_leaf_area) ||
            !tg_finitef(o->supported_mass) ||
            !tg_finitef(o->physiological_age) ||
            !tg_finitef(o->attach_along) || !tg_finitef(o->attach_angle)) {
            record(report, GRAPH_ISSUE_NON_FINITE, i);
            continue;
        }
        if (!tg_nearf(v3_len(o->direction), 1.0f, 1e-3f)) {
            record(report, GRAPH_ISSUE_NON_UNIT_DIRECTION, i);
        }
        /* frame_ref must be a unit vector perpendicular to the direction, or the
         * cross-sections built from it will be sheared. */
        if (!tg_nearf(v3_len(o->frame_ref), 1.0f, 1e-2f) ||
            tg_absf(v3_dot(o->frame_ref, o->direction)) > 1e-2f) {
            record(report, GRAPH_ISSUE_FRAME_NOT_ORTHOGONAL, i);
        }
        if (is_seg) {
            if (!(o->length > 0.0f)) {
                record(report, GRAPH_ISSUE_NON_POSITIVE_LENGTH, i);
            }
            /* Radii are zero until the mechanics pass runs, so only check them
             * once they have been assigned. */
            if (o->radius_base > 0.0f || o->radius_tip > 0.0f) {
                if (!(o->radius_base > 0.0f) || !(o->radius_tip > 0.0f)) {
                    record(report, GRAPH_ISSUE_NON_POSITIVE_RADIUS, i);
                } else if (o->radius_tip > o->radius_base * 1.001f) {
                    record(report, GRAPH_ISSUE_RADIUS_INCREASES_DISTALLY, i);
                }
            }
        }
        if (o->death_step != 0xFFFFu && o->death_step < o->created_step) {
            record(report, GRAPH_ISSUE_DEAD_BEFORE_BORN, i);
        }

        /* --- statistics --------------------------------------------------- */
        report->bounds = aabb_add_point(report->bounds, o->base);
        if (is_seg) {
            V3 tip = organ_tip(o);
            report->bounds = aabb_add_point(report->bounds, tip);
            report->segments++;
            report->total_axis_length += o->length;
            if (tip.y > report->max_height) { report->max_height = tip.y; }
            if (tip.y < report->min_root_depth) { report->min_root_depth = tip.y; }
        }
        if (type == ORGAN_BUD_TERMINAL || type == ORGAN_BUD_AXILLARY) {
            report->buds++;
        }
        if (type == ORGAN_LEAF || type == ORGAN_NEEDLE) { report->leaves++; }
        if ((o->flags & ORGAN_FLAG_DEAD) != 0) { report->dead_organs++; }
        if (o->branch_order > report->max_branch_order) {
            report->max_branch_order = o->branch_order;
        }
        report->order_histogram[tg_min_u32(o->branch_order,
                                          (u32)TG_COUNTOF(report->order_histogram) - 1u)]++;
    }

    /* --- child list consistency ----------------------------------------- */
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        u32 child = o->first_child;
        u32 guard = 0;
        u32 last_seen = TG_INVALID_ID;
        while (child != TG_INVALID_ID) {
            const Organ *c;
            if (child >= n || guard++ > n) {
                record(report, GRAPH_ISSUE_SIBLING_CYCLE, i);
                break;
            }
            c = tree_graph_organ(g, child);
            if (c->parent != i) {
                record(report, GRAPH_ISSUE_CHILD_LIST_BROKEN, child);
                break;
            }
            last_seen = child;
            child = c->next_sibling;
        }
        if (o->first_child != TG_INVALID_ID && last_seen != o->last_child) {
            record(report, GRAPH_ISSUE_CHILD_LIST_BROKEN, i);
        }
        if ((o->first_child == TG_INVALID_ID) != (o->last_child == TG_INVALID_ID)) {
            record(report, GRAPH_ISSUE_CHILD_LIST_BROKEN, i);
        }
    }

    /* --- axis structure -------------------------------------------------- */
    for (i = 0; i < an; ++i) {
        const Axis *a = tree_graph_axis(g, i);
        u32 counted = 0;
        u32 id;

        if (a->id != i) { record(report, GRAPH_ISSUE_BAD_AXIS_ID, i); }
        if (a->first_organ == TG_INVALID_ID) {
            if (a->organ_count != 0) {
                record(report, GRAPH_ISSUE_AXIS_COUNT_MISMATCH, i);
            }
            continue;
        }
        /* An axis's organs form an ORDERED CHAIN, not a contiguous id run.
         *
         * This distinction is load-bearing. Developmental growth extends many
         * axes in alternation across growth steps, so a single axis's organs are
         * necessarily interleaved with other axes' in id space. An earlier
         * version of this check demanded contiguity and rejected every genuinely
         * grown tree; the requirement was wrong, not the growth. Meshing
         * therefore walks the chain and uses index_in_axis as the authoritative
         * order, rather than assuming an id range.
         *
         * Walk from first_organ, at each step following the child that belongs to
         * the same axis. Guarded against cycles by the organ count. */
        id = a->first_organ;
        while (id != TG_INVALID_ID && id < n) {
            const Organ *o = tree_graph_organ(g, id);
            u32 child;
            u32 next = TG_INVALID_ID;
            if (o->axis != i || !organ_type_is_segment((OrganType)o->type) ||
                o->index_in_axis != counted) {
                record(report, GRAPH_ISSUE_AXIS_NOT_CONTIGUOUS, id);
                break;
            }
            counted++;
            if (counted > a->organ_count) {
                record(report, GRAPH_ISSUE_AXIS_COUNT_MISMATCH, i);
                break;
            }
            /* Only SEGMENT children continue an axis. Buds, leaves and scars
             * share their host's axis id (which is useful for queries) but are
             * not links in the chain, so the walk must skip them. */
            for (child = o->first_child; child != TG_INVALID_ID && child < n;) {
                const Organ *cc = tree_graph_organ(g, child);
                if (cc->axis == i && organ_type_is_segment((OrganType)cc->type)) {
                    next = child;
                    break;
                }
                child = cc->next_sibling;
            }
            if (next == TG_INVALID_ID) {
                if (id != a->last_organ) {
                    record(report, GRAPH_ISSUE_AXIS_NOT_CONTIGUOUS, id);
                }
                break;
            }
            id = next;
        }
        if (counted != a->organ_count) {
            record(report, GRAPH_ISSUE_AXIS_COUNT_MISMATCH, i);
        }
    }

    return report->passed ? TG_OK : TG_ERR_VALIDATION_FAILED;
}

void tree_graph_log_report(const GraphValidateReport *r) {
    u32 i;

    TG_CHECK(r != NULL);
    TG_LOG_INFOF(TG_SUB,
                 "graph %s: %u organs (%u segments, %u buds, %u leaves, %u dead) "
                 "in %u axes, max order %u",
                 r->passed ? "PASSED" : "FAILED", r->organs, r->segments, r->buds,
                 r->leaves, r->dead_organs, r->axes, r->max_branch_order);
    TG_LOG_INFOF(TG_SUB,
                 "  height %.3f m, deepest root %.3f m, total axis length %.2f m",
                 (double)r->max_height, (double)r->min_root_depth,
                 (double)r->total_axis_length);
    {
        char hist[128];
        int off = 0;
        u32 k;
        hist[0] = '\0';
        for (k = 0; k < TG_COUNTOF(r->order_histogram); ++k) {
            int wrote;
            if (r->order_histogram[k] == 0) { continue; }
            wrote = snprintf(hist + off, sizeof hist - (size_t)off, "%so%u:%u",
                             off == 0 ? "" : " ", k, r->order_histogram[k]);
            if (wrote <= 0 || (size_t)(off + wrote) >= sizeof hist) { break; }
            off += wrote;
        }
        TG_LOG_INFOF(TG_SUB, "  organs by branch order: %s", hist);
    }
    for (i = 0; i < r->issue_count; ++i) {
        const GraphIssue *is = &r->issue[i];
        char examples[128];
        int off = 0;
        u32 k;
        examples[0] = '\0';
        for (k = 0; k < is->example_count; ++k) {
            int wrote = snprintf(examples + off, sizeof examples - (size_t)off,
                                 "%s%u", k == 0 ? "" : ",", is->example[k]);
            if (wrote <= 0 || (size_t)(off + wrote) >= sizeof examples) { break; }
            off += wrote;
        }
        TG_LOG_ERRORF(TG_SUB, "  ISSUE %-28s count=%llu organs=[%s]",
                      graph_issue_name(is->kind),
                      (unsigned long long)is->count, examples);
    }
}

TgFingerprint tree_graph_fingerprint(const TreeGraph *g) {
    TgFingerprint f = tg_fp_begin(0x47524150u /* 'GRAP' */);
    u32 i;

    TG_CHECK(g != NULL);
    tg_fp_add_u64(&f, g->organs.count);
    tg_fp_add_u64(&f, g->axes.count);
    tg_fp_add_u32(&f, g->trunk_axis);
    tg_fp_add_u32(&f, g->root_axis_first);

    for (i = 0; i < (u32)g->organs.count; ++i) {
        const Organ *o = tree_graph_organ(g, i);
        tg_fp_add_u32(&f, o->parent);
        tg_fp_add_u32(&f, o->first_child);
        tg_fp_add_u32(&f, o->next_sibling);
        tg_fp_add_u32(&f, o->axis);
        tg_fp_add_u32(&f, ((u32)o->type << 24) | ((u32)o->branch_order << 16) |
                          (u32)o->index_in_axis);
        tg_fp_add_u32(&f, ((u32)o->created_step << 16) | (u32)o->death_step);
        tg_fp_add_u32(&f, o->flags);
        tg_fp_add_f32(&f, o->base.x);
        tg_fp_add_f32(&f, o->base.y);
        tg_fp_add_f32(&f, o->base.z);
        tg_fp_add_f32(&f, o->direction.x);
        tg_fp_add_f32(&f, o->direction.y);
        tg_fp_add_f32(&f, o->direction.z);
        tg_fp_add_f32(&f, o->frame_ref.x);
        tg_fp_add_f32(&f, o->frame_ref.y);
        tg_fp_add_f32(&f, o->frame_ref.z);
        tg_fp_add_f32(&f, o->length);
        tg_fp_add_f32(&f, o->radius_base);
        tg_fp_add_f32(&f, o->radius_tip);
        tg_fp_add_f32(&f, o->curvature);
        tg_fp_add_f32(&f, o->torsion);
        tg_fp_add_f32(&f, o->supported_leaf_area);
        tg_fp_add_f32(&f, o->supported_mass);
        tg_fp_add_f32(&f, o->physiological_age);
        tg_fp_add_f32(&f, o->attach_along);
        tg_fp_add_f32(&f, o->attach_angle);
    }
    for (i = 0; i < (u32)g->axes.count; ++i) {
        const Axis *a = tree_graph_axis(g, i);
        tg_fp_add_u32(&f, a->parent_axis);
        tg_fp_add_u32(&f, a->parent_organ);
        tg_fp_add_u32(&f, a->first_organ);
        tg_fp_add_u32(&f, a->organ_count);
        tg_fp_add_u32(&f, ((u32)a->kind << 16) | (u32)a->order);
        tg_fp_add_f32(&f, a->total_length);
        tg_fp_add_f32(&f, a->set_angle);
    }
    return f;
}
