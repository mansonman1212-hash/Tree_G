#include "test_util.h"
#include "test_suites.h"

#include "../src/tree/tree_graph.h"

#include <string.h>

/* Builds a small but structurally complete graph: a trunk of `trunk_len`
 * segments, one lateral axis off the middle, a bud, a leaf and a root. Enough to
 * exercise every invariant without obscuring what is being tested. */
typedef struct Built {
    u32 trunk_axis;
    u32 trunk_first;
    u32 trunk_mid;
    u32 lateral_axis;
    u32 lateral_first;
    u32 root_axis;
    u32 bud;
    u32 leaf;
} Built;

static Built build_small(TreeGraph *g, u32 trunk_len) {
    Built b;
    u32 i;
    V3 pos = v3_zero();
    memset(&b, 0, sizeof b);

    TG_EXPECT_OK(tree_graph_add_axis(g, TG_INVALID_ID, TG_INVALID_ID,
                                     AXIS_ORTHOTROPIC, 0, 0, 0.0f,
                                     v3_zero(), v3(0, 1, 0), &b.trunk_axis));
    for (i = 0; i < trunk_len; ++i) {
        u32 id;
        /* Slight lean so the RMF has something to propagate through. */
        V3 dir = v3_norm_or(v3(0.06f * (f32)i, 1.0f, 0.02f), v3(0, 1, 0));
        TG_EXPECT_OK(tree_graph_add_segment(g, b.trunk_axis, ORGAN_TRUNK_SEGMENT,
                                            pos, dir, 0.5f, (u16)i, &id));
        {
            Organ *o = tree_graph_organ_mut(g, id);
            /* Assign a monotone taper so the radius invariants are exercised. */
            o->radius_base = 0.20f - 0.012f * (f32)i;
            o->radius_tip = 0.20f - 0.012f * (f32)(i + 1u);
            pos = organ_tip(o);
        }
        if (i == 0) { b.trunk_first = id; }
        if (i == trunk_len / 2u) { b.trunk_mid = id; }
    }

    /* Lateral branch off the middle trunk segment. */
    {
        const Organ *host = tree_graph_organ(g, b.trunk_mid);
        V3 base = organ_tip(host);
        V3 dir = v3_norm_or(v3(1.0f, 0.5f, 0.0f), v3(1, 0, 0));
        u32 id;
        TG_EXPECT_OK(tree_graph_add_axis(g, b.trunk_axis, b.trunk_mid,
                                         AXIS_PLAGIOTROPIC, 1, 3,
                                         45.0f * TG_DEG2RAD_F, base, dir,
                                         &b.lateral_axis));
        TG_EXPECT_OK(tree_graph_add_segment(g, b.lateral_axis,
                                            ORGAN_BRANCH_SEGMENT, base, dir,
                                            0.30f, 3, &id));
        b.lateral_first = id;
        {
            Organ *o = tree_graph_organ_mut(g, id);
            o->radius_base = 0.045f;
            o->radius_tip = 0.038f;
        }
        /* A second segment continuing the lateral. */
        {
            const Organ *prev = tree_graph_organ(g, id);
            V3 b2 = organ_tip(prev);
            u32 id2;
            TG_EXPECT_OK(tree_graph_add_segment(g, b.lateral_axis,
                                                ORGAN_BRANCH_SEGMENT, b2,
                                                v3_norm_or(v3(1.0f, 0.2f, 0.1f),
                                                           v3(1, 0, 0)),
                                                0.25f, 4, &id2));
            {
                Organ *o = tree_graph_organ_mut(g, id2);
                o->radius_base = 0.038f;
                o->radius_tip = 0.030f;
            }
        }
    }

    /* A bud and a leaf attached to the lateral. */
    TG_EXPECT_OK(tree_graph_add_attachment(g, b.lateral_first, ORGAN_BUD_AXILLARY,
                                           0.5f, TG_GOLDEN_ANGLE_F,
                                           v3_norm_or(v3(1, 1, 0), v3(0, 1, 0)),
                                           0.004f, 4, &b.bud));
    TG_EXPECT_OK(tree_graph_add_attachment(g, b.lateral_first, ORGAN_LEAF,
                                           0.8f, TG_GOLDEN_ANGLE_F * 2.0f,
                                           v3_norm_or(v3(1, 0.5f, 0.5f),
                                                      v3(0, 1, 0)),
                                           0.10f, 4, &b.leaf));

    /* A root descending from the trunk base. */
    {
        V3 dir = v3_norm_or(v3(0.7f, -0.7f, 0.1f), v3(0, -1, 0));
        u32 id;
        TG_EXPECT_OK(tree_graph_add_axis(g, b.trunk_axis, b.trunk_first,
                                         AXIS_ROOT, 0, 0, TG_PI_F, v3_zero(), dir,
                                         &b.root_axis));
        TG_EXPECT_OK(tree_graph_add_segment(g, b.root_axis, ORGAN_ROOT_SEGMENT,
                                            v3_zero(), dir, 0.6f, 0, &id));
        {
            Organ *o = tree_graph_organ_mut(g, id);
            o->radius_base = 0.09f;
            o->radius_tip = 0.07f;
        }
    }
    return b;
}

