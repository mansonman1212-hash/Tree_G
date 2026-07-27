#include "camera.h"

#include <string.h>

void camera_init(Camera *c) {
    TG_CHECK(c != NULL);
    memset(c, 0, sizeof *c);
    c->mode = CAMERA_MODE_ORBIT;
    c->projection = CAMERA_PROJ_PERSPECTIVE;
    c->pivot = v3_zero();
    c->distance = 5.0f;
    c->yaw = 0.0f;
    c->pitch = -0.25f;              /* slightly above the horizon */
    c->free_position = v3(0.0f, 1.6f, 5.0f);
    c->fov_y = CAMERA_FOV_DEFAULT_DEG * TG_DEG2RAD_F;
    c->near_z = 0.02f;
    c->far_z = 1000.0f;
    c->use_infinite_far = true;
    c->ortho_half_height = 2.0f;
    c->scene_scale = 1.0f;
    c->move_speed_scale = 1.0f;
    c->look_sensitivity = 0.005f;
    c->aspect = 16.0f / 9.0f;
}

void camera_set_scene_bounds(Camera *c, Aabb bounds) {
    f32 diag;
    TG_CHECK(c != NULL);
    diag = aabb_diagonal(bounds);
    if (!(diag > 0.0f) || !tg_finitef(diag)) { diag = 1.0f; }
    c->scene_scale = diag;
    /* Near plane proportional to scale: 1/2000 of the scene diagonal. For a 25 m
     * tree that is 12.5 mm, close enough to press against bark without clipping,
     * and far enough that reverse-Z precision stays excellent. A fixed near
     * plane would either clip a seedling or waste precision on a large tree. */
    c->near_z = tg_clampf(diag * 0.0005f, 0.001f, 1.0f);
    c->ortho_half_height = diag * 0.6f;
}

void camera_set_aspect(Camera *c, u32 pixel_width, u32 pixel_height) {
    TG_CHECK(c != NULL);
    if (pixel_width == 0 || pixel_height == 0) {
        /* A minimised window reports zero. Keeping the last valid aspect avoids
         * a divide-by-zero and avoids a visible jump on restore. */
        return;
    }
    c->aspect = (f32)pixel_width / (f32)pixel_height;
}

f32 camera_set_fov_degrees(Camera *c, f32 degrees) {
    TG_CHECK(c != NULL);
    if (!tg_finitef(degrees)) { degrees = CAMERA_FOV_DEFAULT_DEG; }
    degrees = tg_clampf(degrees, CAMERA_FOV_MIN_DEG, CAMERA_FOV_MAX_DEG);
    c->fov_y = degrees * TG_DEG2RAD_F;
    return degrees;
}

/* Orientation is rebuilt from yaw/pitch every time rather than integrated, so no
 * drift can accumulate over a long inspection session. */
static Quat orientation(const Camera *c) {
    Quat qy = quat_from_axis_angle(v3(0.0f, 1.0f, 0.0f), c->yaw);
    Quat qp = quat_from_axis_angle(v3(1.0f, 0.0f, 0.0f), c->pitch);
    return quat_mul(qy, qp);
}

V3 camera_forward(const Camera *c) {
    TG_CHECK(c != NULL);
    return v3_norm_or(quat_rotate(orientation(c), v3(0.0f, 0.0f, -1.0f)),
                      v3(0.0f, 0.0f, -1.0f));
}

V3 camera_right(const Camera *c) {
    TG_CHECK(c != NULL);
    return v3_norm_or(quat_rotate(orientation(c), v3(1.0f, 0.0f, 0.0f)),
                      v3(1.0f, 0.0f, 0.0f));
}

V3 camera_up(const Camera *c) {
    TG_CHECK(c != NULL);
    return v3_norm_or(quat_rotate(orientation(c), v3(0.0f, 1.0f, 0.0f)),
                      v3(0.0f, 1.0f, 0.0f));
}

V3 camera_position(const Camera *c) {
    TG_CHECK(c != NULL);
    if (c->mode == CAMERA_MODE_FREE) { return c->free_position; }
    return v3_sub(c->pivot, v3_scale(camera_forward(c), c->distance));
}

