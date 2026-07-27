#include "tree_skin.h"
#include "tree_bark.h"

#include "../core/log.h"
#include "../core/rng.h"

#include <string.h>

#define TS_SUB "tree_skin"

/* Hard ceiling on one ring. Guards the scratch buffers and keeps a single
 * pathological radius from consuming the whole vertex budget. */
#define TS_MAX_RING_SEGMENTS 256

u32 tree_skin_ring_segments(const TreeResolved *r, f32 radius) {
    f32 t;
    u32 lo, hi, n;

    TG_CHECK(r != NULL);
    /* The quality setting's minimum applies at TRUNK scale; the floor for a
     * two-millimetre twig is much lower, because at any inspection distance where
     * a twig is more than a couple of pixels wide, eight segments is already
     * sub-pixel. Spending the quality minimum on every twig instead multiplies the
     * vertex count several times over for detail nobody can resolve, and it is the
     * twigs that dominate the count. */
    lo = tg_clamp_u32(r->cross_section_segments_min / 2u, 8u,
                      TS_MAX_RING_SEGMENTS);
    hi = tg_clamp_u32(r->cross_section_segments_max, lo, TS_MAX_RING_SEGMENTS);

    /* Resolution scales with the LOGARITHM of radius, not linearly.
     *
     * A tree's radii span four orders of magnitude, from a 0.3 m trunk to a 2 mm
     * twig. Linear interpolation would give every twig the minimum and waste the
     * entire range on the trunk; what actually matters perceptually is the
     * angular size of a facet, which is what a log mapping keeps roughly
     * constant across scales. */
    {
        f32 r_min = tg_maxf(r->profile->tip_radius_m, 1e-5f);
        f32 r_max = tg_maxf(r->trunk_base_radius_m * 1.6f, r_min * 4.0f);
        f32 lg = logf(tg_clampf(radius, r_min, r_max) / r_min);
        f32 lg_max = logf(r_max / r_min);
        t = (lg_max > 1e-6f) ? tg_saturatef(lg / lg_max) : 0.0f;
    }
    n = lo + (u32)((f32)(hi - lo) * t + 0.5f);
    /* Even counts keep opposite sides of a section aligned, which makes the
     * eccentric and lobed profiles symmetric about their own axis. */
    if ((n & 1u) != 0u) { n++; }
    return tg_clamp_u32(n, lo, hi);
}

/* ------------------------------------------------------------------------- */
/* Cross-section shape                                                       */
/* ------------------------------------------------------------------------- */

typedef struct SectionShape {
    f32 radius;
    f32 lobe_amplitude;
    u32 lobe_count;
    f32 lobe_phase;
    f32 eccentricity;   /* 0..0.5 fraction of radius                         */
    f32 ecc_angle;      /* angle in the section plane the thickening faces    */
} SectionShape;

/* Radius of the section at angle theta.
 *
 * Deliberately NOT a circle plus noise. Each term is a named cause:
 *   - the lobe harmonic is low order and phase-locked per organ, so it reads as a
 *     grown form rather than as jitter, and it is what stops the trunk silhouette
 *     looking lathe-turned;
 *   - the eccentricity term is a single cosine, which produces a genuinely oval
 *     section with an off-centre pith -- the external signature of reaction wood.
 */
static f32 section_radius(const SectionShape *sh, f32 theta) {
    f32 r = sh->radius;
    if (sh->lobe_amplitude > 0.0f && sh->lobe_count > 0u) {
        r *= 1.0f + sh->lobe_amplitude
                    * cosf((f32)sh->lobe_count * theta + sh->lobe_phase);
    }
    if (sh->eccentricity > 0.0f) {
        r *= 1.0f + sh->eccentricity * cosf(theta - sh->ecc_angle);
    }
    return tg_maxf(r, 1e-6f);
}

/* Angle, within a frame's cross-section plane, that the reaction wood faces.
 *
 * The SIDE is derived here rather than stored on the organ, so there is exactly
 * one place that knows the rule: angiosperm tension wood thickens the UPPER side
 * of a leaning axis, gymnosperm compression wood the LOWER side. Getting this
 * backwards makes every branch section subtly and systematically wrong. */