static u64 issue_count_of(const GraphValidateReport *r, GraphIssueKind k) {
    u32 i;
    for (i = 0; i < r->issue_count; ++i) {
        if (r->issue[i].kind == k) { return r->issue[i].count; }
    }
    return 0;
}

static void test_type_predicates(void) {
    u32 i;

    TG_T_CASE("every organ type and axis kind has a name");
    for (i = 0; i < ORGAN_TYPE_COUNT; ++i) {
        TG_EXPECT(strcmp(organ_type_name((OrganType)i), "invalid") != 0);
    }
    for (i = 0; i < AXIS_KIND_COUNT; ++i) {
        TG_EXPECT(strcmp(axis_kind_name((AxisKind)i), "invalid") != 0);
    }
    for (i = 0; i < GRAPH_ISSUE_KIND_COUNT; ++i) {
        TG_EXPECT(strcmp(graph_issue_name((GraphIssueKind)i), "unknown") != 0);
    }

    TG_T_CASE("segment and woody predicates classify correctly");
    TG_EXPECT(organ_type_is_segment(ORGAN_TRUNK_SEGMENT));
    TG_EXPECT(organ_type_is_segment(ORGAN_ROOT_SEGMENT));
    TG_EXPECT(!organ_type_is_segment(ORGAN_LEAF));
    TG_EXPECT(!organ_type_is_segment(ORGAN_BUD_TERMINAL));
    TG_EXPECT(organ_type_is_woody(ORGAN_KNOT));
    TG_EXPECT(organ_type_is_woody(ORGAN_TRUNK_SEGMENT));
    TG_EXPECT(!organ_type_is_woody(ORGAN_LEAF));
    TG_EXPECT(!organ_type_is_woody(ORGAN_NEEDLE));
}

static void test_valid_graph_passes(void) {
    TreeGraph g;
    GraphValidateReport rep;
    Built b;

    TG_T_CASE("a correctly constructed graph validates cleanly");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    b = build_small(&g, 7);
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT(rep.passed);
    TG_EXPECT_EQ_U64(rep.issue_count, 0);

    TG_T_CASE("the report counts organs, axes and orders truthfully");
    TG_EXPECT_EQ_U64(rep.organs, tree_graph_organ_count(&g));
    TG_EXPECT_EQ_U64(rep.axes, 3);
    TG_EXPECT_EQ_U64(rep.segments, 7 + 2 + 1);
    TG_EXPECT_EQ_U64(rep.buds, 1);
    TG_EXPECT_EQ_U64(rep.leaves, 1);
    TG_EXPECT_EQ_U64(rep.max_branch_order, 1);
    TG_EXPECT(rep.max_height > 3.0f);
    TG_EXPECT(rep.min_root_depth < 0.0f);
    TG_EXPECT(rep.total_axis_length > 0.0f);

    TG_T_CASE("the trunk axis is identified and root axes are recorded");
    TG_EXPECT_EQ_U64(g.trunk_axis, b.trunk_axis);
    TG_EXPECT_EQ_U64(g.root_axis_first, b.root_axis);

    tree_graph_destroy(&g);
}

