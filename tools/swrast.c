#include "swrast.h"

#include "../src/core/log.h"
#include "../src/core/mem.h"

#include <string.h>

#define SW_SUB "swrast"

/* ------------------------------------------------------------------------- */
/* Target                                                                    */
/* ------------------------------------------------------------------------- */

TgResult sw_target_create(SwTarget *t, u32 width, u32 height) {
    u64 pixels;

    TG_CHECK(t != NULL);
    memset(t, 0, sizeof *t);
    if (width == 0 || height == 0) { return TG_ERR_INVALID_ARGUMENT; }
    if (!tg_ckd_mul_u64(width, height, &pixels)) { return TG_ERR_OVERFLOW; }
    if (!tg_ckd_mul_u64(pixels, 3u * sizeof(f32), &t->colour_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    if (!tg_ckd_mul_u64(pixels, sizeof(f32), &t->depth_bytes)) {
        return TG_ERR_OVERFLOW;
    }
    t->colour = (f32 *)tg_alloc(t->colour_bytes);
    t->depth = (f32 *)tg_alloc(t->depth_bytes);
    if (t->colour == NULL || t->depth == NULL) {
        sw_target_destroy(t);
        return TG_ERR_OUT_OF_MEMORY;
    }
    t->width = width;
    t->height = height;
    /* A neutral studio backdrop with a slight vertical gradient. Deliberately
     * subordinate to the subject and not a photographic environment. */
    t->background_top = v3(0.055f, 0.058f, 0.062f);
    t->background_bottom = v3(0.020f, 0.021f, 0.023f);
    sw_target_clear(t);
    return TG_OK;
}

void sw_target_destroy(SwTarget *t) {
    if (t == NULL) { return; }
    if (t->colour != NULL) { tg_free(t->colour, t->colour_bytes); }
    if (t->depth != NULL) { tg_free(t->depth, t->depth_bytes); }
    memset(t, 0, sizeof *t);
}

void sw_target_clear(SwTarget *t) {
    u32 x, y;
    TG_CHECK(t != NULL);
    for (y = 0; y < t->height; ++y) {
        f32 v = (t->height > 1u) ? (f32)y / (f32)(t->height - 1u) : 0.0f;
        V3 bg = v3_lerp(t->background_top, t->background_bottom, v);
        for (x = 0; x < t->width; ++x) {
            u64 i = ((u64)y * t->width + x) * 3u;
            t->colour[i + 0] = bg.x;
            t->colour[i + 1] = bg.y;
            t->colour[i + 2] = bg.z;
            /* Reverse-Z: cleared to 0, which is the far plane, and the test keeps
             * GREATER values. Identical to the engine's convention on purpose. */
            t->depth[(u64)y * t->width + x] = 0.0f;
        }
    }
    t->triangles_submitted = 0;
    t->triangles_clipped = 0;
    t->triangles_culled = 0;
    t->fragments_written = 0;
}

SwLight sw_default_light(void) {
    SwLight l;
    /* A high side light: reveals form, silhouette and surface relief far better
     * than a head-on light, which flattens everything it touches. */
    l.direction = v3_norm_or(v3(-0.45f, -0.78f, -0.44f), v3(0, -1, 0));
    l.colour = v3(3.05f, 2.92f, 2.68f);
    l.sky_colour = v3(0.34f, 0.40f, 0.50f);
    l.ground_colour = v3(0.14f, 0.12f, 0.09f);
    return l;
}

/* ------------------------------------------------------------------------- */
/* Vertex processing                                                         */
/* ------------------------------------------------------------------------- */

typedef struct SwVert {
    V4 clip;
    f32 sx, sy;    /* screen space pixels                                    */
    f32 inv_w;
    f32 depth;     /* reverse-Z NDC z                                        */
    V3 normal;     /* world                                                  */
    V3 albedo;
    V3 world;
    f32 ao;
} SwVert;

static bool project(const SwTarget *t, M4 vp, const MeshVertex *mv, SwVert *out) {
    V4 c = m4_mul_v4(vp, v4(mv->position.x, mv->position.y, mv->position.z, 1.0f));
    f32 r, g, b, a;

    /* Reject anything at or behind the eye. Full near-plane clipping is not
     * implemented, and saying so matters: a capture taken from INSIDE the crown
     * will drop triangles that straddle the eye plane. Every capture used for
     * judgement is taken from outside, and the dropped count is reported. */
    if (!(c.w > 1e-6f)) { return false; }

    out->clip = c;
    out->inv_w = 1.0f / c.w;
    out->sx = (c.x * out->inv_w * 0.5f + 0.5f) * (f32)t->width;
    /* NDC +Y is up, pixel +Y is down. */
    out->sy = (0.5f - c.y * out->inv_w * 0.5f) * (f32)t->height;
    out->depth = c.z * out->inv_w;
    out->normal = mv->normal;
    out->world = mv->position;
    out->ao = mv->ao;
    mesh_unpack_rgba(mv->color, &r, &g, &b, &a);
    /* Vertex colours are authored in sRGB-ish perceptual terms; lighting has to
     * happen in linear light, so decode first. Skipping this is the classic
     * reason procedural surfaces look chalky. */
    out->albedo = v3(r * r, g * g, b * b);
    return true;
}

static V3 shade(V3 normal, V3 albedo, f32 ao, const SwLight *l, bool two_sided,
                V3 view_dir) {
    V3 n = v3_norm_or(normal, v3(0, 1, 0));
    V3 to_light = v3_neg(l->direction);
    f32 ndl;
    V3 out;

    if (two_sided && v3_dot(n, view_dir) > 0.0f) { n = v3_neg(n); }

    ndl = v3_dot(n, to_light);

    /* Hemispheric ambient: sky from above, bounce from below. Cheap, and enough
     * to keep shadowed interiors readable without pretending to be indirect
     * illumination. It is NOT called global illumination anywhere. */
    {
        f32 up = 0.5f + 0.5f * n.y;
        V3 ambient = v3_lerp(l->ground_colour, l->sky_colour, up);
        out = v3_mul(albedo, v3_scale(ambient, ao));
    }
    if (ndl > 0.0f) {
        out = v3_add(out, v3_mul(albedo, v3_scale(l->colour, ndl)));
        /* A restrained Blinn-Phong highlight. Bark is rough, so the lobe is wide
         * and weak; without any specular at all the wood reads as paper. */
        {
            V3 h = v3_norm_or(v3_sub(to_light, view_dir), n);
            f32 ndh = tg_maxf(v3_dot(n, h), 0.0f);
            f32 spec = powf(ndh, 18.0f) * 0.045f;
            out = v3_add(out, v3_scale(l->colour, spec));
        }
    } else if (two_sided) {
        /* Thin biological tissue transmits. Driven by the geometry's own
         * two-sidedness, never by alpha blending. */
        f32 trans = powf(tg_maxf(-ndl, 0.0f), 1.6f) * 0.30f;
        out = v3_add(out, v3_mul(albedo, v3_scale(l->colour, trans)));
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Triangle rasterisation                                                    */
/* ------------------------------------------------------------------------- */

static f32 edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void raster_triangle(SwTarget *t, const SwVert *v0, const SwVert *v1,
                            const SwVert *v2, const SwLight *l,
                            bool cull_back, bool two_sided, V3 eye) {
    f32 area;
    i32 min_x, max_x, min_y, max_y;
    i32 px, py;
    bool flip = false;

    area = edge(v0->sx, v0->sy, v1->sx, v1->sy, v2->sx, v2->sy);

    /* Screen-space winding. The engine's convention is counter-clockwise front
     * faces seen from outside; after the Y flip into pixel space that becomes a
     * negative signed area. Deriving the sign here rather than guessing means a
     * winding error in the generator shows up as a hole in the capture, which is
     * exactly what a validation renderer is for. */
    if (area > 0.0f) {
        if (cull_back) {
            t->triangles_culled++;
            return;
        }
        flip = true;
    } else if (area < 0.0f) {
        area = -area;
    } else {
        t->triangles_culled++;
        return;
    }
    if (flip) { /* area already positive */ }

    min_x = (i32)floorf(tg_minf(v0->sx, tg_minf(v1->sx, v2->sx)));
    max_x = (i32)ceilf(tg_maxf(v0->sx, tg_maxf(v1->sx, v2->sx)));
    min_y = (i32)floorf(tg_minf(v0->sy, tg_minf(v1->sy, v2->sy)));
    max_y = (i32)ceilf(tg_maxf(v0->sy, tg_maxf(v1->sy, v2->sy)));
    min_x = tg_max_i32(min_x, 0);
    min_y = tg_max_i32(min_y, 0);
    max_x = tg_min_i32(max_x, (i32)t->width - 1);
    max_y = tg_min_i32(max_y, (i32)t->height - 1);
    if (min_x > max_x || min_y > max_y) { return; }
    if (!(area > 1e-9f)) { return; }

    for (py = min_y; py <= max_y; ++py) {
        for (px = min_x; px <= max_x; ++px) {
            f32 cx = (f32)px + 0.5f;
            f32 cy = (f32)py + 0.5f;
            f32 w0 = edge(v1->sx, v1->sy, v2->sx, v2->sy, cx, cy);
            f32 w1 = edge(v2->sx, v2->sy, v0->sx, v0->sy, cx, cy);
            f32 w2 = edge(v0->sx, v0->sy, v1->sx, v1->sy, cx, cy);
            f32 b0, b1, b2, depth;
            u64 di;

            if (flip) { w0 = -w0; w1 = -w1; w2 = -w2; }
            if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f) { continue; }
            b0 = -w0 / area;
            b1 = -w1 / area;
            b2 = -w2 / area;

            depth = b0 * v0->depth + b1 * v1->depth + b2 * v2->depth;
            di = (u64)py * t->width + (u64)px;
            /* Reverse-Z: keep the GREATER value. */
            if (depth <= t->depth[di]) { continue; }

            {
                /* Perspective-correct attribute interpolation. Interpolating in
                 * screen space instead would visibly warp shading across the
                 * large near-field triangles of a trunk seen up close. */
                f32 iw = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
                f32 s0, s1, s2;
                V3 n, albedo, world, colour;
                f32 ao;
                if (!(iw > 1e-12f)) { continue; }
                s0 = b0 * v0->inv_w / iw;
                s1 = b1 * v1->inv_w / iw;
                s2 = b2 * v2->inv_w / iw;
                n = v3_add(v3_add(v3_scale(v0->normal, s0),
                                  v3_scale(v1->normal, s1)),
                           v3_scale(v2->normal, s2));
                albedo = v3_add(v3_add(v3_scale(v0->albedo, s0),
                                       v3_scale(v1->albedo, s1)),
                                v3_scale(v2->albedo, s2));
                world = v3_add(v3_add(v3_scale(v0->world, s0),
                                      v3_scale(v1->world, s1)),
                               v3_scale(v2->world, s2));
                ao = s0 * v0->ao + s1 * v1->ao + s2 * v2->ao;
                colour = shade(n, albedo, ao, l, two_sided,
                               v3_norm_or(v3_sub(world, eye), v3(0, 0, -1)));
                t->depth[di] = depth;
                t->colour[di * 3 + 0] = colour.x;
                t->colour[di * 3 + 1] = colour.y;
                t->colour[di * 3 + 2] = colour.z;
                t->fragments_written++;
            }
        }
    }
}

TgResult sw_draw_mesh_section(SwTarget *t, const Camera *cam, const Mesh *m,
                              MeshSection section, const SwLight *light) {
    const MeshSectionInfo *info;
    const MeshSectionSpan *sp;
    const u32 *ind;
    const MeshVertex *verts;
    M4 vp;
    V3 eye;
    u32 i;

    TG_CHECK(t != NULL && cam != NULL && m != NULL);
    if ((u32)section >= (u32)MESH_SECTION_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (!m->section_used[section]) { return TG_OK; }
    info = mesh_section_info(section);
    sp = &m->span[section];
    ind = mesh_indices(m);
    verts = mesh_vertices(m);
    vp = camera_view_projection(cam);
    eye = camera_position(cam);

    for (i = 0; i < sp->triangle_count; ++i) {
        u32 base = sp->first_index + i * 3u;
        SwVert a, b, c;
        t->triangles_submitted++;
        if (!project(t, vp, &verts[ind[base + 0]], &a) ||
            !project(t, vp, &verts[ind[base + 1]], &b) ||
            !project(t, vp, &verts[ind[base + 2]], &c)) {
            t->triangles_clipped++;
            continue;
        }
        raster_triangle(t, &a, &b, &c, light, info->backface_cull,
                        info->two_sided, eye);
    }
    return TG_OK;
}

TgResult sw_draw_mesh(SwTarget *t, const Camera *cam, const Mesh *m,
                      const SwLight *light) {
    u32 s;
    TgResult r;
    for (s = 0; s < MESH_SECTION_COUNT; ++s) {
        r = sw_draw_mesh_section(t, cam, m, (MeshSection)s, light);
        if (r != TG_OK) { return r; }
    }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Lines and points                                                          */
/* ------------------------------------------------------------------------- */

static bool project_point(const SwTarget *t, M4 vp, V3 p, f32 *out_x, f32 *out_y,
                          f32 *out_depth) {
    V4 c = m4_mul_v4(vp, v4(p.x, p.y, p.z, 1.0f));
    f32 iw;
    if (!(c.w > 1e-6f)) { return false; }
    iw = 1.0f / c.w;
    *out_x = (c.x * iw * 0.5f + 0.5f) * (f32)t->width;
    *out_y = (0.5f - c.y * iw * 0.5f) * (f32)t->height;
    *out_depth = c.z * iw;
    return true;
}

static void plot(SwTarget *t, i32 x, i32 y, f32 depth, V3 colour, f32 depth_bias) {
    u64 di;
    if (x < 0 || y < 0 || x >= (i32)t->width || y >= (i32)t->height) { return; }
    di = (u64)y * t->width + (u64)x;
    /* Reverse-Z: a positive bias pulls the fragment toward the camera. */
    if (depth + depth_bias < t->depth[di]) { return; }
    t->depth[di] = depth + depth_bias;
    t->colour[di * 3 + 0] = colour.x;
    t->colour[di * 3 + 1] = colour.y;
    t->colour[di * 3 + 2] = colour.z;
    t->fragments_written++;
}

void sw_draw_line(SwTarget *t, const Camera *cam, V3 a, V3 b, V3 colour) {
    M4 vp;
    f32 ax, ay, ad, bx, by, bd;
    f32 dx, dy, steps;
    i32 i, n;

    TG_CHECK(t != NULL && cam != NULL);
    vp = camera_view_projection(cam);
    if (!project_point(t, vp, a, &ax, &ay, &ad)) { return; }
    if (!project_point(t, vp, b, &bx, &by, &bd)) { return; }

    dx = bx - ax;
    dy = by - ay;
    steps = tg_maxf(tg_absf(dx), tg_absf(dy));
    if (!(steps > 0.0f)) {
        plot(t, (i32)ax, (i32)ay, ad, colour, 1e-4f);
        return;
    }
    /* Bounded so a line that projects across a huge extent cannot stall the
     * capture; the visible portion is what matters. */
    n = (i32)tg_minf(steps, 8192.0f);
    for (i = 0; i <= n; ++i) {
        f32 s = (f32)i / (f32)n;
        plot(t, (i32)(ax + dx * s), (i32)(ay + dy * s),
             tg_lerpf(ad, bd, s), colour, 1e-4f);
    }
}

void sw_draw_point(SwTarget *t, const Camera *cam, V3 p, f32 radius_px,
                   V3 colour) {
    M4 vp;
    f32 sx, sy, sd;
    i32 r, x, y;

    TG_CHECK(t != NULL && cam != NULL);
    vp = camera_view_projection(cam);
    if (!project_point(t, vp, p, &sx, &sy, &sd)) { return; }
    r = (i32)tg_maxf(radius_px, 0.5f);
    for (y = -r; y <= r; ++y) {
        for (x = -r; x <= r; ++x) {
            if (x * x + y * y > r * r) { continue; }
            plot(t, (i32)sx + x, (i32)sy + y, sd, colour, 2e-4f);
        }
    }
}

TgResult sw_draw_wireframe(SwTarget *t, const Camera *cam, const Mesh *m,
                           MeshSection section, V3 colour) {
    const MeshSectionSpan *sp;
    const u32 *ind;
    const MeshVertex *verts;
    u32 i;

    TG_CHECK(t != NULL && cam != NULL && m != NULL);
    if ((u32)section >= (u32)MESH_SECTION_COUNT) { return TG_ERR_INVALID_ARGUMENT; }
    if (!m->section_used[section]) { return TG_OK; }
    sp = &m->span[section];
    ind = mesh_indices(m);
    verts = mesh_vertices(m);

    for (i = 0; i < sp->triangle_count; ++i) {
        u32 base = sp->first_index + i * 3u;
        V3 p0 = verts[ind[base + 0]].position;
        V3 p1 = verts[ind[base + 1]].position;
        V3 p2 = verts[ind[base + 2]].position;
        sw_draw_line(t, cam, p0, p1, colour);
        sw_draw_line(t, cam, p1, p2, colour);
        sw_draw_line(t, cam, p2, p0, colour);
    }
    return TG_OK;
}

/* ------------------------------------------------------------------------- */
/* Resolve                                                                   */
/* ------------------------------------------------------------------------- */

static f32 linear_to_srgb(f32 v) {
    /* The actual sRGB transfer function, not an approximate gamma of 2.2. Using
     * the approximation visibly lifts the deep shadows inside a crown, which is
     * exactly the region whose readability is being judged. */
    v = tg_saturatef(v);
    if (v <= 0.0031308f) { return v * 12.92f; }
    return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

TgResult sw_resolve_srgb(const SwTarget *t, f32 exposure, u8 *out_rgb) {
    u64 i, n;

    TG_CHECK(t != NULL);
    if (out_rgb == NULL) { return TG_ERR_INVALID_ARGUMENT; }
    if (!(exposure > 0.0f)) { exposure = 1.0f; }
    n = (u64)t->width * t->height;
    for (i = 0; i < n; ++i) {
        u32 k;
        for (k = 0; k < 3; ++k) {
            f32 v = t->colour[i * 3 + k] * exposure;
            if (!tg_finitef(v) || v < 0.0f) { v = 0.0f; }
            /* Reinhard. Named for what it is; no claim of a film response. */
            v = v / (1.0f + v);
            out_rgb[i * 3 + k] = (u8)(linear_to_srgb(v) * 255.0f + 0.5f);
        }
    }
    return TG_OK;
}

TgResult sw_resolve_depth(const SwTarget *t, u8 *out_rgb) {
    u64 i, n;
    f32 mn = 3.402823466e38f, mx = -3.402823466e38f;

    TG_CHECK(t != NULL);
    if (out_rgb == NULL) { return TG_ERR_INVALID_ARGUMENT; }
    n = (u64)t->width * t->height;
    /* Auto-range over the occupied depths only: reverse-Z packs everything into a
     * narrow band near 1 for a close subject, and a fixed 0..1 mapping would
     * render a featureless white silhouette that diagnoses nothing. */
    for (i = 0; i < n; ++i) {
        f32 d = t->depth[i];
        if (d <= 0.0f) { continue; }
        if (d < mn) { mn = d; }
        if (d > mx) { mx = d; }
    }
    for (i = 0; i < n; ++i) {
        f32 d = t->depth[i];
        u8 v;
        if (d <= 0.0f) {
            v = 0;
        } else if (mx - mn < 1e-9f) {
            v = 200;
        } else {
            v = (u8)(tg_saturatef((d - mn) / (mx - mn)) * 235.0f + 20.0f);
        }
        out_rgb[i * 3 + 0] = v;
        out_rgb[i * 3 + 1] = v;
        out_rgb[i * 3 + 2] = v;
    }
    return TG_OK;
}