static f32 reaction_angle(Frame f, ReactionWood kind) {
    /* Project world up into the section plane. For a vertical axis the projection
     * vanishes and the angle is arbitrary -- which is correct, because a vertical
     * axis has no upper or lower side and its eccentricity is zero anyway. */
    V3 up = v3(0.0f, 1.0f, 0.0f);
    V3 in_plane = v3_reject_unit(up, f.t);
    f32 angle;
    if (v3_len_sq(in_plane) < 1e-8f) { return 0.0f; }
    angle = frame_angle_of(f, in_plane);
    return (kind == REACTION_TENSION_UPPER) ? angle : angle + TG_PI_F;
}

/* ------------------------------------------------------------------------- */
/* Branch collars                                                            */
/* ------------------------------------------------------------------------- */

/* Applies the swelling produced by every lateral inserted on `host` to a ring at
 * normalised position `s` along it.
 *
 * A branch union is grown tissue, not two intersecting pipes: the parent swells
 * around the branch base and the swelling is DIRECTIONAL, strongest on the side
 * the branch leaves from. Both properties are required for the union to read as
 * grown, and both are produced here rather than by displacing bark later. */
static f32 collar_scale(const TreeGraph *g, const Organ *host,
                        const TreeResolved *res, Frame ring_frame, f32 s,
                        f32 theta, u32 *out_collars) {
    f32 scale = 1.0f;
    u32 child;

    for (child = host->first_child; child != TG_INVALID_ID;) {
        const Organ *bud = tree_graph_organ(g, child);
        u32 gc;
        child = bud->next_sibling;
        if (organ_type_is_segment((OrganType)bud->type)) { continue; }

        for (gc = bud->first_child; gc != TG_INVALID_ID;) {
            const Organ *lat = tree_graph_organ(g, gc);
            gc = lat->next_sibling;
            if (!organ_type_is_segment((OrganType)lat->type)) { continue; }
            if (lat->radius_base <= 0.0f) { continue; }

            {
                /* Longitudinal extent of the collar scales with the CHILD's
                 * radius: a big limb produces a long shoulder, a twig almost
                 * none. Using a fixed length would give every twig a collar as
                 * prominent as a scaffold branch's, which is one of the scale
                 * incoherences the directive calls out. */
                f32 reach = res->profile->collar_length_factor * lat->radius_base;
                f32 along_m = (s - bud->attach_along) * host->length;
                f32 falloff = 1.0f - tg_smoothstepf(0.0f, tg_maxf(reach, 1e-5f),
                                                    tg_absf(along_m));
                f32 dir_weight;
                f32 dtheta;
                if (falloff <= 0.0f) { continue; }

                /* Directional: peaks at the branch's azimuth and falls to nothing
                 * on the opposite side. The fourth power keeps it a local
                 * shoulder rather than a ring of swelling round the whole trunk. */
                dtheta = tg_wrap_pi(theta - bud->attach_angle);
                dir_weight = cosf(tg_clampf(dtheta, -TG_PI_F * 0.5f,
                                            TG_PI_F * 0.5f));
                if (tg_absf(dtheta) > TG_PI_F * 0.5f) { dir_weight = 0.0f; }
                dir_weight = dir_weight * dir_weight;
                dir_weight = dir_weight * dir_weight;

                /* Swelling is proportional to how big the child is relative to
                 * the parent, so a union between near-equal stems produces a
                 * pronounced shoulder and a twig on a trunk produces a slight
                 * one. */
                {
                    f32 ratio = tg_saturatef(lat->radius_base
                                             / tg_maxf(host->radius_base, 1e-6f));
                    scale += res->profile->collar_swell_factor * ratio
                           * falloff * dir_weight;
                }
                if (out_collars != NULL) { (*out_collars)++; }
            }
        }
    }
    TG_UNUSED(ring_frame);
    return scale;
}

/* ------------------------------------------------------------------------- */
/* Ring emission                                                             */
/* ------------------------------------------------------------------------- */

typedef struct SkinCtx {
    Mesh *mesh;
    const TreeGraph *graph;
    const TreeResolved *res;
    SkinResult *out;
    u32 *ring_a;
    u32 *ring_b;
    u32 *ring_dup;   /* duplicated rim for a hard-edged cap                  */
    u64 ring_bytes;
    /* Bark relief for the axis currently being swept. Held here rather than
     * recomputed per ring because the lattice density has to be constant for the
     * whole axis -- letting it follow the local radius would shear every ridge
     * along the taper. */
    BarkAxisField bark;
} SkinCtx;