static void test_ordering_invariant(void) {
    TreeGraph g;
    u32 i;

    /* The invariant on which every basipetal and acropetal pass depends. */
    TG_T_CASE("every child has a strictly greater id than its parent");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 9);
    for (i = 0; i < tree_graph_organ_count(&g); ++i) {
        const Organ *o = tree_graph_organ(&g, i);
        if (o->parent == TG_INVALID_ID) { continue; }
        TG_EXPECT_MSG(o->parent < o->id, "organ %u has parent %u", o->id, o->parent);
    }

    TG_T_CASE("ascending id order visits every parent before its children");
    /* Restated as the property the resource passes actually rely on. */
    {
        bool *seen = (bool *)tg_alloc_zero(tree_graph_organ_count(&g));
        TG_EXPECT(seen != NULL);
        if (seen != NULL) {
            for (i = 0; i < tree_graph_organ_count(&g); ++i) {
                const Organ *o = tree_graph_organ(&g, i);
                if (o->parent != TG_INVALID_ID) {
                    TG_EXPECT_MSG(seen[o->parent],
                                  "organ %u visited before its parent %u",
                                  i, o->parent);
                }
                seen[i] = true;
            }
            tg_free(seen, tree_graph_organ_count(&g));
        }
    }
    tree_graph_destroy(&g);
}

static void test_frame_propagation(void) {
    TreeGraph g;
    u32 i;
    u32 axis;
    V3 pos = v3_zero();

    TG_T_CASE("frames stay orthogonal to the direction along a curving axis");
    /* If this drifts, cross-sections shear and bark grain spirals. */
    TG_EXPECT_OK(tree_graph_init(&g, 400, 10000));
    TG_EXPECT_OK(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                     AXIS_ORTHOTROPIC, 0, 0, 0.0f, v3_zero(),
                                     v3(0, 1, 0), &axis));
    for (i = 0; i < 300; ++i) {
        f32 t = (f32)i * 0.05f;
        /* A strongly curving, twisting path: the case a Frenet frame fails on. */
        V3 dir = v3_norm_or(v3(sinf(t) * 0.5f, 1.0f, cosf(t * 1.3f) * 0.5f),
                            v3(0, 1, 0));
        u32 id;
        TG_EXPECT_OK(tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, pos,
                                            dir, 0.1f, (u16)i, &id));
        {
            const Organ *o = tree_graph_organ(&g, id);
            TG_EXPECT_MSG(tg_nearf(v3_len(o->frame_ref), 1.0f, 1e-3f),
                          "frame_ref not unit at segment %u: %.6f", i,
                          (double)v3_len(o->frame_ref));
            TG_EXPECT_MSG(tg_absf(v3_dot(o->frame_ref, o->direction)) < 1e-3f,
                          "frame_ref not perpendicular at segment %u: %.6f", i,
                          (double)v3_dot(o->frame_ref, o->direction));
            pos = organ_tip(o);
        }
    }

    TG_T_CASE("attachments carry a frame perpendicular to THEIR OWN direction");
    /* frame_ref must mean exactly one thing for every organ type. It previously
     * held the parent-surface radial direction for attachments, which is not
     * perpendicular to the attachment's own axis, and the graph validator caught
     * it. The placement data now lives in attach_along / attach_angle. */
    {
        u32 host = 50;
        u32 att;
        TG_EXPECT_OK(tree_graph_add_attachment(&g, host, ORGAN_LEAF, 0.4f, 1.234f,
                                               v3_norm_or(v3(1, 1, 1), v3(0, 1, 0)),
                                               0.09f, 2, &att));
        {
            const Organ *o = tree_graph_organ(&g, att);
            TG_EXPECT_NEAR(v3_len(o->frame_ref), 1.0f, 1e-4);
            TG_EXPECT_NEAR(v3_dot(o->frame_ref, o->direction), 0.0f, 1e-4);
            TG_EXPECT_NEAR(o->attach_along, 0.4f, 1e-6);
            TG_EXPECT_NEAR(o->attach_angle, 1.234f, 1e-6);
            /* Placement must be on the parent internode, not at its base. */
            {
                const Organ *p = tree_graph_organ(&g, host);
                f32 d = v3_dist(o->base, p->base);
                TG_EXPECT_NEAR(d, 0.4f * p->length, 1e-5);
            }
        }
    }

    TG_T_CASE("frame continuity is preserved across a branch union");
    /* A lateral axis seeds its frame from the host organ, so bark grain does not
     * restart at an arbitrary angle at every junction. */
    {
        u32 host = 100;
        const Organ *h = tree_graph_organ(&g, host);
        V3 base = organ_tip(h);
        V3 dir = v3_norm_or(v3_add(h->direction, v3(1.0f, 0.0f, 0.0f)), v3(1, 0, 0));
        u32 lat, id;
        V3 host_frame = h->frame_ref;
        TG_EXPECT_OK(tree_graph_add_axis(&g, 0, host, AXIS_PLAGIOTROPIC, 1, 1,
                                         0.8f, base, dir, &lat));
        TG_EXPECT_OK(tree_graph_add_segment(&g, lat, ORGAN_BRANCH_SEGMENT, base,
                                            dir, 0.2f, 1, &id));
        {
            const Organ *c = tree_graph_organ(&g, id);
            /* Not identical (the tangent changed) but strongly correlated: an
             * arbitrary restart would give a near-random dot product. */
            TG_EXPECT_MSG(v3_dot(c->frame_ref, host_frame) > 0.5f,
                          "child frame is unrelated to the host frame: dot %.4f",
                          (double)v3_dot(c->frame_ref, host_frame));
        }
    }
    tree_graph_destroy(&g);
}