M4 camera_view_matrix(const Camera *c) {
    V3 eye, fwd;
    TG_CHECK(c != NULL);
    eye = camera_position(c);
    fwd = camera_forward(c);
    return m4_look_at_rh(eye, v3_add(eye, fwd), v3(0.0f, 1.0f, 0.0f));
}

M4 camera_projection_matrix(const Camera *c) {
    TG_CHECK(c != NULL);
    if (c->projection == CAMERA_PROJ_ORTHOGRAPHIC) {
        f32 hh = tg_maxf(c->ortho_half_height, 1e-4f);
        f32 hw = hh * tg_maxf(c->aspect, 1e-4f);
        /* Orthographic needs a finite far plane; size it from the scene so the
         * whole object always fits regardless of camera distance. */
        f32 far_z = tg_maxf(c->distance + c->scene_scale * 4.0f,
                            c->near_z + 1.0f);
        return m4_ortho_reverse_z(hw, hh, c->near_z, far_z);
    }
    if (c->use_infinite_far) {
        return m4_perspective_reverse_z_infinite(c->fov_y, c->aspect, c->near_z);
    }
    return m4_perspective_reverse_z(c->fov_y, c->aspect, c->near_z,
                                    tg_maxf(c->far_z, c->near_z * 2.0f));
}

M4 camera_view_projection(const Camera *c) {
    return m4_mul(camera_projection_matrix(c), camera_view_matrix(c));
}

/* ------------------------------------------------------------------------- */
/* Interaction                                                               */
/* ------------------------------------------------------------------------- */

void camera_orbit(Camera *c, f32 dx, f32 dy) {
    TG_CHECK(c != NULL);
    if (!tg_finitef(dx) || !tg_finitef(dy)) { return; }
    c->yaw = tg_wrap_pi(c->yaw - dx * c->look_sensitivity);
    c->pitch = tg_clampf(c->pitch - dy * c->look_sensitivity,
                         -CAMERA_PITCH_LIMIT, CAMERA_PITCH_LIMIT);
}

void camera_pan(Camera *c, f32 dx, f32 dy) {
    f32 scale;
    V3 offset;

    TG_CHECK(c != NULL);
    if (!tg_finitef(dx) || !tg_finitef(dy)) { return; }

    /* Screen-space pan must be proportional to the world size of a pixel at the
     * pivot depth, otherwise panning is uselessly slow when zoomed out and wild
     * when zoomed in. For a perspective camera that world size is
     * 2*d*tan(fov/2) per viewport height. */
    if (c->projection == CAMERA_PROJ_ORTHOGRAPHIC) {
        scale = c->ortho_half_height * 2.0f * 0.001f;
    } else {
        scale = 2.0f * tg_maxf(c->distance, c->near_z)
              * tanf(c->fov_y * 0.5f) * 0.001f;
    }
    offset = v3_add(v3_scale(camera_right(c), -dx * scale),
                    v3_scale(camera_up(c), dy * scale));
    c->pivot = v3_add(c->pivot, offset);
    if (c->mode == CAMERA_MODE_FREE) {
        c->free_position = v3_add(c->free_position, offset);
    }
}

void camera_dolly(Camera *c, f32 steps) {
    f32 factor;
    f32 min_d, max_d;

    TG_CHECK(c != NULL);
    if (!tg_finitef(steps) || steps == 0.0f) { return; }

    /* Multiplicative so each notch changes the view by the same proportion at
     * every scale. 1.12 per notch is about 8 notches per doubling. */
    factor = powf(1.12f, -steps);
    if (!tg_finitef(factor) || factor <= 0.0f) { return; }

    if (c->projection == CAMERA_PROJ_ORTHOGRAPHIC) {
        c->ortho_half_height = tg_clampf(c->ortho_half_height * factor,
                                         c->scene_scale * 0.0005f,
                                         c->scene_scale * 20.0f);
        return;
    }

    /* Limits are derived from the scene, not hard-coded. The lower bound is
     * deliberately tiny (1/5000 of the scene diagonal) because the project
     * requires that the user be able to approach the surface closely; a fixed
     * minimum distance would make bark inspection impossible. */
    min_d = tg_maxf(c->scene_scale * 0.0002f, c->near_z * 1.5f);
    max_d = c->scene_scale * 40.0f;
    c->distance = tg_clampf(c->distance * factor, min_d, max_d);

    if (c->mode == CAMERA_MODE_FREE) {
        c->free_position = v3_add(c->free_position,
                                  v3_scale(camera_forward(c),
                                           c->scene_scale * 0.02f * steps));
    }
}

