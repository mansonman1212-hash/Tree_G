#include "tree_build.h"
#include "tree_bark.h"

#include "../core/hash.h"
#include "../core/log.h"
#include "../core/mem.h"

#include <math.h>
#include <string.h>

#define TB_SUB "tree_build"

const char *tree_build_stage_name(TreeBuildStage s) {
    switch (s) {
    case BUILD_STAGE_RESOLVE:      return "resolve";
    case BUILD_STAGE_GROW_SHOOTS:  return "grow shoots";
    case BUILD_STAGE_GROW_ROOTS:   return "grow roots";
    case BUILD_STAGE_MECHANICS:    return "mechanics";
    case BUILD_STAGE_SKIN:         return "skin";
    case BUILD_STAGE_FOLIAGE:      return "foliage";
    case BUILD_STAGE_FINALISE:     return "finalise";
    case BUILD_STAGE_VALIDATE:     return "validate";
    case BUILD_STAGE_ACCELERATE:   return "accelerate";
    case BUILD_STAGE_STAGES:       return "construction record";
    case BUILD_STAGE_COUNT:        break;
    }
    return "unknown";
}

TreeBuildOptions tree_build_default_options(void) {
    TreeBuildOptions o;
    memset(&o, 0, sizeof o);
    o.build_foliage = true;
    o.build_bvh = true;
    o.validate_mesh = true;
    /* Wood and its exposed faces only. Foliage is by far the largest section and
     * an inspection cursor that snaps to a leaf when the user is pointing at a
     * branch is worse than useless. A caller wanting to pick leaves builds a
     * second BVH with the leaf sections, which is cheaper than one BVH over
     * everything plus a filter on every ray. */
    o.bvh_section_mask = (1u << (u32)MESH_SECTION_WOOD)
                       | (1u << (u32)MESH_SECTION_EXPOSED_WOOD);
    o.validate_scratch_bytes = 0;
    return o;
}

static void report(const TreeBuildOptions *o, TreeBuildStage s, f32 f) {
    if (o->progress != NULL) { o->progress(s, tg_saturatef(f), o->progress_user); }
}