static void test_detects_corruption(void) {
    TreeGraph g;
    GraphValidateReport rep;
    Built b;

    tg_test_logs_mute();

    TG_T_CASE("a broken ordering invariant is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    b = build_small(&g, 6);
    tree_graph_organ_mut(&g, 2)->parent = 5; /* parent after child */
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_PARENT_NOT_BEFORE_CHILD) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("an out-of-range parent is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 3)->parent = 9999;
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_BAD_PARENT_ID) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a disconnected segment is detected: this is a junction crack");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 3)->base =
        v3_add(tree_graph_organ(&g, 3)->base, v3(0.05f, 0, 0));
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_DISCONNECTED_SEGMENT) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a sub-tolerance gap is accepted: float noise is not a crack");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 3)->base =
        v3_add(tree_graph_organ(&g, 3)->base, v3(1e-5f, 0, 0));
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    tree_graph_destroy(&g);

    TG_T_CASE("a radius that grows distally is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 2)->radius_tip =
        tree_graph_organ(&g, 2)->radius_base * 2.0f;
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_RADIUS_INCREASES_DISTALLY) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a child thicker than its parent is detected");
    /* The directive's explicit rule: no parent may be thinner than its child
     * without a recorded deformity. */
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    b = build_small(&g, 6);
    tree_graph_organ_mut(&g, b.lateral_first)->radius_base = 0.9f;
    tree_graph_organ_mut(&g, b.lateral_first)->radius_tip = 0.8f;
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_CHILD_THICKER_THAN_PARENT) >= 1);

    TG_T_CASE("the same thickening is ACCEPTED when a deformity is recorded");
    /* A graft, break or burl legitimately produces this, so the rule must have
     * an explicit escape rather than being unconditional. */
    tree_graph_organ_mut(&g, b.lateral_first)->flags |= ORGAN_FLAG_DAMAGED;
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    tree_graph_destroy(&g);

    TG_T_CASE("a non-unit direction is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 2)->direction = v3(0, 3, 0);
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_NON_UNIT_DIRECTION) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a sheared frame is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 2)->frame_ref = tree_graph_organ(&g, 2)->direction;
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_FRAME_NOT_ORTHOGONAL) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a NaN anywhere in an organ is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    {
        volatile f32 zero = 0.0f;
        tree_graph_organ_mut(&g, 4)->base.y = 0.0f / zero;
    }
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_NON_FINITE) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a broken child list is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 0)->first_child = 5; /* 5's parent is not 0 */
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_CHILD_LIST_BROKEN) >= 1);
    tree_graph_destroy(&g);

    TG_T_CASE("a sibling cycle is detected without looping forever");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    {
        /* Make organ 1 its own sibling. A naive walker would never terminate. */
        Organ *o = tree_graph_organ_mut(&g, 1);
        o->next_sibling = 1;
    }
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    tree_graph_destroy(&g);

    TG_T_CASE("death before birth is detected");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 6);
    tree_graph_organ_mut(&g, 5)->death_step = 0;
    tree_graph_organ_mut(&g, 5)->created_step = 3;
    TG_EXPECT_ERR(tree_graph_validate(&g, 0.0f, &rep), TG_ERR_VALIDATION_FAILED);
    TG_EXPECT(issue_count_of(&rep, GRAPH_ISSUE_DEAD_BEFORE_BORN) >= 1);
    tree_graph_log_report(&rep);
    tree_graph_destroy(&g);

    tg_test_logs_unmute();
}

