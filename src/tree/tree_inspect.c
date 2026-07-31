#include "tree_inspect.h"
#include "tree_mechanics.h"

#include "../core/log.h"
#include "../core/mem.h"

#include <stdio.h>
#include <string.h>

/* Fills everything that depends only on the organ, not on the ray. */
static void fill_from_organ(const Tree *t, u32 organ_id, InspectResult *out) {
    const Organ *o = tree_graph_organ(&t->graph, organ_id);
    const Axis *a;
    u32 walk;
    u32 guard = 0;

    out->organ_id = organ_id;
    out->organ_type = (OrganType)o->type;
    out->branch_order = o->branch_order;
    out->axis_id = o->axis;
    out->dead = (o->flags & ORGAN_FLAG_DEAD) != 0;
    out->terminal = (o->flags & ORGAN_FLAG_TERMINAL) != 0;
    out->created_step = o->created_step;
    out->death_step = out->dead ? o->death_step : 0u;
    out->length_m = o->length;
    out->radius_base_m = o->radius_base;
    out->radius_tip_m = o->radius_tip;
    out->physiological_age_years = o->physiological_age;
    out->eccentricity = o->eccentricity;
    out->supported_leaf_area_m2 = o->supported_leaf_area;
    out->supported_mass_kg = o->supported_mass;
    out->light = o->light;
    out->height_above_ground_m = organ_tip(o).y;

    a = (o->axis < tree_graph_axis_count(&t->graph))
          ? tree_graph_axis(&t->graph, o->axis) : NULL;
    out->axis_kind = (a != NULL) ? (AxisKind)a->kind : AXIS_ORTHOTROPIC;

    /* Walk to the base along the ACTUAL parent chain, summing segment lengths.
     *
     * Not the straight-line distance: the interesting quantity is how far sap has
     * to travel and how much wood is between this point and the ground, and on a
     * long arching limb those differ by tens of per cent. The parent-id-is-less
     * invariant guarantees this walk terminates, and the guard is there for the
     * case where it does not, because a silent infinite loop in an interactive
     * cursor is the worst possible failure. */
    walk = organ_id;
    while (walk != TG_INVALID_ID && guard++ < 1u << 20) {
        const Organ *w = tree_graph_organ(&t->graph, walk);
        if (organ_type_is_segment((OrganType)w->type)) {
            out->path_length_from_base_m += w->length;
            out->depth_from_base++;
        }
        if (w->parent == TG_INVALID_ID) { break; }
        TG_CHECK(w->parent < walk);
        walk = w->parent;
    }
}

bool tree_inspect_ray(const Tree *t, Ray ray, InspectResult *out) {
    MeshBvhHit hit;

    TG_CHECK(t != NULL);
    if (out != NULL) { memset(out, 0, sizeof *out); }
    if (t->bvh.node_count == 0u) {
        /* Deliberately a refusal rather than a fallback linear scan. A fallback
         * would work on the twelve-triangle test tree and stall for a second per
         * mouse move on a real one, which is the kind of defect that only appears
         * in front of a user. */
        TG_LOG_WARNF("tree_inspect",
                     "ray inspection needs the BVH; build with build_bvh set");
        return false;
    }
    if (!mesh_bvh_raycast(&t->bvh, &t->mesh, ray, false, &hit)) {
        if (out != NULL) {
            out->nodes_visited = hit.nodes_visited;
            out->triangles_tested = hit.triangles_tested;
        }
        return false;
    }
    if (out == NULL) { return true; }

    out->hit = true;
    out->position = hit.position;
    out->normal = hit.normal;
    out->distance = hit.t;
    out->triangle = hit.triangle;
    out->section = hit.section;
    out->nodes_visited = hit.nodes_visited;
    out->triangles_tested = hit.triangles_tested;
    {
        MeshTriangle tri;
        if (mesh_get_triangle(&t->mesh, hit.triangle, &tri)) {
            out->material = tri.material;
        }
    }
    if (hit.organ_id < tree_graph_organ_count(&t->graph)) {
        fill_from_organ(t, hit.organ_id, out);
    } else {
        out->organ_id = TG_INVALID_ID;
    }
    return true;
}

