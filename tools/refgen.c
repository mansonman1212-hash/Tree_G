/* refgen.c -- headless reference generator and validation capture tool.
 *
 * TEST INSTRUMENT. Not part of the shipped application.
 *
 * Runs the real generation pipeline end to end, validates the result, and writes
 * PNG captures plus an OBJ so the geometry can be inspected outside this program.
 * Its whole purpose is to make claims about how the tree LOOKS checkable in an
 * environment with no GPU and no display.
 *
 * Every number it prints is measured, never estimated.
 */
#include "../src/core/log.h"
#include "../src/geom/camera.h"
#include "../src/geom/mesh_validate.h"
#include "../src/tree/tree_growth.h"
#include "../src/tree/tree_mechanics.h"
#include "../src/tree/tree_skin.h"
#include "../src/tree/tree_foliage.h"
#include "../src/tree/tree_bark.h"

#include "img_png.h"
#include "swrast.h"

#include <stdio.h>
#include <string.h>

typedef struct Built {
    TreeSettings     settings;
    TreeResolved     resolved;
    TreeGraph        graph;
    GrowthResult     growth;
    MechanicsResult  mechanics;
    SkinResult       skin;
    FoliageResult    foliage;
    Mesh             mesh;
    MeshValidateReport validation;
    bool             valid;
} Built;

static TgResult build_tree(Built *b, TreeCategory cat, f32 age, TreeQuality q,
                           u64 seed, TreeSeason season) {
    TgResult r;

    memset(b, 0, sizeof *b);
    b->settings = tree_settings_default(cat);
    b->settings.age_years = age;
    b->settings.quality = q;
    b->settings.seed = seed;
    b->settings.season = season;

    r = tree_profile_resolve(&b->settings, &b->resolved);
    if (r != TG_OK) { return r; }
    r = tree_graph_init(&b->graph, 8192, b->resolved.max_organs);
    if (r != TG_OK) { return r; }
    r = tree_growth_run(&b->graph, &b->resolved, NULL, &b->growth);
    if (r != TG_OK) { return r; }
    r = tree_growth_roots(&b->graph, &b->resolved, NULL, &b->growth);
    if (r != TG_OK) { return r; }
    r = tree_mechanics_run(&b->graph, &b->resolved, &b->mechanics);
    if (r != TG_OK) { return r; }

    r = mesh_init(&b->mesh, 1u << 16, 1u << 17);
    if (r != TG_OK) { return r; }
    r = tree_skin_build(&b->mesh, &b->graph, &b->resolved, &b->skin);
    if (r != TG_OK) { return r; }
    r = tree_foliage_build(&b->mesh, &b->graph, &b->resolved,
                           (u16)b->resolved.growth_steps, &b->foliage);
    if (r != TG_OK) { return r; }
    r = mesh_finalize(&b->mesh);
    if (r != TG_OK) { return r; }

    /* Validation is not optional and its result is not assumed. */
    {
        MeshValidateOptions opt = mesh_validate_default_options();
        opt.max_scratch_bytes = (u64)2048 * 1024 * 1024;
        b->valid = (mesh_validate(&b->mesh, &opt, &b->validation) == TG_OK);
    }
    return TG_OK;
}

static void destroy_tree(Built *b) {
    mesh_destroy(&b->mesh);
    tree_graph_destroy(&b->graph);
}

static Aabb section_bounds_or_all(const Mesh *m, bool roots_only, bool crown_only,
                                  const TreeGraph *g, f32 crown_base) {
    Aabb bb = aabb_empty();
    u64 i;
    const MeshVertex *v = mesh_vertices(m);
    for (i = 0; i < mesh_vertex_count(m); ++i) {
        const Organ *o = tree_graph_organ(g, v[i].organ_id);
        bool is_root = (o != NULL && o->type == ORGAN_ROOT_SEGMENT);
        if (roots_only && !is_root) { continue; }
        if (crown_only && (is_root || v[i].position.y < crown_base)) { continue; }
        bb = aabb_add_point(bb, v[i].position);
    }
    if (aabb_is_empty(bb)) { bb = m->bounds; }
    return bb;
}