static MeshMaterial material_for(const Organ *o, const TreeResolved *res) {
    if (o->type == ORGAN_ROOT_SEGMENT) { return MESH_MAT_ROOT; }
    if ((o->flags & ORGAN_FLAG_DEAD) != 0) { return MESH_MAT_DEAD_WOOD; }
    /* Bark maturity is a function of local radius and cambial age, never a random
     * draw -- which is what lets one trunk legitimately carry smooth young branch
     * bark and furrowed mature trunk bark at the same time without mixing
     * families. The bark GEOMETRY pass will refine this; here it only selects the
     * material category. */
    {
        f32 by_radius = tg_smoothstepf(res->profile->tip_radius_m * 6.0f,
                                       res->trunk_base_radius_m * 0.35f,
                                       o->radius_base);
        f32 by_age = tg_smoothstepf(2.0f, 25.0f, o->physiological_age);
        f32 maturity = 0.5f * by_radius + 0.5f * by_age;
        return (maturity > 0.5f) ? MESH_MAT_BARK_MATURE : MESH_MAT_BARK_YOUNG;
    }
}

static u32 pack_wood_colour(const Organ *o, const TreeResolved *res, f32 maturity) {
    /* Per-vertex colour only. The first implementation uses no image textures at
     * all, and colour never substitutes for geometry: it varies smoothly and
     * carries no fissure or ridge information, because those are triangles. */
    TgRng rng = tg_rng_substream(res->settings.seed, TG_RNG_MATERIAL_VARIATION,
                                 o->id, 0);
    f32 jitter = tg_rng_range(&rng, -0.035f, 0.035f);
    f32 base = tg_lerpf(0.30f, 0.22f, maturity) + jitter;
    if ((o->flags & ORGAN_FLAG_DEAD) != 0) {
        /* Weathered deadwood: grey, and lighter than live bark, which is what
         * makes a dead limb read as dead rather than as live wood in shadow. */
        return mesh_pack_rgba(base * 1.25f, base * 1.20f, base * 1.08f, 1.0f);
    }
    if (o->type == ORGAN_ROOT_SEGMENT) {
        return mesh_pack_rgba(base * 1.15f, base * 0.92f, base * 0.68f, 1.0f);
    }
    /* YOUNG SHOOTS ARE NOT GREY.
     *
     * The previous ramp started at 0.44 and desaturated toward grey, and the
     * rendered crown was full of pale sticks that read as dead twigs against the
     * foliage -- the twigs are the majority of the wood surface by area, so getting
     * their hue wrong miscolours the whole crown. A current-year shoot is olive to
     * red-brown; bark darkens and greys as it matures, so the green component is
     * suppressed with maturity rather than the red raised. */
    {
        f32 youth = 1.0f - tg_saturatef(maturity);
        f32 red = base * (1.00f + 0.22f * youth);
        f32 grn = base * (0.80f + 0.26f * youth);
        f32 blu = base * (0.62f + 0.06f * youth);
        return mesh_pack_rgba(red, grn, blu, 1.0f);
    }
}