/* ------------------------------------------------------------------------- */
/* Construction record                                                       */
/*                                                                           */
/* Derived from the FINISHED GRAPH, not captured during simulation.            */
/*                                                                           */
/* Growth already records per-step statistics, and using those would have been    */
/* less code. It would also have been describing states that no longer exist: the  */
/* per-step record is a snapshot of transient simulation bookkeeping, whereas       */
/* every organ permanently records the step it was born in and the step it died in. */
/* Reconstructing the history from the organs means the replay narrates the tree     */
/* that is actually on screen -- the same geometry the birth-step clip reveals --     */
/* and it stays correct if the simulation's internal bookkeeping ever changes.        */
/* ------------------------------------------------------------------------- */
static TgResult build_stage_record(Tree *t, const TreeBuildOptions *opt) {
    u32 steps = t->growth.steps_run + 1u;
    u32 i, n;
    TgResult r = TG_OK;

    if (steps < 1u) { steps = 1u; }
    if (!tg_ckd_mul_u64(steps, sizeof(TreeConstructionStage), &t->stage_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    t->stage = (TreeConstructionStage *)tg_alloc_zero(t->stage_bytes);
    if (t->stage == NULL) { t->stage_bytes = 0; return TG_ERR_OUT_OF_MEMORY; }
    t->stage_count = steps;
    for (i = 0; i < steps; ++i) { t->stage[i].step = (u16)i; }

    n = tree_graph_organ_count(&t->graph);
    for (i = 0; i < n; ++i) {
        const Organ *o = tree_graph_organ(&t->graph, i);
        u32 born = o->created_step;
        V3 tip;
        f32 radial;
        if (born >= steps) { born = steps - 1u; }
        t->stage[born].organs++;
        if ((o->flags & ORGAN_FLAG_DEAD) == 0) { t->stage[born].living_organs++; }
        if (!organ_type_is_segment((OrganType)o->type)) { continue; }
        if (o->type == ORGAN_ROOT_SEGMENT) { continue; }
        tip = organ_tip(o);
        radial = v3_len(v3(tip.x, 0.0f, tip.z));
        if (tip.y > t->stage[born].height_m) { t->stage[born].height_m = tip.y; }
        if (radial > t->stage[born].crown_radius_m) {
            t->stage[born].crown_radius_m = radial;
        }
        /* Frustum volume of the tapered segment. Summed at the step the wood was
         * FORMED, which is not the same as the wood that segment ends up carrying:
         * a segment thickens for the rest of its life. The prefix sum below is
         * therefore the volume of wood whose PITH dates from that year or earlier,
         * which is what a growth-ring narrative wants. */
        {
            f32 a = o->radius_base, b = o->radius_tip;
            t->stage[born].wood_volume_m3 +=
                (TG_PI_F / 3.0f) * o->length * (a * a + a * b + b * b);
        }
    }
    for (i = 0; i < tree_graph_axis_count(&t->graph); ++i) {
        const Axis *a = tree_graph_axis(&t->graph, i);
        u32 born = a->created_step;
        if (born >= steps) { born = steps - 1u; }
        t->stage[born].axes++;
    }

    /* Prefix-sum into "what exists at the end of step i", and make the height and
     * radius monotone, because a tree does not get shorter as it grows: a step in
     * which nothing tall was born still has the previous height. */
    for (i = 1; i < steps; ++i) {
        t->stage[i].organs += t->stage[i - 1u].organs;
        t->stage[i].living_organs += t->stage[i - 1u].living_organs;
        t->stage[i].axes += t->stage[i - 1u].axes;
        t->stage[i].wood_volume_m3 += t->stage[i - 1u].wood_volume_m3;
        if (t->stage[i - 1u].height_m > t->stage[i].height_m) {
            t->stage[i].height_m = t->stage[i - 1u].height_m;
        }
        if (t->stage[i - 1u].crown_radius_m > t->stage[i].crown_radius_m) {
            t->stage[i].crown_radius_m = t->stage[i - 1u].crown_radius_m;
        }
    }
    report(opt, BUILD_STAGE_STAGES, 1.0f);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Fingerprint                                                               */
/* ------------------------------------------------------------------------- */
static u64 compute_fingerprint(const Tree *t) {
    u64 h = tree_settings_hash(&t->settings);
    TgFingerprint mf = mesh_fingerprint(&t->mesh);
    h = tg_hash64_u64(h, mf.value);
    h = tg_hash64_u64(h, mf.element_count);
    /* A non-finite coordinate anywhere makes the fingerprint meaningless, because
     * NaN compares unequal to itself: two identical runs would then disagree. It is
     * folded in explicitly so the comparison fails LOUDLY rather than
     * intermittently. */
    h = tg_hash64_u32(h, mf.saw_non_finite ? 0xDEADu : 0u);
    h = tg_hash64_u32(h, tree_graph_organ_count(&t->graph));
    h = tg_hash64_u32(h, tree_graph_axis_count(&t->graph));
    h = tg_hash64_u32(h, t->growth.steps_run);
    return h;
}

/* ------------------------------------------------------------------------- */

TgResult tree_build(const TreeSettings *settings, const TreeBuildOptions *options,
                    Tree *out) {
    TreeBuildOptions opt;
    TgResult r;

    TG_CHECK(settings != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    opt = (options != NULL) ? *options : tree_build_default_options();
    out->settings = *settings;
    out->failed_stage = BUILD_STAGE_COUNT;

    /* --- resolve ---------------------------------------------------------- */
    report(&opt, BUILD_STAGE_RESOLVE, 0.0f);
    r = tree_profile_resolve(&out->settings, &out->resolved);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_RESOLVE; return r; }
    report(&opt, BUILD_STAGE_RESOLVE, 1.0f);

    /* --- graph ------------------------------------------------------------ */
    r = tree_graph_init(&out->graph, 4096, out->resolved.max_organs);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_RESOLVE; return r; }

    /* --- shoots ----------------------------------------------------------- */
    /* Depends on: resolve. Growth reads the resolved individual's height curve,
     * crown envelope and quality budgets, and nothing else. */
    report(&opt, BUILD_STAGE_GROW_SHOOTS, 0.0f);
    r = tree_growth_run(&out->graph, &out->resolved, opt.cancel, &out->growth);
    if (r == TG_ERR_CANCELLED) { out->cancelled = true; }
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_GROW_SHOOTS; return r; }
    report(&opt, BUILD_STAGE_GROW_SHOOTS, 1.0f);

    /* --- roots ------------------------------------------------------------ */
    /* Depends on: shoots. Root scale is derived from the REALISED crown, not from
     * the profile, because a root system must relate to the structure it actually
     * supports. Reversing these two produces roots sized for a tree that did not
     * grow. */
    report(&opt, BUILD_STAGE_GROW_ROOTS, 0.0f);
    r = tree_growth_roots(&out->graph, &out->resolved, opt.cancel, &out->growth);
    if (r == TG_ERR_CANCELLED) { out->cancelled = true; }
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_GROW_ROOTS; return r; }
    report(&opt, BUILD_STAGE_GROW_ROOTS, 1.0f);

    /* --- mechanics -------------------------------------------------------- */
    /* Depends on: the complete skeleton. This pass assigns every radius from the
     * pipe model and bends every segment under load, and BOTH are inputs to
     * skinning. Skinning first would produce a complete, valid, silently wrong
     * mesh: an unbent tree of zero-radius tubes. Nothing downstream would
     * complain, which is exactly why the order is fixed here. */
    report(&opt, BUILD_STAGE_MECHANICS, 0.0f);
    r = tree_mechanics_run(&out->graph, &out->resolved, &out->mechanics);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_MECHANICS; return r; }
    report(&opt, BUILD_STAGE_MECHANICS, 1.0f);

    /* --- mesh ------------------------------------------------------------- */
    {
        /* Reserve from the organ count rather than growing from nothing. A mature
         * tree reaches sixteen million triangles, and reaching it by repeated
         * doubling from 64k copies the whole buffer eight times. */
        u64 seg = tree_graph_organ_count(&out->graph);
        u64 v_guess = seg * 16u + 4096u;
        u64 t_guess = seg * 26u + 4096u;
        r = mesh_init(&out->mesh, v_guess, t_guess);
        if (r != TG_OK) { out->failed_stage = BUILD_STAGE_SKIN; return r; }
    }

    report(&opt, BUILD_STAGE_SKIN, 0.0f);
    r = tree_skin_build(&out->mesh, &out->graph, &out->resolved, &out->skin);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_SKIN; return r; }
    report(&opt, BUILD_STAGE_SKIN, 1.0f);

    if (opt.build_foliage) {
        /* Depends on: mechanics, for the same reason as skinning -- foliage is
         * attached to the BENT skeleton, and leaves placed on the unbent one hang
         * in the air beside their twigs. */
        report(&opt, BUILD_STAGE_FOLIAGE, 0.0f);
        r = tree_foliage_build(&out->mesh, &out->graph, &out->resolved,
                               (u16)out->resolved.growth_steps, &out->foliage);
        if (r != TG_OK) { out->failed_stage = BUILD_STAGE_FOLIAGE; return r; }
        report(&opt, BUILD_STAGE_FOLIAGE, 1.0f);
    }

    report(&opt, BUILD_STAGE_FINALISE, 0.0f);
    r = mesh_finalize(&out->mesh);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_FINALISE; return r; }
    report(&opt, BUILD_STAGE_FINALISE, 1.0f);

    /* --- validation ------------------------------------------------------- */
    if (opt.validate_mesh) {
        MeshValidateOptions vo = mesh_validate_default_options();
        TgResult vr;
        if (opt.validate_scratch_bytes > 0u) {
            vo.max_scratch_bytes = opt.validate_scratch_bytes;
        }
        report(&opt, BUILD_STAGE_VALIDATE, 0.0f);
        vr = mesh_validate(&out->mesh, &vo, &out->validation);
        out->mesh_validated = !out->validation.topology_not_checked;
        out->mesh_valid = (vr == TG_OK) && out->mesh_validated;
        /* A validation failure is NOT a build failure. The caller is given the tree
         * and the report and decides: a research build wants to look at the broken
         * geometry, and refusing to hand it over is how a diagnostic tool becomes
         * useless at the moment it is needed. It is logged loudly instead. */
        if (vr != TG_OK && out->mesh_validated) {
            TG_LOG_ERRORF(TB_SUB, "generated mesh FAILED validation with %u "
                                  "distinct issues; the tree is returned anyway "
                                  "so it can be inspected",
                          out->validation.issue_count);
        }
        report(&opt, BUILD_STAGE_VALIDATE, 1.0f);
    }

    /* --- acceleration ----------------------------------------------------- */
    if (opt.build_bvh) {
        /* Depends on: finalise. mesh_bvh_build refuses a non-finalised mesh, so
         * this dependency is enforced by the callee rather than by this comment. */
        report(&opt, BUILD_STAGE_ACCELERATE, 0.0f);
        r = mesh_bvh_build(&out->bvh, &out->mesh, opt.bvh_section_mask);
        if (r != TG_OK) { out->failed_stage = BUILD_STAGE_ACCELERATE; return r; }
        report(&opt, BUILD_STAGE_ACCELERATE, 1.0f);
    }

    /* --- construction record and fingerprint ------------------------------ */
    report(&opt, BUILD_STAGE_STAGES, 0.0f);
    r = build_stage_record(out, &opt);
    if (r != TG_OK) { out->failed_stage = BUILD_STAGE_STAGES; return r; }

    out->fingerprint = compute_fingerprint(out);
    return TG_OK;
}

void tree_free(Tree *t) {
    if (t == NULL) { return; }
    mesh_bvh_destroy(&t->bvh);
    mesh_destroy(&t->mesh);
    tree_graph_destroy(&t->graph);
    if (t->stage != NULL) { tg_free(t->stage, t->stage_bytes); }
    /* Zeroed so a double free is a no-op rather than a crash. A build that failed
     * part way is freed by the same call as one that succeeded, and a caller
     * unwinding several error paths should not have to track which. */
    memset(t, 0, sizeof *t);
}

const TreeConstructionStage *tree_stage_at(const Tree *t, u32 step) {
    TG_CHECK(t != NULL);
    if (t->stage == NULL || t->stage_count == 0u) { return NULL; }
    if (step >= t->stage_count) { step = t->stage_count - 1u; }
    return &t->stage[step];
}