typedef struct ViewSpec {
    const char *name;
    f32  yaw, pitch;
    f32  margin;
    bool roots_only;
    bool crown_only;
    bool wireframe;
    bool depth;
    f32  distance_scale;  /* <1 moves in for a close-up                      */
    V3   focus_offset;    /* world offset applied to the pivot                */
    /* When set, focus_offset is an ABSOLUTE world point rather than an offset.
     * Needed because the bounds pivot of a mature tree sits ten metres up in the
     * crown, so a close-up specified as an offset lands wherever the crown happens
     * to be -- the first attempt at a bark view framed a twig. */
    bool focus_absolute;
} ViewSpec;

static TgResult capture(const Built *b, const ViewSpec *vs, const char *prefix,
                        u32 width, u32 height, u8 *rgb, SwTarget *target) {
    Camera cam;
    SwLight light = sw_default_light();
    Aabb bb;
    char path[512];
    TgResult r;

    bb = section_bounds_or_all(&b->mesh, vs->roots_only, vs->crown_only,
                               &b->graph, b->resolved.crown_base_height_m);

    camera_init(&cam);
    camera_set_aspect(&cam, width, height);
    camera_frame_bounds_reset_angle(&cam, bb, vs->margin, vs->yaw, vs->pitch);
    if (vs->focus_absolute) { cam.pivot = vs->focus_offset; }
    else { cam.pivot = v3_add(cam.pivot, vs->focus_offset); }
    if (vs->distance_scale > 0.0f && vs->distance_scale != 1.0f) {
        cam.distance *= vs->distance_scale;
    }

    sw_target_clear(target);
    r = sw_draw_mesh(target, &cam, &b->mesh, &light);
    if (r != TG_OK) { return r; }
    if (vs->wireframe) {
        r = sw_draw_wireframe(target, &cam, &b->mesh, MESH_SECTION_WOOD,
                              v3(0.0f, 1.6f, 0.9f));
        if (r != TG_OK) { return r; }
    }

    if (vs->depth) {
        r = sw_resolve_depth(target, rgb);
    } else {
        r = sw_resolve_srgb(target, 1.0f, rgb);
    }
    if (r != TG_OK) { return r; }

    (void)snprintf(path, sizeof path, "%s_%s.png", prefix, vs->name);
    r = png_write_rgb(path, rgb, width, height);
    if (r != TG_OK) { return r; }

    printf("    %-16s %6llu tris submitted, %6llu culled, %5llu dropped at the "
           "eye plane, %9llu fragments -> %s\n",
           vs->name,
           (unsigned long long)target->triangles_submitted,
           (unsigned long long)target->triangles_culled,
           (unsigned long long)target->triangles_clipped,
           (unsigned long long)target->fragments_written, path);
    if (target->fragments_written == 0) {
        printf("    WARNING: %s wrote no fragments; the capture is empty\n",
               vs->name);
    }
    return TG_OK;
}

static TgResult write_obj(const Built *b, const char *path) {
    FILE *f = fopen(path, "wb");
    u64 i;
    const MeshVertex *v;
    const u32 *ind;

    if (f == NULL) { return TG_ERR_IO; }
    v = mesh_vertices(&b->mesh);
    ind = mesh_indices(&b->mesh);
    fprintf(f, "# Tree_G generated woody surface\n");
    fprintf(f, "# settings hash %016llx, seed %llu, age %.1f yr, quality %s\n",
            (unsigned long long)tree_settings_hash(&b->settings),
            (unsigned long long)b->settings.seed, (double)b->settings.age_years,
            tree_quality_name(b->settings.quality));
    for (i = 0; i < mesh_vertex_count(&b->mesh); ++i) {
        fprintf(f, "v %.6f %.6f %.6f\n", (double)v[i].position.x,
                (double)v[i].position.y, (double)v[i].position.z);
    }
    for (i = 0; i < mesh_vertex_count(&b->mesh); ++i) {
        fprintf(f, "vn %.5f %.5f %.5f\n", (double)v[i].normal.x,
                (double)v[i].normal.y, (double)v[i].normal.z);
    }
    for (i = 0; i < mesh_triangle_count(&b->mesh); ++i) {
        u64 base = i * 3u;
        fprintf(f, "f %u//%u %u//%u %u//%u\n",
                ind[base] + 1u, ind[base] + 1u,
                ind[base + 1] + 1u, ind[base + 1] + 1u,
                ind[base + 2] + 1u, ind[base + 2] + 1u);
    }
    if (fclose(f) != 0) { return TG_ERR_IO; }
    return TG_OK;
}