static void test_rejects_bad_construction(void) {
    TreeGraph g;
    u32 axis, id;
    volatile f32 zero = 0.0f;

    tg_test_logs_mute();
    TG_T_CASE("construction rejects invalid arguments instead of storing them");
    TG_EXPECT_OK(tree_graph_init(&g, 16, 100));
    TG_EXPECT_ERR(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                      AXIS_KIND_COUNT, 0, 0, 0.0f, v3_zero(),
                                      v3(0, 1, 0), &axis),
                  TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_ERR(tree_graph_add_axis(&g, 99, TG_INVALID_ID, AXIS_ORTHOTROPIC, 0,
                                      0, 0.0f, v3_zero(), v3(0, 1, 0), &axis),
                  TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_ERR(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                      AXIS_ORTHOTROPIC, 0, 0, 0.0f,
                                      v3(1.0f / zero, 0, 0), v3(0, 1, 0), &axis),
                  TG_ERR_INVALID_ARGUMENT);

    TG_EXPECT_OK(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                     AXIS_ORTHOTROPIC, 0, 0, 0.0f, v3_zero(),
                                     v3(0, 1, 0), &axis));
    TG_T_CASE("a zero or negative length segment is refused");
    TG_EXPECT_ERR(tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, v3_zero(),
                                         v3(0, 1, 0), 0.0f, 0, &id),
                  TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_ERR(tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, v3_zero(),
                                         v3(0, 1, 0), -1.0f, 0, &id),
                  TG_ERR_INVALID_ARGUMENT);
    TG_T_CASE("a non-segment type is refused by add_segment and vice versa");
    TG_EXPECT_ERR(tree_graph_add_segment(&g, axis, ORGAN_LEAF, v3_zero(),
                                         v3(0, 1, 0), 1.0f, 0, &id),
                  TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_OK(tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, v3_zero(),
                                        v3(0, 1, 0), 1.0f, 0, &id));
    TG_EXPECT_ERR(tree_graph_add_attachment(&g, id, ORGAN_TRUNK_SEGMENT, 0.5f,
                                            0.0f, v3(1, 0, 0), 0.1f, 0, NULL),
                  TG_ERR_INVALID_ARGUMENT);
    TG_EXPECT_ERR(tree_graph_add_attachment(&g, 9999, ORGAN_LEAF, 0.5f, 0.0f,
                                            v3(1, 0, 0), 0.1f, 0, NULL),
                  TG_ERR_INVALID_ARGUMENT);

    TG_T_CASE("the organ ceiling is reported, not exceeded");
    {
        u32 i;
        TgResult r = TG_OK;
        V3 pos = v3(0, 1, 0);
        for (i = 0; i < 200 && r == TG_OK; ++i) {
            r = tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, pos,
                                       v3(0, 1, 0), 0.1f, 0, &id);
            if (r == TG_OK) { pos = organ_tip(tree_graph_organ(&g, id)); }
        }
        TG_EXPECT_ERR(r, TG_ERR_LIMIT_EXCEEDED);
        TG_EXPECT_EQ_U64(tree_graph_organ_count(&g), 100);
    }
    tree_graph_destroy(&g);
    tg_test_logs_unmute();
}