void camera_move(Camera *c, V3 local_dir, f32 dt, bool fast) {
    f32 speed;
    V3 world;

    TG_CHECK(c != NULL);
    if (!tg_finitef(dt) || dt <= 0.0f) { return; }
    if (!v3_finite(local_dir) || v3_len_sq(local_dir) < TG_TINY_F) { return; }
    local_dir = v3_norm_or(local_dir, v3_zero());

    /* Base speed traverses the scene in about four seconds. Scale-adaptive, so
     * the same key feels right on a seedling and on a mature tree. */
    speed = c->scene_scale * 0.25f * c->move_speed_scale * (fast ? 4.0f : 1.0f);

    world = v3_add(v3_add(v3_scale(camera_right(c), local_dir.x),
                          v3_scale(camera_up(c), local_dir.y)),
                   v3_scale(camera_forward(c), -local_dir.z));
    world = v3_scale(world, speed * dt);

    c->free_position = v3_add(c->free_position, world);
    /* Keep the orbit pivot ahead of the camera so that switching back to orbit
     * mode does not snap the view to a stale pivot. */
    c->pivot = v3_add(c->free_position,
                      v3_scale(camera_forward(c), c->distance));
}

/* ------------------------------------------------------------------------- */
/* Framing                                                                   */
/* ------------------------------------------------------------------------- */

/* Distance at which a sphere of `radius` exactly fills the smaller of the two
 * frustum half-angles.
 *
 * Using the bounding SPHERE rather than fitting the eight box corners
 * individually is deliberate: it is rotation independent, so framing gives the
 * same distance from every viewing angle instead of breathing as the user
 * orbits. */
static f32 distance_for_radius(const Camera *c, f32 radius, f32 margin) {
    f32 half_v = c->fov_y * 0.5f;
    f32 half_h = atanf(tanf(half_v) * tg_maxf(c->aspect, 1e-4f));
    f32 limiting = tg_minf(half_v, half_h);
    f32 s = sinf(limiting);
    if (!(s > 1e-4f)) { s = 1e-4f; }
    return radius * (1.0f + tg_maxf(margin, 0.0f)) / s;
}

void camera_frame_bounds(Camera *c, Aabb bounds, f32 margin) {
    V3 center;
    f32 radius;

    TG_CHECK(c != NULL);
    if (aabb_is_empty(bounds)) { return; }

    center = aabb_center(bounds);
    radius = aabb_diagonal(bounds) * 0.5f;
    if (!(radius > 0.0f)) { radius = 1.0f; }

    camera_set_scene_bounds(c, bounds);
    c->pivot = center;

    if (c->projection == CAMERA_PROJ_ORTHOGRAPHIC) {
        c->ortho_half_height = radius * (1.0f + tg_maxf(margin, 0.0f));
        c->distance = radius * 3.0f;
    } else {
        c->distance = distance_for_radius(c, radius, margin);
    }
    if (c->mode == CAMERA_MODE_FREE) {
        c->free_position = v3_sub(c->pivot,
                                  v3_scale(camera_forward(c), c->distance));
    }
}

void camera_frame_bounds_reset_angle(Camera *c, Aabb bounds, f32 margin,
                                     f32 yaw, f32 pitch) {
    TG_CHECK(c != NULL);
    c->yaw = tg_wrap_pi(yaw);
    c->pitch = tg_clampf(pitch, -CAMERA_PITCH_LIMIT, CAMERA_PITCH_LIMIT);
    camera_frame_bounds(c, bounds, margin);
}