static void report(const Built *b, const char *label) {
    const MeshStats st = mesh_stats(&b->mesh);
    GraphValidateReport gr;

    (void)tree_graph_validate(&b->graph, 0.0f, &gr);

    printf("\n=== %s ===\n", label);
    printf("  settings   seed=%llu age=%.0f yr quality=%s season=%s hash=%016llx\n",
           (unsigned long long)b->settings.seed, (double)b->settings.age_years,
           tree_quality_name(b->settings.quality),
           tree_season_name(b->settings.season),
           (unsigned long long)tree_settings_hash(&b->settings));
    printf("  growth     %u of %u steps%s, %u shoots self-pruned, "
           "%u pinned at the crown surface\n",
           b->growth.steps_run, b->resolved.growth_steps,
           b->growth.hit_organ_limit ? " (TRUNCATED: organ ceiling reached)" : "",
           b->growth.shoots_killed, b->growth.stopped_by_envelope);
    printf("             shed: %u by shade, %u by crown recession\n",
           b->growth.stopped_by_shade, b->growth.stopped_by_recession);
    printf("  graph      %s | %u organs, %u axes, %u segments, %u buds, "
           "%u dead, max order %u\n",
           gr.passed ? "VALID" : "INVALID",
           gr.organs, gr.axes, gr.segments, gr.buds, gr.dead_organs,
           gr.max_branch_order);
    printf("  form       height %.2f m (target %.2f), root depth %.2f m, "
           "trunk base r %.4f m\n",
           (double)gr.max_height, (double)b->resolved.height_m,
           (double)gr.min_root_depth, (double)b->mechanics.trunk_base_radius_m);
    printf("  mechanics  wood %.0f kg, foliage %.1f kg, leaf area %.1f m2, "
           "%u terminal shoots\n",
           (double)b->mechanics.total_wood_mass_kg,
           (double)b->mechanics.total_foliage_mass_kg,
           (double)b->mechanics.total_leaf_area_m2,
           b->mechanics.terminal_shoots);
    printf("  deflection max bend %.4f rad, max displacement %.3f m, "
           "%u of %u clamped, sign violations %u\n",
           (double)b->mechanics.max_segment_rotation_rad,
           (double)b->mechanics.max_tip_deflection_m,
           b->mechanics.clamped_rotations, b->mechanics.segments_considered,
           b->mechanics.own_bend_upward);
    printf("  bark       %u of %u axes carry relief, family %s -> %s, "
           "feature %.3f m\n",
           b->skin.bark_axes, b->skin.axes_meshed,
           tree_bark_family_name(b->resolved.profile->bark_juvenile),
           tree_bark_family_name(b->resolved.profile->bark_mature),
           (double)b->resolved.bark_feature_size_m);
    printf("  surface    %llu vertices, %llu triangles, rings %u, "
           "ring segments %u..%u, collars %u\n",
           (unsigned long long)st.vertices, (unsigned long long)st.triangles,
           b->skin.rings_emitted, b->skin.min_ring_segments,
           b->skin.max_ring_segments, b->skin.collars_applied);
    printf("  memory     CPU %.1f MiB, GPU estimate %.1f MiB\n",
           (double)st.cpu_bytes / (1024.0 * 1024.0),
           (double)st.gpu_bytes_estimate / (1024.0 * 1024.0));
    printf("  mesh       %s | %u closed components, %llu boundary edges, "
           "enclosed volume %.4f m3\n",
           b->validation.topology_not_checked
               ? "NOT VALIDATED (too large for the topology scratch buffers; "
                 "the counts below were never measured)"
               : (b->valid ? "VALID" : "INVALID"),
           b->validation.closed_components[MESH_SECTION_WOOD],
           (unsigned long long)b->validation.boundary_edges[MESH_SECTION_WOOD],
           b->validation.enclosed_volume[MESH_SECTION_WOOD]);
    printf("  foliage    %llu of %llu %s placed (%.1f%%), %u tris each, "
           "%u bearing shoots\n",
           (unsigned long long)b->foliage.leaves_placed,
           (unsigned long long)b->foliage.leaves_wanted,
           b->resolved.profile->category == TREE_CATEGORY_CONIFER ? "needles"
                                                                  : "leaves",
           b->foliage.leaves_wanted > 0u
               ? 100.0 * (double)b->foliage.leaves_placed
                       / (double)b->foliage.leaves_wanted
               : 0.0,
           b->foliage.triangles_per_leaf, b->foliage.bearing_segments);
    printf("  leaf area  %.1f m2 realised on the geometry placed, "
           "%.1f m2 assumed by the mechanics for the full crown\n",
           (double)b->foliage.realised_leaf_area_m2,
           (double)b->foliage.target_leaf_area_m2);
    printf("  interim    %u unions are interpenetrating tubes (junction meshing "
           "not yet implemented)\n", b->skin.interpenetrating_unions);
    if (!b->valid) { mesh_validate_log_report(&b->validation); }
    printf("  wood volume check: %.4f m3 implies %.0f kg at %.0f kg/m3\n",
           b->validation.enclosed_volume[MESH_SECTION_WOOD],
           b->validation.enclosed_volume[MESH_SECTION_WOOD]
               * (double)b->resolved.profile->wood_density_kgm3,
           (double)b->resolved.profile->wood_density_kgm3);
}