static void test_finalize_immutability(void) {
    TreeGraph g;
    u32 id;

    TG_T_CASE("a finalized graph refuses every mutation");
    /* Half of the "tree never changes during inspection" guarantee. */
    TG_EXPECT_OK(tree_graph_init(&g, 16, 1000));
    (void)build_small(&g, 4);
    tree_graph_finalize(&g);
    TG_EXPECT(g.finalized);
    TG_EXPECT_ERR(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                      AXIS_ORTHOTROPIC, 0, 0, 0.0f, v3_zero(),
                                      v3(0, 1, 0), NULL),
                  TG_ERR_INVALID_STATE);
    TG_EXPECT_ERR(tree_graph_add_segment(&g, 0, ORGAN_TRUNK_SEGMENT, v3_zero(),
                                         v3(0, 1, 0), 1.0f, 0, &id),
                  TG_ERR_INVALID_STATE);
    TG_EXPECT_ERR(tree_graph_add_attachment(&g, 0, ORGAN_LEAF, 0.5f, 0.0f,
                                            v3(1, 0, 0), 0.1f, 0, NULL),
                  TG_ERR_INVALID_STATE);
    TG_EXPECT_EQ_U64(tree_graph_kill_subtree(&g, 0, 5, 0), 0);

    TG_T_CASE("reads still work after finalization");
    TG_EXPECT(tree_graph_organ(&g, 0) != NULL);
    TG_EXPECT(tree_graph_organ(&g, 99999) == NULL);
    TG_EXPECT(tree_graph_axis(&g, 0) != NULL);
    TG_EXPECT(tree_graph_axis(&g, 99999) == NULL);
    tree_graph_destroy(&g);
}

static void test_kill_subtree(void) {
    TreeGraph g;
    Built b;
    u32 killed;
    GraphValidateReport rep;

    TG_T_CASE("killing an organ kills everything distal to it");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    b = build_small(&g, 7);
    killed = tree_graph_kill_subtree(&g, b.lateral_first, 12,
                                     ORGAN_FLAG_BARK_RETAINED);
    /* The lateral's two segments plus the bud and the leaf on it. */
    TG_EXPECT_EQ_U64(killed, 4);
    {
        const Organ *o = tree_graph_organ(&g, b.lateral_first);
        TG_EXPECT((o->flags & ORGAN_FLAG_DEAD) != 0);
        TG_EXPECT((o->flags & ORGAN_FLAG_ALIVE) == 0);
        TG_EXPECT((o->flags & ORGAN_FLAG_BARK_RETAINED) != 0);
        TG_EXPECT_EQ_U64(o->death_step, 12);
        TG_EXPECT((tree_graph_organ(&g, b.leaf)->flags & ORGAN_FLAG_DEAD) != 0);
        TG_EXPECT((tree_graph_organ(&g, b.bud)->flags & ORGAN_FLAG_DEAD) != 0);
    }

    TG_T_CASE("the trunk above the killed branch stays alive");
    /* Killing must be strictly distal: a shed branch does not kill the trunk. */
    {
        const Organ *trunk_top = tree_graph_organ(&g, b.trunk_mid + 1u);
        TG_EXPECT((trunk_top->flags & ORGAN_FLAG_DEAD) == 0);
        TG_EXPECT((tree_graph_organ(&g, b.trunk_first)->flags & ORGAN_FLAG_DEAD)
                  == 0);
        TG_EXPECT((tree_graph_organ(&g, b.root_axis == TG_INVALID_ID ? 0
                                      : tree_graph_axis(&g, b.root_axis)->first_organ)
                   ->flags & ORGAN_FLAG_DEAD) == 0);
    }

    TG_T_CASE("killing twice changes nothing further");
    TG_EXPECT_EQ_U64(tree_graph_kill_subtree(&g, b.lateral_first, 13, 0), 0);
    TG_EXPECT_EQ_U64(tree_graph_organ(&g, b.lateral_first)->death_step, 12);

    TG_T_CASE("the graph still validates after mortality");
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT_EQ_U64(rep.dead_organs, 4);

    TG_T_CASE("out-of-range kill is a no-op");
    TG_EXPECT_EQ_U64(tree_graph_kill_subtree(&g, 99999, 14, 0), 0);
    tree_graph_destroy(&g);
}