/* Emits one ring and writes its vertex indices into `dst`. */
static TgResult emit_ring(SkinCtx *c, const Organ *o, Frame f, f32 radius,
                          f32 s_along_organ, f32 s_param, f32 arc_m, u32 nseg,
                          u32 *dst) {
    SectionShape sh;
    MeshMaterial mat;
    f32 maturity;
    u32 i;
    TgRng phase_rng;

    memset(&sh, 0, sizeof sh);
    sh.radius = radius;
    sh.lobe_amplitude = c->res->profile->lobing_amplitude;
    sh.lobe_count = c->res->profile->lobing_lobes;
    /* Phase is keyed to the AXIS, not the organ, so the lobes run continuously
     * along a branch instead of restarting at every internode. */
    phase_rng = tg_rng_substream(c->res->settings.seed, TG_RNG_CROSS_SECTION,
                                 o->axis, 0);
    sh.lobe_phase = tg_rng_f32(&phase_rng) * TG_TAU_F;
    sh.eccentricity = o->eccentricity;
    sh.ecc_angle = reaction_angle(f, c->res->profile->reaction_wood);

    mat = material_for(o, c->res);
    /* CONTINUOUS maturity, from the bark model itself.
     *
     * This was briefly taken from the material -- 1.0 for MESH_MAT_BARK_MATURE and
     * 0.0 otherwise -- and the consequence was that bark relief was completely
     * INVISIBLE on the rendered trunk: the furrow depth is proportional to
     * maturity, the trunk's material came back BARK_YOUNG, and so every furrow was
     * exactly zero deep. A binary maturity also cannot express what the bark model
     * exists to express, which is that maturity varies continuously up a trunk and
     * out along a limb. */
    (void)tree_bark_family_at(c->res, radius, &maturity);
    /* Bark relief is only expressed on living wood. Dead branches lose their
     * rhytidome to weathering and read as smooth or fibrous grey, not as a
     * furrowed oak trunk, and giving a dead limb full bark was one of the things
     * that made deadwood look like live wood painted grey. */
    if ((o->flags & ORGAN_FLAG_DEAD) != 0 || o->type == ORGAN_ROOT_SEGMENT) {
        maturity *= 0.25f;
    }

    for (i = 0; i < nseg; ++i) {
        MeshVertex v;
        f32 theta = (f32)i / (f32)nseg * TG_TAU_F;
        f32 r = section_radius(&sh, theta);
        f32 cscale = collar_scale(c->graph, o, c->res, f, s_along_organ, theta,
                                  &c->out->collars_applied);
        V3 dir = frame_ring_dir(f, theta);
        u32 idx;
        TgResult res;
        BarkSample bs;
        f32 bark_mat_maturity = maturity;

        memset(&v, 0, sizeof v);
        /* BARK IS A DISPLACEMENT OF THIS SURFACE, not a second shell.
         *
         * Adding a separate bark mesh over the wood would put two surfaces a
         * millimetre apart, which z-fights and doubles the triangle count for a
         * feature that is already only a radial offset. Displacing the tube's own
         * rings means the furrows are in the silhouette, occlude correctly under
         * grazing light, and cost nothing beyond the extra tessellation they need
         * to be resolved. */
        bs = tree_bark_sample(&c->bark, theta, arc_m, r * cscale,
                              bark_mat_maturity);
        v.position = v3_add(f.origin,
                            v3_scale(dir, r * cscale + bs.displacement_m));
        /* A provisional radial normal. mesh_compute_normals replaces it from the
         * actual triangles; keeping something sane here means a failure in that
         * pass shows as slightly wrong shading rather than as a NaN. */
        v.normal = dir;
        v.tangent = f.t;
        v.param = v2(s_param, (f32)i / (f32)nseg);
        v.color = pack_wood_colour(o, c->res, maturity);
        /* Furrow floors are darker than ridge crests -- but only as much as the
         * geometry justifies, because the ridges themselves are triangles and the
         * colour must not be doing their work. */
        if (c->bark.active) {
            v.color = mesh_scale_rgba(v.color, 0.55f + 0.45f * bs.exposure);
        }
        v.organ_id = o->id;
        v.birth_step = o->created_step;
        if (c->bark.active) {
            /* Material and occlusion both follow the geometry rather than being
             * painted on: the furrow floor IS deeper, so it IS more occluded, and
             * the ambient term is a real geometric quantity here rather than a
             * darkening factor chosen to look right. */
            v.attrib = mesh_pack_attrib(bs.material, MESH_SECTION_WOOD, 0);
            v.ao = 0.35f + 0.65f * bs.exposure;
        } else {
            v.attrib = mesh_pack_attrib(mat, MESH_SECTION_WOOD, 0);
            v.ao = 1.0f;
        }

        res = mesh_add_vertex(c->mesh, &v, &idx);
        if (res != TG_OK) { return res; }
        dst[i] = idx;
    }
    c->out->rings_emitted++;
    return TG_OK;
}

/* Duplicates a ring's vertices with an axial normal, so a cap meets the wall at a
 * genuinely hard edge.
 *
 * The duplicates sit at bit-identical positions, which is what lets the mesh
 * validator weld them and still see a closed surface. Sharing the rim instead
 * would average an axial and a radial normal into a normal that misrepresents
 * both surfaces -- the soft bulge the hard-edge check exists to detect. */