int main(int argc, char **argv) {
    static const ViewSpec views[] = {
        { "front",      0.00f,  -0.10f, 0.06f, false, false, false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "threequarter", -0.70f, -0.18f, 0.06f, false, false, false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "side",      -1.5708f, -0.10f, 0.06f, false, false, false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "top",        0.00f,  -1.45f, 0.06f, false, false, false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "roots",     -0.70f,   0.55f, 0.10f, true,  false, false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "crown",     -0.70f,  -0.25f, 0.04f, false, true,  false, false, 1.0f,
          { 0, 0, 0 }, false },
        { "trunkbase", -0.70f,  -0.05f, 0.02f, false, false, false, false, 0.10f,
          { 0, 0, 0 }, false },
        /* BARK. The relief is 3 to 7 cm deep and the trunkbase view frames several
         * metres, which puts a furrow at two or three pixels: measured relief of
         * 74 mm peak-to-trough looked like a perfectly smooth cylinder. Bark cannot
         * be judged without a view that resolves it, so this one frames roughly
         * half a metre of bole at breast height. The pivot is raised because the
         * bounds pivot sits at mid-tree, ten metres up in the crown. */
        { "bark",      -0.70f,  -0.02f, 0.00f, false, false, false, false, 0.026f,
          { 0.0f, 1.60f, 0.0f }, true },
        { "barkwire",  -0.70f,  -0.02f, 0.00f, false, false, true,  false, 0.026f,
          { 0.0f, 1.60f, 0.0f }, true },
        { "wireframe", -0.70f,  -0.18f, 0.06f, false, false, true,  false, 1.0f,
          { 0, 0, 0 }, false },
        { "depth",     -0.70f,  -0.18f, 0.06f, false, false, false, true,  1.0f,
          { 0, 0, 0 }, false }
    };
    const u32 width = 900, height = 1100;
    const char *out_dir = (argc > 1) ? argv[1] : "artifacts";
    /* Optional prefix filter. Regenerating four mature trees takes minutes, and
     * iterating on one surface should not require rebuilding all of them. */
    const char *only = (argc > 2) ? argv[2] : NULL;
    u8 *rgb;
    SwTarget target;
    TgResult r;
    u32 vi;
    Built b;
    int failures = 0;

    tg_log_init(TG_LOG_WARN);

    rgb = (u8 *)tg_alloc((u64)width * height * 3u);
    if (rgb == NULL) { return 1; }
    r = sw_target_create(&target, width, height);
    if (r != TG_OK) {
        printf("cannot create render target: %s\n", tg_result_name(r));
        return 1;
    }

    printf("Tree_G reference generator\n");
    printf("captures -> %s/\n", out_dir);

    {
        struct {
            const char  *label;
            const char  *prefix;
            TreeCategory cat;
            f32          age;
            TreeQuality  quality;
            u64          seed;
            TreeSeason   season;
            /* An OBJ of a mature tree runs to hundreds of megabytes and cannot be
             * inspected from inside this repository anyway. One is written for the
             * young tree, which is small enough to be genuinely useful for
             * checking the geometry in an external viewer. */
            bool         want_obj;
        } cases[] = {
            { "broadleaf, 80 yr, draft",  "broadleaf_80",  
              TREE_CATEGORY_BROADLEAF, 80.0f,  QUALITY_DRAFT,    0x5EEDC0FFEEull,
              SEASON_SUMMER, false },
            { "broadleaf, 12 yr, draft",  "broadleaf_12",
              TREE_CATEGORY_BROADLEAF, 12.0f,  QUALITY_DRAFT,    0x5EEDC0FFEEull,
              SEASON_SUMMER, true },
            { "broadleaf, 220 yr, draft", "broadleaf_220",
              TREE_CATEGORY_BROADLEAF, 220.0f, QUALITY_DRAFT,    0x5EEDC0FFEEull,
              SEASON_SUMMER, false },
            { "conifer, 80 yr, draft",    "conifer_80",
              TREE_CATEGORY_CONIFER,   80.0f,  QUALITY_DRAFT,    0x5EEDC0FFEEull,
              SEASON_SUMMER, false }
        };
        u32 ci;
        for (ci = 0; ci < TG_COUNTOF(cases); ++ci) {
            if (only != NULL && strcmp(only, cases[ci].prefix) != 0) {
                continue;
            }
            /* Sized so that appending the longest view suffix and ".png" to a
             * full-length prefix cannot truncate. gcc's -Wformat-truncation
             * proves this rather than trusting it. */
            char prefix[256];
            char objpath[320];
            r = build_tree(&b, cases[ci].cat, cases[ci].age, cases[ci].quality,
                           cases[ci].seed, cases[ci].season);
            if (r != TG_OK) {
                printf("\n=== %s ===\n  BUILD FAILED: %s\n", cases[ci].label,
                       tg_result_name(r));
                failures++;
                destroy_tree(&b);
                continue;
            }
            report(&b, cases[ci].label);
            if (!b.valid) { failures++; }

            (void)snprintf(prefix, sizeof prefix, "%s/%s", out_dir,
                           cases[ci].prefix);
            for (vi = 0; vi < TG_COUNTOF(views); ++vi) {
                r = capture(&b, &views[vi], prefix, width, height, rgb, &target);
                if (r != TG_OK) {
                    printf("    capture '%s' failed: %s\n", views[vi].name,
                           tg_result_name(r));
                    failures++;
                }
            }
            if (!cases[ci].want_obj) {
                printf("    OBJ -> skipped (mature tree, see want_obj)\n");
                goto after_obj;
            }
            (void)snprintf(objpath, sizeof objpath, "%s/%s.obj", out_dir,
                           cases[ci].prefix);
            if (write_obj(&b, objpath) != TG_OK) {
                printf("    OBJ write failed\n");
                failures++;
            } else {
                printf("    OBJ -> %s\n", objpath);
            }
        after_obj:
            destroy_tree(&b);
        }
    }

    sw_target_destroy(&target);
    tg_free(rgb, (u64)width * height * 3u);

    printf("\n%s (%d problem%s)\n", failures == 0 ? "ALL CAPTURES OK" : "PROBLEMS",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