bool tree_inspect_organ(const Tree *t, u32 organ_id, InspectResult *out) {
    const Organ *o;

    TG_CHECK(t != NULL);
    if (out != NULL) { memset(out, 0, sizeof *out); }
    if (organ_id >= tree_graph_organ_count(&t->graph)) { return false; }
    if (out == NULL) { return true; }
    o = tree_graph_organ(&t->graph, organ_id);
    out->hit = true;
    out->position = v3_add(o->base, v3_scale(o->direction, o->length * 0.5f));
    out->normal = o->direction;
    out->triangle = 0;
    out->section = MESH_SECTION_WOOD;
    fill_from_organ(t, organ_id, out);
    return true;
}

u64 tree_inspect_describe(const Tree *t, const InspectResult *r, char *buf,
                          u64 size) {
    char local[1024];
    int n;

    TG_CHECK(t != NULL && r != NULL);
    if (!r->hit) {
        n = snprintf(local, sizeof local, "nothing under the cursor\n");
    } else {
        n = snprintf(local, sizeof local,
            "%s, branch order %u, axis %u (%s)%s%s\n"
            "  formed in year %u of %u; cambial age %.1f yr\n"
            "  %.4f m long, radius %.4f m at the base to %.4f m at the tip\n"
            "  carries %.4f m2 of leaf and %.3f kg of wood distal to it\n"
            "  %.2f m above ground, %.2f m of stem from the base, %u organs deep\n"
            "  light %.2f, section eccentricity %.3f, material %s\n"
            "  pick cost: %u nodes visited, %u triangles tested\n",
            organ_type_name(r->organ_type), r->branch_order, r->axis_id,
            axis_kind_name(r->axis_kind),
            r->dead ? ", DEAD" : "", r->terminal ? ", terminal shoot" : "",
            (unsigned)r->created_step, t->resolved.growth_steps,
            (double)r->physiological_age_years,
            (double)r->length_m, (double)r->radius_base_m,
            (double)r->radius_tip_m,
            (double)r->supported_leaf_area_m2, (double)r->supported_mass_kg,
            (double)r->height_above_ground_m,
            (double)r->path_length_from_base_m, r->depth_from_base,
            (double)r->light, (double)r->eccentricity,
            mesh_material_name(r->material),
            r->nodes_visited, r->triangles_tested);
    }
    if (n < 0) { return 0; }
    if (buf != NULL && size > 0u) {
        u64 copy = ((u64)n < size - 1u) ? (u64)n : size - 1u;
        memcpy(buf, local, (size_t)copy);
        buf[copy] = '\0';
    }
    return (u64)n;
}

/* State for the region walk. The organ set is a BITSET over organ ids, which is
 * what makes "distinct organs" an exact count rather than an estimate. One bit per
 * organ is 149 KB for the largest tree this engine builds (1 190 126 organs), so
 * exactness here costs less than the sample buffer it replaces cost in stack. */
typedef struct RegionWalk {
    const Tree   *tree;
    InspectRegion *out;
    u64          *seen;        /* one bit per organ id                          */
    u32           organ_count;
} RegionWalk;