static TgResult emit_cap(SkinCtx *c, const Organ *o, const u32 *ring, u32 nseg,
                         V3 centre, V3 axial_normal, bool outward, f32 s_param) {
    u32 i;
    u32 centre_idx;
    TgResult r;
    MeshMaterial mat = material_for(o, c->res);

    for (i = 0; i < nseg; ++i) {
        MeshVertex v = mesh_vertices(c->mesh)[ring[i]];
        v.normal = axial_normal;
        v.attrib = mesh_pack_attrib(MESH_MAT_SAPWOOD, MESH_SECTION_WOOD,
                                    MESH_VFLAG_SEAM);
        r = mesh_add_vertex(c->mesh, &v, &c->ring_dup[i]);
        if (r != TG_OK) { return r; }
    }
    {
        MeshVertex v;
        memset(&v, 0, sizeof v);
        v.position = centre;
        v.normal = axial_normal;
        v.tangent = v3_any_perpendicular(axial_normal);
        v.param = v2(s_param, 0.5f);
        v.color = pack_wood_colour(o, c->res, 1.0f);
        v.organ_id = o->id;
        v.attrib = mesh_pack_attrib(MESH_MAT_SAPWOOD, MESH_SECTION_WOOD,
                                    MESH_VFLAG_SEAM);
        v.ao = 0.75f;
        r = mesh_add_vertex(c->mesh, &v, &centre_idx);
        if (r != TG_OK) { return r; }
    }
    TG_UNUSED(mat);
    return mesh_cap_ring(c->mesh, c->ring_dup, nseg, centre_idx, outward, o->id);
}

/* Frame at a parametric position along an organ, derived from the organ's stored
 * frame by translation. The tangent and reference are constant along a straight
 * internode, so no re-propagation is needed or wanted. */
static Frame frame_at(const Organ *o, f32 s) {
    Frame f;
    f.origin = v3_add(o->base, v3_scale(o->direction, o->length * s));
    f.t = o->direction;
    f.n = o->frame_ref;
    f.b = v3_cross(f.t, f.n);
    return f;
}

/* ------------------------------------------------------------------------- */
/* Axis sweep                                                                */
/* ------------------------------------------------------------------------- */