void camera_reset(Camera *c, Aabb bounds) {
    TG_CHECK(c != NULL);
    c->mode = CAMERA_MODE_ORBIT;
    c->projection = CAMERA_PROJ_PERSPECTIVE;
    c->fov_y = CAMERA_FOV_DEFAULT_DEG * TG_DEG2RAD_F;
    c->use_infinite_far = true;
    c->move_speed_scale = 1.0f;
    /* Three-quarter view slightly above the horizon: shows trunk taper, crown
     * asymmetry and root flare at once. */
    camera_frame_bounds_reset_angle(c, bounds, 0.08f, -0.6f, -0.12f);
}

void camera_set_mode(Camera *c, CameraMode mode) {
    TG_CHECK(c != NULL);
    if (mode == c->mode || (u32)mode >= (u32)CAMERA_MODE_COUNT) { return; }
    if (mode == CAMERA_MODE_FREE) {
        /* Adopt the orbit eye position so the view does not jump. */
        c->free_position = camera_position(c);
    } else {
        /* Put the pivot where the free camera was looking, at the same distance,
         * so orbit continues around what the user was inspecting. */
        c->pivot = v3_add(c->free_position,
                          v3_scale(camera_forward(c), c->distance));
    }
    c->mode = mode;
}

Ray camera_pick_ray(const Camera *c, f32 px, f32 py, u32 width, u32 height) {
    Ray r;
    f32 ndc_x, ndc_y;
    V3 eye, dir;

    TG_CHECK(c != NULL);
    r.origin = camera_position(c);
    r.dir = camera_forward(c);
    r.t_min = 0.0f;
    r.t_max = 3.402823466e38f;
    if (width == 0 || height == 0) { return r; }

    /* Pixel space has its origin at the top-left, NDC has +Y up, hence the
     * negation on Y. Getting this wrong inverts picking vertically, which is
     * easy to miss on a roughly symmetric object like a tree. */
    ndc_x = (2.0f * px / (f32)width) - 1.0f;
    ndc_y = 1.0f - (2.0f * py / (f32)height);

    eye = camera_position(c);

    if (c->projection == CAMERA_PROJ_ORTHOGRAPHIC) {
        f32 hh = c->ortho_half_height;
        f32 hw = hh * c->aspect;
        r.origin = v3_add(eye, v3_add(v3_scale(camera_right(c), ndc_x * hw),
                                      v3_scale(camera_up(c), ndc_y * hh)));
        r.dir = camera_forward(c);
        return r;
    }

    {
        f32 ty = tanf(c->fov_y * 0.5f);
        f32 tx = ty * c->aspect;
        dir = v3_add(camera_forward(c),
                     v3_add(v3_scale(camera_right(c), ndc_x * tx),
                            v3_scale(camera_up(c), ndc_y * ty)));
        r.dir = v3_norm_or(dir, camera_forward(c));
    }
    return r;
}

bool camera_bounds_fully_visible(const Camera *c, Aabb bounds) {
    M4 vp;
    int i;

    TG_CHECK(c != NULL);
    if (aabb_is_empty(bounds)) { return true; }
    vp = camera_view_projection(c);

    for (i = 0; i < 8; ++i) {
        V3 p = v3(((i & 1) ? bounds.mx : bounds.mn).x,
                  ((i & 2) ? bounds.mx : bounds.mn).y,
                  ((i & 4) ? bounds.mx : bounds.mn).z);
        V4 clip = m4_mul_v4(vp, v4(p.x, p.y, p.z, 1.0f));
        if (c->projection == CAMERA_PROJ_PERSPECTIVE) {
            if (!(clip.w > 0.0f)) { return false; } /* behind the eye */
        }
        if (clip.x < -clip.w || clip.x > clip.w) { return false; }
        if (clip.y < -clip.w || clip.y > clip.w) { return false; }
        /* Reverse-Z: visible depth is in [0, w], with w mapping to the near
         * plane. A point nearer than the near plane has clip.z > clip.w. */
        if (clip.z < 0.0f || clip.z > clip.w) { return false; }
    }
    return true;
}