static f32 leaf_area_one(const Organ *o, void *user) {
    TG_UNUSED(user);
    return (o->type == ORGAN_LEAF || o->type == ORGAN_NEEDLE) ? 1.0f : 0.0f;
}

static void test_basipetal_accumulation(void) {
    TreeGraph g;
    Built b;
    u32 axis, i;
    V3 pos;

    TG_T_CASE("basipetal accumulation sums the whole distal subtree in one pass");
    TG_EXPECT_OK(tree_graph_init(&g, 128, 10000));
    b = build_small(&g, 5);

    /* Add three more leaves on the lateral so the sums are non-trivial. */
    for (i = 0; i < 3; ++i) {
        TG_EXPECT_OK(tree_graph_add_attachment(&g, b.lateral_first, ORGAN_LEAF,
                                               0.2f + 0.2f * (f32)i,
                                               TG_GOLDEN_ANGLE_F * (f32)i,
                                               v3(1, 0, 0), 0.08f, 5, NULL));
    }

    tree_graph_accumulate_basipetal(&g, leaf_area_one, NULL);

    /* 4 leaves in total, all on the lateral branch. */
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.lateral_first)->supported_leaf_area,
                   4.0f, 1e-5);
    /* The host trunk segment must see them all. */
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.trunk_mid)->supported_leaf_area,
                   4.0f, 1e-5);
    /* And so must the trunk base, since everything is distal to it. */
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.trunk_first)->supported_leaf_area,
                   4.0f, 1e-5);
    /* A trunk segment ABOVE the branch supports none of them. */
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.trunk_mid + 1u)->supported_leaf_area,
                   0.0f, 1e-5);
    /* A leaf supports only itself. */
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.leaf)->supported_leaf_area, 1.0f, 1e-5);

    TG_T_CASE("accumulation is idempotent: running it twice gives the same result");
    tree_graph_accumulate_basipetal(&g, leaf_area_one, NULL);
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.trunk_first)->supported_leaf_area,
                   4.0f, 1e-5);

    TG_T_CASE("a NULL contribution function zeroes everything safely");
    tree_graph_accumulate_basipetal(&g, NULL, NULL);
    TG_EXPECT_NEAR(tree_graph_organ(&g, b.trunk_first)->supported_leaf_area,
                   0.0f, 0.0);
    tree_graph_destroy(&g);

    TG_T_CASE("accumulation handles a long single chain without recursion limits");
    /* A recursive implementation would risk a stack overflow here. */
    TG_EXPECT_OK(tree_graph_init(&g, 40000, 100000));
    TG_EXPECT_OK(tree_graph_add_axis(&g, TG_INVALID_ID, TG_INVALID_ID,
                                     AXIS_ORTHOTROPIC, 0, 0, 0.0f, v3_zero(),
                                     v3(0, 1, 0), &axis));
    pos = v3_zero();
    for (i = 0; i < 30000; ++i) {
        u32 id;
        TG_EXPECT_OK(tree_graph_add_segment(&g, axis, ORGAN_TRUNK_SEGMENT, pos,
                                            v3(0, 1, 0), 0.001f, 0, &id));
        pos = organ_tip(tree_graph_organ(&g, id));
    }
    TG_EXPECT_OK(tree_graph_add_attachment(&g, 29999, ORGAN_LEAF, 1.0f, 0.0f,
                                           v3(1, 0, 0), 0.01f, 0, NULL));
    tree_graph_accumulate_basipetal(&g, leaf_area_one, NULL);
    TG_EXPECT_NEAR(tree_graph_organ(&g, 0)->supported_leaf_area, 1.0f, 1e-5);
    TG_EXPECT_EQ_U64(tree_graph_kill_subtree(&g, 0, 1, 0), 30001);
    tree_graph_destroy(&g);
}