static TgResult sweep_axis(SkinCtx *c, u32 axis_id) {
    const TreeGraph *g = c->graph;
    const Axis *axis = tree_graph_axis(g, axis_id);
    u32 id;
    u32 nseg;
    bool first = true;
    TgResult r;
    f32 axis_length_acc = 0.0f;
    f32 total_length = tg_maxf(axis->total_length, 1e-6f);
    const Organ *last_organ = NULL;

    if (axis == NULL || axis->first_organ == TG_INVALID_ID) {
        c->out->axes_skipped_empty++;
        return TG_OK;
    }

    /* One segment count for the whole axis, taken from its thickest point.
     * Varying it along the axis would require re-tessellating between rings of
     * different counts, which is a source of cracks for no visual gain: an axis
     * spans well under one order of magnitude in radius. */
    {
        f32 max_r = 0.0f;
        id = axis->first_organ;
        while (id != TG_INVALID_ID) {
            const Organ *o = tree_graph_organ(g, id);
            u32 child, next = TG_INVALID_ID;
            if (o->radius_base > max_r) { max_r = o->radius_base; }
            for (child = o->first_child; child != TG_INVALID_ID;) {
                const Organ *cc = tree_graph_organ(g, child);
                if (cc->axis == axis_id &&
                    organ_type_is_segment((OrganType)cc->type)) {
                    next = child;
                    break;
                }
                child = cc->next_sibling;
            }
            id = next;
        }
        nseg = tree_skin_ring_segments(c->res, max_r);

        /* BARK RELIEF DECIDES ITS OWN TESSELLATION.
         *
         * The relief has a feature size, and a surface cannot carry a feature it
         * does not have samples for. Rather than raising the global ring count and
         * paying for it on a hundred thousand twigs, the bark field reports what it
         * needs and only the axes that actually carry relief -- on an 80-year
         * broadleaf, the trunk and the primary limbs, of the order of a hundred
         * axes -- are tessellated for it. */
        {
            BarkFamily fam;
            f32 unused_maturity;
            fam = tree_bark_family_at(c->res, max_r, &unused_maturity);
            tree_bark_axis_setup(c->res, axis_id, max_r, fam, &c->bark);
            if (axis->kind == AXIS_ROOT) {
                /* Roots are underground and are inspected in cutaway, where the
                 * relevant surface is the root's form, not its rhytidome. */
                c->bark.active = false;
            }
            if (c->bark.active) {
                u32 want = tree_bark_ring_segments(&c->bark);
                if (want > nseg) {
                    nseg = tg_min_u32(want, TS_MAX_RING_SEGMENTS);
                }
                c->out->bark_axes++;
            }
        }
    }
    if (nseg < c->out->min_ring_segments || c->out->min_ring_segments == 0) {
        c->out->min_ring_segments = nseg;
    }
    if (nseg > c->out->max_ring_segments) { c->out->max_ring_segments = nseg; }

    id = axis->first_organ;
    while (id != TG_INVALID_ID) {
        const Organ *o = tree_graph_organ(g, id);
        u32 child, next = TG_INVALID_ID;

        if (first) {
            r = emit_ring(c, o, frame_at(o, 0.0f), o->radius_base, 0.0f, 0.0f,
                          0.0f, nseg, c->ring_a);
            if (r != TG_OK) { return r; }

            /* Basal cap. For the trunk and for root axes this is a real end of
             * the solid; for a lateral it is buried inside the parent, which is
             * the interim state documented in the header. */
            {
                V3 n = v3_neg(o->direction);
                r = emit_cap(c, o, c->ring_a, nseg, o->base, n, false, 0.0f);
                if (r != TG_OK) { return r; }
                if (axis->parent_organ != TG_INVALID_ID) {
                    c->out->interpenetrating_unions++;
                }
            }
            first = false;
        }

        /* One ring per internode is right for a twig and far too coarse for a
         * bark-bearing limb: an 18 m trunk in 106 rings cannot show a 6 cm ridge.
         * The internode is therefore subdivided to whatever the relief needs, and
         * to nothing more when there is no relief. */
        {
            f32 spacing = tree_bark_ring_spacing(&c->bark);
            u32 sub = 1u;
            u32 k;
            if (c->bark.active && spacing > 1e-5f) {
                sub = tg_clamp_u32((u32)(o->length / spacing + 0.999f), 1u, 64u);
            }
            for (k = 1u; k <= sub; ++k) {
                f32 s_local = (f32)k / (f32)sub;
                f32 rad = tg_lerpf(o->radius_base, o->radius_tip, s_local);
                r = emit_ring(c, o, frame_at(o, s_local), rad, s_local,
                              (axis_length_acc + o->length * s_local)
                                  / total_length,
                              axis_length_acc + o->length * s_local,
                              nseg, c->ring_b);
                if (r != TG_OK) { return r; }
                r = mesh_stitch_rings(c->mesh, c->ring_a, c->ring_b, nseg, o->id);
                if (r != TG_OK) { return r; }
                memcpy(c->ring_a, c->ring_b, (size_t)nseg * sizeof(u32));
            }
        }
        axis_length_acc += o->length;

        last_organ = o;
        for (child = o->first_child; child != TG_INVALID_ID;) {
            const Organ *cc = tree_graph_organ(g, child);
            if (cc->axis == axis_id &&
                organ_type_is_segment((OrganType)cc->type)) {
                next = child;
                break;
            }
            child = cc->next_sibling;
        }
        id = next;
    }

    /* Distal cap. A twig terminating in an unexplained flat disc is one of the
     * failure modes the directive names, so the tip cap is deliberately tiny:
     * the final ring is already at tip_radius, a couple of millimetres across. */
    if (last_organ != NULL) {
        r = emit_cap(c, last_organ, c->ring_a, nseg, organ_tip(last_organ),
                     last_organ->direction, true, 1.0f);
        if (r != TG_OK) { return r; }
    }

    c->out->axes_meshed++;
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                               */
/* ------------------------------------------------------------------------- */

TgResult tree_skin_build(Mesh *mesh, const TreeGraph *graph,
                         const TreeResolved *resolved, SkinResult *out) {
    SkinCtx c;
    SkinResult local;
    TgResult r;
    u32 a, na;
    u64 before_v, before_t;

    TG_CHECK(mesh != NULL);
    if (graph == NULL || resolved == NULL || resolved->profile == NULL) {
        return TG_ERR_INVALID_ARGUMENT;
    }
    if (tree_graph_organ_count(graph) == 0) {
        TG_LOG_ERRORF(TS_SUB, "cannot skin an empty graph");
        return TG_ERR_INVALID_STATE;
    }

    /* Radii are the mechanics pass's output. Skinning without them would produce
     * a tree of uniform twigs, so it is refused rather than silently done. */
    {
        const Axis *trunk = tree_graph_axis(graph, graph->trunk_axis);
        if (trunk == NULL || trunk->first_organ == TG_INVALID_ID ||
            !(tree_graph_organ(graph, trunk->first_organ)->radius_base > 0.0f)) {
            TG_LOG_ERRORF(TS_SUB,
                          "graph has no radii: run tree_mechanics_run first");
            return TG_ERR_INVALID_STATE;
        }
    }

    memset(&local, 0, sizeof local);
    memset(&c, 0, sizeof c);
    c.mesh = mesh;
    c.graph = graph;
    c.res = resolved;
    c.out = &local;

    c.ring_bytes = (u64)TS_MAX_RING_SEGMENTS * sizeof(u32);
    c.ring_a = (u32 *)tg_alloc(c.ring_bytes);
    c.ring_b = (u32 *)tg_alloc(c.ring_bytes);
    c.ring_dup = (u32 *)tg_alloc(c.ring_bytes);
    if (c.ring_a == NULL || c.ring_b == NULL || c.ring_dup == NULL) {
        r = TG_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }

    before_v = mesh_vertex_count(mesh);
    before_t = mesh_triangle_count(mesh);

    r = mesh_begin_section(mesh, MESH_SECTION_WOOD);
    if (r != TG_OK) { goto cleanup; }

    na = tree_graph_axis_count(graph);
    for (a = 0; a < na; ++a) {
        r = sweep_axis(&c, a);
        if (r == TG_ERR_LIMIT_EXCEEDED) {
            /* Reported, never silently truncated. Whatever was emitted so far is
             * still a valid set of closed components. */
            local.hit_vertex_limit = true;
            TG_LOG_WARNF(TS_SUB,
                         "vertex or triangle ceiling reached after %u of %u axes",
                         a, na);
            r = TG_OK;
            break;
        }
        if (r != TG_OK) { goto cleanup_section; }
    }

    r = mesh_end_section(mesh);
    if (r != TG_OK) { goto cleanup; }

    r = mesh_compute_normals(mesh, MESH_SECTION_WOOD, 1.2f,
                             &local.hard_corner_violations);
    if (r != TG_OK) { goto cleanup; }
    /* Tangents are NOT computed from the parameterisation here: the axis
     * direction already is the grain direction, and it was written per vertex
     * during emission. Deriving them from param would additionally be wrong at
     * the angular seam, where param.y wraps from 1 back to 0. */

    local.vertices = (u32)(mesh_vertex_count(mesh) - before_v);
    local.triangles = (u32)(mesh_triangle_count(mesh) - before_t);

    TG_LOG_INFOF(TS_SUB,
                 "skinned %u axes: %u rings, %u vertices, %u triangles, "
                 "ring segments %u..%u",
                 local.axes_meshed, local.rings_emitted, local.vertices,
                 local.triangles, local.min_ring_segments,
                 local.max_ring_segments);
    if (local.hard_corner_violations > 0) {
        TG_LOG_WARNF(TS_SUB, "%u corners exceed the hard-edge threshold",
                     local.hard_corner_violations);
    }
    if (local.interpenetrating_unions > 0) {
        TG_LOG_WARNF(TS_SUB,
                     "%u branch unions are interpenetrating tubes rather than "
                     "welded junctions: interim state, see docs/limitations.md",
                     local.interpenetrating_unions);
    }
    goto cleanup;

cleanup_section:
    (void)mesh_end_section(mesh);
cleanup:
    if (c.ring_a != NULL) { tg_free(c.ring_a, c.ring_bytes); }
    if (c.ring_b != NULL) { tg_free(c.ring_b, c.ring_bytes); }
    if (c.ring_dup != NULL) { tg_free(c.ring_dup, c.ring_bytes); }
    if (out != NULL) { *out = local; }
    return r;
}