static void region_visit(u64 triangle, const MeshTriangle *tri, void *user) {
    RegionWalk *w = (RegionWalk *)user;
    InspectRegion *out = w->out;
    const Organ *o;
    u32 id = tri->organ_id;

    (void)triangle;
    out->triangles++;
    if (id >= w->organ_count) { return; }
    /* Exact de-duplication. Every accumulation below is PER ORGAN, so an organ
     * whose triangles are scattered across the BVH -- which is every organ, because
     * the build permutes spatially -- must contribute exactly once. */
    if ((w->seen[id >> 6] >> (id & 63u)) & 1u) { return; }
    w->seen[id >> 6] |= 1ull << (id & 63u);

    o = tree_graph_organ(&w->tree->graph, id);
    out->organs++;
    if ((o->flags & ORGAN_FLAG_DEAD) == 0) { out->living_organs++; }
    if (o->branch_order > out->max_branch_order) {
        out->max_branch_order = o->branch_order;
    }
    if (o->created_step < out->earliest_step) {
        out->earliest_step = o->created_step;
    }
    if (o->created_step > out->latest_step) {
        out->latest_step = o->created_step;
    }
    out->total_leaf_area_m2 +=
        tree_mechanics_segment_leaf_area(o, &w->tree->resolved,
                                         (u16)w->tree->resolved.growth_steps);
    if (o->radius_base > out->max_radius_m) {
        out->max_radius_m = o->radius_base;
    }
    if (o->radius_tip < out->min_radius_m && o->radius_tip > 0.0f) {
        out->min_radius_m = o->radius_tip;
    }
}

bool tree_inspect_region(const Tree *t, Aabb box, InspectRegion *out) {
    /* This query used to sample the first 4096 triangles the BVH handed back and
     * count distinct organs with a ONE-ELEMENT memo, on the stated grounds that
     * "triangles arrive grouped by organ because the skin pass emits them that
     * way". They do not: mesh_bvh_query_box walks bvh->tri_ids, which the build
     * spatially permutes, so an organ's triangles arrive in many separate runs and
     * each run was counted as another organ. Measured on a 40-year broadleaf, the
     * two errors ran in opposite directions and neither was small:
     *
     *     box       organs reported / exact      living        truncated
     *      4%           25 / 6    (x4.17)      x7.50          NO
     *      8%          213 / 92   (x2.32)      x4.10          yes
     *     16%          595 / 1520 (x0.39)      x1.37          yes
     *     32%          941 / 8805 (x0.11)      x0.19          yes
     *
     * The 4% box is the one that matters. It fitted inside the sample buffer
     * entirely, so the function reported truncated = false -- a caller doing
     * exactly what the API told it to do got an answer four times too large with no
     * indication at all. Leaf area was worse: 0.000 m2 reported against 0.039 m2
     * present, because a scattered organ's area was added on its first run and
     * whichever organ happened to end a run was skipped.
     *
     * So: every overlapping triangle is now visited, and organs are de-duplicated
     * through a bitset. There is no sample and no truncation. */
    RegionWalk w;
    u32 organ_count;
    u64 words, bytes;

    TG_CHECK(t != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->min_radius_m = 1.0e9f;
    out->earliest_step = 0xFFFFu;
    if (t->bvh.node_count == 0u) { return false; }

    organ_count = tree_graph_organ_count(&t->graph);
    words = ((u64)organ_count + 63u) / 64u;
    bytes = words * sizeof(u64);
    w.seen = (words > 0u) ? (u64 *)tg_alloc_zero(bytes) : NULL;
    if (words > 0u && w.seen == NULL) {
        /* Refuse rather than fall back to the sampling estimate. A caller cannot
         * act on a number whose accuracy depends on whether an allocation
         * succeeded, and silently degrading is how the old behaviour survived. */
        TG_LOG_WARNF("tree_inspect",
                     "region query needs %llu bytes to count organs exactly and "
                     "could not get them; refusing rather than estimating",
                     (unsigned long long)bytes);
        return false;
    }

    w.tree = t;
    w.out = out;
    w.organ_count = organ_count;
    (void)mesh_bvh_visit_box(&t->bvh, &t->mesh, box, region_visit, &w);
    tg_free(w.seen, bytes);

    if (out->min_radius_m > 1.0e8f) { out->min_radius_m = 0.0f; }
    if (out->earliest_step == 0xFFFFu) { out->earliest_step = 0u; }
    return out->triangles > 0u;
}