static void test_fingerprint(void) {
    TreeGraph a, b;
    TgFingerprint fa, fb;

    TG_T_CASE("identical construction gives an identical fingerprint");
    TG_EXPECT_OK(tree_graph_init(&a, 16, 10000));
    (void)build_small(&a, 6);
    TG_EXPECT_OK(tree_graph_init(&b, 512, 10000)); /* different reservation */
    (void)build_small(&b, 6);
    fa = tree_graph_fingerprint(&a);
    fb = tree_graph_fingerprint(&b);
    TG_EXPECT_MSG(tg_fp_equal(fa, fb),
                  "fingerprint depends on allocation history");
    TG_EXPECT(!fa.saw_non_finite);

    TG_T_CASE("a change in topology, geometry or state changes the fingerprint");
    {
        TgFingerprint f2;
        tree_graph_organ_mut(&b, 3)->length += 1e-4f;
        f2 = tree_graph_fingerprint(&b);
        TG_EXPECT(!tg_fp_equal(fa, f2));
    }
    {
        TgFingerprint f3;
        TreeGraph c;
        TG_EXPECT_OK(tree_graph_init(&c, 16, 10000));
        (void)build_small(&c, 6);
        (void)tree_graph_kill_subtree(&c, 4, 9, 0);
        f3 = tree_graph_fingerprint(&c);
        TG_EXPECT_MSG(!tg_fp_equal(fa, f3),
                      "mortality did not change the fingerprint");
        tree_graph_destroy(&c);
    }
    {
        TgFingerprint f4;
        TreeGraph d;
        TG_EXPECT_OK(tree_graph_init(&d, 16, 10000));
        (void)build_small(&d, 7); /* one more trunk segment */
        f4 = tree_graph_fingerprint(&d);
        TG_EXPECT(!tg_fp_equal(fa, f4));
        tree_graph_destroy(&d);
    }
    tree_graph_destroy(&a);
    tree_graph_destroy(&b);
}

static void test_reset(void) {
    TreeGraph g;

    TG_T_CASE("reset clears content, keeps capacity, and allows a rebuild");
    TG_EXPECT_OK(tree_graph_init(&g, 64, 10000));
    (void)build_small(&g, 5);
    TG_EXPECT(tree_graph_organ_count(&g) > 0);
    tree_graph_reset(&g);
    TG_EXPECT_EQ_U64(tree_graph_organ_count(&g), 0);
    TG_EXPECT_EQ_U64(tree_graph_axis_count(&g), 0);
    TG_EXPECT_EQ_U64(g.trunk_axis, TG_INVALID_ID);
    TG_EXPECT(!g.finalized);
    TG_EXPECT(g.organs.capacity >= 64);
    (void)build_small(&g, 5);
    TG_EXPECT(tree_graph_organ_count(&g) > 0);
    tree_graph_destroy(&g);
}

static void test_empty_graph(void) {
    TreeGraph g;
    GraphValidateReport rep;

    TG_T_CASE("an empty graph validates: this is the startup state");
    TG_EXPECT_OK(tree_graph_init(&g, 0, 1000));
    TG_EXPECT_OK(tree_graph_validate(&g, 0.0f, &rep));
    TG_EXPECT(rep.passed);
    TG_EXPECT_EQ_U64(rep.organs, 0);
    TG_EXPECT_EQ_U64(rep.segments, 0);
    TG_EXPECT(aabb_is_empty(rep.bounds));
    {
        TgFingerprint f = tree_graph_fingerprint(&g);
        TG_EXPECT(!f.saw_non_finite);
    }
    tree_graph_destroy(&g);
}

void test_suite_tree_graph(void) {
    test_type_predicates();
    test_valid_graph_passes();
    test_ordering_invariant();
    test_frame_propagation();
    test_detects_corruption();
    test_rejects_bad_construction();
    test_finalize_immutability();
    test_kill_subtree();
    test_basipetal_accumulation();
    test_fingerprint();
    test_reset();
    test_empty_graph();
}
