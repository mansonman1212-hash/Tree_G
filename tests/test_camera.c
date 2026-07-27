#include "test_util.h"
#include "test_suites.h"

#include "../src/core/rng.h"
#include "../src/geom/camera.h"

/* A tree-shaped bounding box: 24 m tall, 18 m crown spread, base at y = -2 to
 * include exposed roots. Used everywhere so the numbers are meaningful. */
static Aabb tree_bounds(void) {
    Aabb b = aabb_empty();
    b = aabb_add_point(b, v3(-9.0f, -2.0f, -9.0f));
    b = aabb_add_point(b, v3(9.0f, 24.0f, 9.0f));
    return b;
}

static void test_defaults(void) {
    Camera c;

    TG_T_CASE("defaults are inside the mandated restrained FOV range");
    camera_init(&c);
    TG_EXPECT_NEAR(c.fov_y * TG_RAD2DEG_F, CAMERA_FOV_DEFAULT_DEG, 1e-4);
    TG_EXPECT(c.fov_y * TG_RAD2DEG_F >= 35.0f);
    TG_EXPECT(c.fov_y * TG_RAD2DEG_F <= 50.0f);
    TG_EXPECT(c.use_infinite_far);
    TG_EXPECT_EQ_U64(c.projection, CAMERA_PROJ_PERSPECTIVE);

    TG_T_CASE("matrices are valid before any bounds are supplied");
    {
        M4 vp = camera_view_projection(&c);
        int i;
        for (i = 0; i < 16; ++i) { TG_EXPECT(tg_finitef(vp.m[i])); }
    }

    TG_T_CASE("FOV is clamped, and the applied value is reported back");
    TG_EXPECT_NEAR(camera_set_fov_degrees(&c, 500.0f), CAMERA_FOV_MAX_DEG, 1e-4);
    TG_EXPECT_NEAR(c.fov_y * TG_RAD2DEG_F, CAMERA_FOV_MAX_DEG, 1e-4);
    TG_EXPECT_NEAR(camera_set_fov_degrees(&c, 1.0f), CAMERA_FOV_MIN_DEG, 1e-4);
    TG_EXPECT_NEAR(camera_set_fov_degrees(&c, 45.0f), 45.0f, 1e-4);
    TG_T_CASE("a non-finite FOV falls back to the default rather than propagating");
    {
        volatile f32 zero = 0.0f;
        TG_EXPECT_NEAR(camera_set_fov_degrees(&c, 0.0f / zero),
                       CAMERA_FOV_DEFAULT_DEG, 1e-4);
    }
}

static void test_basis(void) {
    Camera c;
    TgRng r = tg_rng_substream(11, TG_RNG_GLOBAL, 0, 0);
    u32 i;

    TG_T_CASE("the camera basis stays orthonormal and right-handed for all angles");
    camera_init(&c);
    for (i = 0; i < 500; ++i) {
        V3 f, rt, up;
        c.yaw = tg_rng_range(&r, -4.0f, 4.0f);
        c.pitch = tg_clampf(tg_rng_range(&r, -2.0f, 2.0f),
                            -CAMERA_PITCH_LIMIT, CAMERA_PITCH_LIMIT);
        f = camera_forward(&c);
        rt = camera_right(&c);
        up = camera_up(&c);
        TG_EXPECT_NEAR(v3_len(f), 1.0f, 1e-4);
        TG_EXPECT_NEAR(v3_len(rt), 1.0f, 1e-4);
        TG_EXPECT_NEAR(v3_len(up), 1.0f, 1e-4);
        TG_EXPECT_NEAR(v3_dot(f, rt), 0.0f, 1e-4);
        TG_EXPECT_NEAR(v3_dot(f, up), 0.0f, 1e-4);
        TG_EXPECT_NEAR(v3_dot(rt, up), 0.0f, 1e-4);
        /* Right-handed: right x up = -forward for a -Z-forward camera. */
        TG_EXPECT_V3_NEAR(v3_cross(rt, up), v3_neg(f), 1e-3);
    }

    TG_T_CASE("zero yaw and pitch looks along -Z with world up preserved");
    c.yaw = 0.0f;
    c.pitch = 0.0f;
    TG_EXPECT_V3_NEAR(camera_forward(&c), v3(0, 0, -1), 1e-5);
    TG_EXPECT_V3_NEAR(camera_up(&c), v3(0, 1, 0), 1e-5);
    TG_EXPECT_V3_NEAR(camera_right(&c), v3(1, 0, 0), 1e-5);
}

static void test_pitch_clamp_no_flip(void) {
    Camera c;
    u32 i;
    f32 prev_y;

    TG_T_CASE("pitch clamps at the poles and the up vector never flips");
    /* A gimbal flip is the classic orbit-camera defect: the view snaps upside
     * down as the user drags past vertical. */
    camera_init(&c);
    camera_set_scene_bounds(&c, tree_bounds());
    prev_y = camera_up(&c).y;
    for (i = 0; i < 400; ++i) {
        camera_orbit(&c, 0.0f, -50.0f); /* drag up, hard, repeatedly */
        TG_EXPECT(c.pitch <= CAMERA_PITCH_LIMIT + 1e-6f);
        TG_EXPECT_MSG(camera_up(&c).y > 0.0f,
                      "up vector flipped at iteration %u (pitch %.4f)",
                      i, (double)c.pitch);
        prev_y = camera_up(&c).y;
    }
    TG_EXPECT_NEAR(c.pitch, CAMERA_PITCH_LIMIT, 1e-5);
    for (i = 0; i < 800; ++i) {
        camera_orbit(&c, 0.0f, 50.0f); /* and all the way back down */
        TG_EXPECT(c.pitch >= -CAMERA_PITCH_LIMIT - 1e-6f);
        TG_EXPECT(camera_up(&c).y > 0.0f);
    }
    TG_EXPECT_NEAR(c.pitch, -CAMERA_PITCH_LIMIT, 1e-5);
    TG_UNUSED(prev_y);

    TG_T_CASE("yaw wraps rather than growing without bound over a long session");
    for (i = 0; i < 5000; ++i) { camera_orbit(&c, 100.0f, 0.0f); }
    TG_EXPECT(c.yaw >= -TG_PI_F - 1e-3f && c.yaw <= TG_PI_F + 1e-3f);

    TG_T_CASE("a non-finite input is ignored rather than poisoning the state");
    {
        volatile f32 zero = 0.0f;
        f32 yaw_before = c.yaw, pitch_before = c.pitch;
        camera_orbit(&c, 1.0f / zero, 0.0f);
        camera_orbit(&c, 0.0f, 0.0f / zero);
        TG_EXPECT_NEAR(c.yaw, yaw_before, 0.0);
        TG_EXPECT_NEAR(c.pitch, pitch_before, 0.0);
    }
}

static void test_orbit_keeps_pivot(void) {
    Camera c;
    TgRng r = tg_rng_substream(13, TG_RNG_GLOBAL, 1, 0);
    u32 i;
    V3 pivot0;
    f32 dist0;

    TG_T_CASE("orbiting changes only the viewpoint, never the pivot or distance");
    /* This is the camera-side half of "the user moves the camera, not the tree". */
    camera_init(&c);
    camera_frame_bounds(&c, tree_bounds(), 0.08f);
    pivot0 = c.pivot;
    dist0 = c.distance;
    for (i = 0; i < 2000; ++i) {
        camera_orbit(&c, tg_rng_range(&r, -200.0f, 200.0f),
                     tg_rng_range(&r, -200.0f, 200.0f));
        TG_EXPECT_V3_NEAR(c.pivot, pivot0, 0.0);
        TG_EXPECT_NEAR(c.distance, dist0, 0.0);
    }

    TG_T_CASE("the eye always sits exactly `distance` from the pivot");
    for (i = 0; i < 500; ++i) {
        camera_orbit(&c, tg_rng_range(&r, -100.0f, 100.0f),
                     tg_rng_range(&r, -100.0f, 100.0f));
        TG_EXPECT_NEAR(v3_dist(camera_position(&c), c.pivot), c.distance, 1e-3);
    }
}

static void test_framing(void) {
    Camera c;
    Aabb b = tree_bounds();
    TgRng r = tg_rng_substream(17, TG_RNG_GLOBAL, 2, 0);
    u32 i;

    TG_T_CASE("framing makes the whole object visible from any viewing angle");
    /* Uses the bounding sphere precisely so that framing is rotation
     * independent -- otherwise the framed distance would change as the user
     * orbits, which reads as the object breathing. */
    camera_init(&c);
    camera_set_aspect(&c, 1920, 1080);
    for (i = 0; i < 300; ++i) {
        c.yaw = tg_rng_range(&r, -TG_PI_F, TG_PI_F);
        c.pitch = tg_rng_range(&r, -CAMERA_PITCH_LIMIT, CAMERA_PITCH_LIMIT);
        camera_frame_bounds(&c, b, 0.08f);
        TG_EXPECT_MSG(camera_bounds_fully_visible(&c, b),
                      "bounds not fully visible at yaw %.3f pitch %.3f",
                      (double)c.yaw, (double)c.pitch);
    }

    /* Anti-vacuity: the visibility predicate above is the basis of every framing
     * assertion, so prove it can return false. A predicate that always returns
     * true would make the whole framing section meaningless. */
    TG_T_CASE("the visibility predicate genuinely rejects: zoomed in, behind, aside");
    {
        camera_frame_bounds_reset_angle(&c, b, 0.08f, 0.0f, 0.0f);
        TG_EXPECT(camera_bounds_fully_visible(&c, b));
        /* Too close. */
        c.distance *= 0.2f;
        TG_EXPECT_MSG(!camera_bounds_fully_visible(&c, b),
                      "predicate accepted a view far too close to contain the box");
        /* Object entirely behind the camera. */
        camera_frame_bounds_reset_angle(&c, b, 0.0f, 0.08f, 0.0f);
        c.distance = -c.distance;
        TG_EXPECT(!camera_bounds_fully_visible(&c, b));
        /* Aimed off to one side. */
        camera_frame_bounds_reset_angle(&c, b, 0.08f, 0.0f, 0.0f);
        c.pivot = v3_add(c.pivot, v3(1000.0f, 0.0f, 0.0f));
        TG_EXPECT(!camera_bounds_fully_visible(&c, b));
        camera_frame_bounds_reset_angle(&c, b, 0.08f, 0.0f, 0.0f);
    }

    TG_T_CASE("framed distance is independent of the viewing angle");
    {
        f32 d0;
        c.yaw = 0.0f; c.pitch = 0.0f;
        camera_frame_bounds(&c, b, 0.08f);
        d0 = c.distance;
        for (i = 0; i < 50; ++i) {
            c.yaw = tg_rng_range(&r, -TG_PI_F, TG_PI_F);
            c.pitch = tg_rng_range(&r, -1.0f, 1.0f);
            camera_frame_bounds(&c, b, 0.08f);
            TG_EXPECT_NEAR(c.distance, d0, 1e-3);
        }
    }

    TG_T_CASE("framing works at extreme aspect ratios");
    {
        u32 widths[5] = { 320, 800, 1920, 3840, 400 };
        u32 heights[5] = { 1200, 600, 1080, 1080, 2000 };
        u32 k;
        for (k = 0; k < 5; ++k) {
            camera_set_aspect(&c, widths[k], heights[k]);
            camera_frame_bounds(&c, b, 0.08f);
            TG_EXPECT_MSG(camera_bounds_fully_visible(&c, b),
                          "not visible at %ux%u", widths[k], heights[k]);
        }
        /* A very tall viewport must push the camera further back than a wide
         * one, because the horizontal half-angle becomes the limiting one. */
        camera_set_aspect(&c, 400, 2000);
        camera_frame_bounds(&c, b, 0.08f);
        {
            f32 tall = c.distance;
            camera_set_aspect(&c, 2000, 400);
            camera_frame_bounds(&c, b, 0.08f);
            TG_EXPECT_MSG(tall > c.distance,
                          "tall viewport distance %.3f should exceed wide %.3f",
                          (double)tall, (double)c.distance);
        }
    }

    TG_T_CASE("framing does not widen the lens to compensate for distance");
    /* The forbidden shortcut is to place the camera very close and open the FOV.
     * Framing must move the camera and leave the FOV alone. */
    {
        f32 fov_before;
        camera_set_aspect(&c, 1600, 900);
        fov_before = c.fov_y;
        camera_frame_bounds(&c, b, 0.08f);
        TG_EXPECT_NEAR(c.fov_y, fov_before, 0.0);
        /* And the resulting distance must be a sane multiple of the object size,
         * not pressed against it. */
        TG_EXPECT_MSG(c.distance > aabb_diagonal(b) * 0.5f,
                      "framed distance %.3f is inside the bounding sphere",
                      (double)c.distance);
    }

    TG_T_CASE("a bigger margin means a greater distance");
    {
        f32 d_small, d_big;
        camera_frame_bounds(&c, b, 0.0f);
        d_small = c.distance;
        camera_frame_bounds(&c, b, 0.5f);
        d_big = c.distance;
        TG_EXPECT(d_big > d_small);
    }

    TG_T_CASE("framing an empty box is a no-op, not a teleport to NaN");
    {
        V3 p = camera_position(&c);
        camera_frame_bounds(&c, aabb_empty(), 0.1f);
        TG_EXPECT_V3_NEAR(camera_position(&c), p, 1e-5);
    }

    TG_T_CASE("orthographic framing also contains the object");
    c.projection = CAMERA_PROJ_ORTHOGRAPHIC;
    camera_set_aspect(&c, 1600, 900);
    camera_frame_bounds(&c, b, 0.05f);
    TG_EXPECT(camera_bounds_fully_visible(&c, b));
}

static void test_scale_adaptive_behaviour(void) {
    Camera seedling, mature;
    Aabb small_b = aabb_empty();
    Aabb big_b = tree_bounds();

    small_b = aabb_add_point(small_b, v3(-0.15f, 0.0f, -0.15f));
    small_b = aabb_add_point(small_b, v3(0.15f, 0.4f, 0.15f));

    TG_T_CASE("near plane and speed scale with the scene, not fixed constants");
    /* Without this, a fixed near plane clips a 0.4 m seedling or wastes depth
     * precision on a 26 m tree, and a fixed move speed makes one unusable. */
    camera_init(&seedling);
    camera_set_scene_bounds(&seedling, small_b);
    camera_init(&mature);
    camera_set_scene_bounds(&mature, big_b);
    TG_EXPECT(seedling.near_z < mature.near_z);
    TG_EXPECT(seedling.scene_scale < mature.scene_scale);

    TG_T_CASE("the same key press traverses a comparable fraction of each scene");
    {
        Camera a = seedling, b_ = mature;
        V3 pa, pb;
        f32 fa, fb;
        a.mode = CAMERA_MODE_FREE;
        b_.mode = CAMERA_MODE_FREE;
        pa = a.free_position;
        pb = b_.free_position;
        camera_move(&a, v3(0, 0, -1), 1.0f, false);
        camera_move(&b_, v3(0, 0, -1), 1.0f, false);
        fa = v3_dist(a.free_position, pa) / a.scene_scale;
        fb = v3_dist(b_.free_position, pb) / b_.scene_scale;
        TG_EXPECT_NEAR(fa, fb, 1e-4);
        TG_EXPECT(fa > 0.0f);
    }

    TG_T_CASE("zoom is multiplicative and bounded by the scene, not hard-coded");
    {
        Camera c = mature;
        u32 i;
        camera_frame_bounds(&c, big_b, 0.08f);
        for (i = 0; i < 500; ++i) { camera_dolly(&c, 1.0f); } /* zoom right in */
        TG_EXPECT(c.distance > 0.0f);
        TG_EXPECT(tg_finitef(c.distance));
        /* Must be able to get genuinely close: the project requires close bark
         * inspection, so the minimum distance has to be a small fraction of the
         * scene rather than a fixed floor. */
        TG_EXPECT_MSG(c.distance < c.scene_scale * 0.01f,
                      "cannot approach the surface: min distance %.5f of scene %.2f",
                      (double)c.distance, (double)c.scene_scale);
        for (i = 0; i < 2000; ++i) { camera_dolly(&c, -1.0f); } /* and back out */
        TG_EXPECT(tg_finitef(c.distance));
        TG_EXPECT(c.distance <= c.scene_scale * 40.0f + 1e-3f);
    }

    TG_T_CASE("a zero-size scene falls back to unit scale instead of dividing by 0");
    {
        Camera c;
        Aabb point = aabb_add_point(aabb_empty(), v3(3, 3, 3));
        camera_init(&c);
        camera_set_scene_bounds(&c, point);
        TG_EXPECT(c.scene_scale > 0.0f);
        TG_EXPECT(tg_finitef(c.near_z) && c.near_z > 0.0f);
    }
}

static void test_aspect_robustness(void) {
    Camera c;
    f32 before;

    TG_T_CASE("a minimised window (0x0) keeps the previous aspect");
    /* Windows reports a zero client area when minimised; recomputing the aspect
     * from it would divide by zero and produce a NaN projection that persists
     * after restore. */
    camera_init(&c);
    camera_set_aspect(&c, 1600, 900);
    before = c.aspect;
    camera_set_aspect(&c, 0, 0);
    TG_EXPECT_NEAR(c.aspect, before, 0.0);
    camera_set_aspect(&c, 1920, 0);
    TG_EXPECT_NEAR(c.aspect, before, 0.0);
    {
        M4 vp = camera_view_projection(&c);
        int i;
        for (i = 0; i < 16; ++i) { TG_EXPECT(tg_finitef(vp.m[i])); }
    }
}

static void test_pick_ray(void) {
    Camera c;
    Aabb b = tree_bounds();
    Ray centre, corner;

    TG_T_CASE("the centre pixel produces the forward ray");
    camera_init(&c);
    camera_set_aspect(&c, 1600, 900);
    camera_frame_bounds_reset_angle(&c, b, 0.08f, 0.0f, 0.0f);
    centre = camera_pick_ray(&c, 800.0f, 450.0f, 1600, 900);
    TG_EXPECT_V3_NEAR(centre.origin, camera_position(&c), 1e-4);
    TG_EXPECT_V3_NEAR(centre.dir, camera_forward(&c), 1e-4);

    TG_T_CASE("pixel Y increases downward: a top pixel must aim upward");
    /* Getting this backwards inverts picking vertically, which is easy to miss
     * on an object as symmetric as a tree. */
    corner = camera_pick_ray(&c, 800.0f, 10.0f, 1600, 900);
    TG_EXPECT_MSG(v3_dot(corner.dir, camera_up(&c)) > 0.0f,
                  "a near-top pixel produced a downward ray");
    corner = camera_pick_ray(&c, 800.0f, 890.0f, 1600, 900);
    TG_EXPECT(v3_dot(corner.dir, camera_up(&c)) < 0.0f);

    TG_T_CASE("pixel X increases rightward");
    corner = camera_pick_ray(&c, 1590.0f, 450.0f, 1600, 900);
    TG_EXPECT(v3_dot(corner.dir, camera_right(&c)) > 0.0f);
    corner = camera_pick_ray(&c, 10.0f, 450.0f, 1600, 900);
    TG_EXPECT(v3_dot(corner.dir, camera_right(&c)) < 0.0f);

    TG_T_CASE("the corner ray angle matches the configured FOV exactly");
    {
        /* The ray through the top edge centre must be exactly fov_y/2 off axis.
         * This ties picking and projection to the same FOV, so a mismatch (a
         * classic source of "picking is offset from what I clicked") fails. */
        Ray top = camera_pick_ray(&c, 800.0f, 0.0f, 1600, 900);
        f32 ang = v3_angle_between(top.dir, camera_forward(&c));
        TG_EXPECT_NEAR(ang, c.fov_y * 0.5f, 1e-4);
    }

    TG_T_CASE("picking rays are always unit length and finite");
    {
        u32 i;
        TgRng r = tg_rng_substream(23, TG_RNG_GLOBAL, 3, 0);
        for (i = 0; i < 500; ++i) {
            Ray q = camera_pick_ray(&c, tg_rng_range(&r, 0.0f, 1600.0f),
                                    tg_rng_range(&r, 0.0f, 900.0f), 1600, 900);
            TG_EXPECT_NEAR(v3_len(q.dir), 1.0f, 1e-4);
            TG_EXPECT(v3_finite(q.origin));
        }
    }

    TG_T_CASE("a zero-size viewport yields the forward ray, not a division by zero");
    {
        Ray q = camera_pick_ray(&c, 0.0f, 0.0f, 0, 0);
        TG_EXPECT(v3_finite(q.dir));
        TG_EXPECT_V3_NEAR(q.dir, camera_forward(&c), 1e-5);
    }

    TG_T_CASE("orthographic picking rays are parallel but offset");
    c.projection = CAMERA_PROJ_ORTHOGRAPHIC;
    {
        Ray a = camera_pick_ray(&c, 100.0f, 100.0f, 1600, 900);
        Ray d = camera_pick_ray(&c, 1500.0f, 800.0f, 1600, 900);
        TG_EXPECT_V3_NEAR(a.dir, d.dir, 1e-5);
        TG_EXPECT(v3_dist(a.origin, d.origin) > 0.1f);
    }
}

static void test_mode_switch_no_jump(void) {
    Camera c;
    Aabb b = tree_bounds();
    V3 p_orbit, p_free;

    TG_T_CASE("switching orbit -> free keeps the eye where it was");
    camera_init(&c);
    camera_frame_bounds(&c, b, 0.08f);
    p_orbit = camera_position(&c);
    camera_set_mode(&c, CAMERA_MODE_FREE);
    p_free = camera_position(&c);
    TG_EXPECT_V3_NEAR(p_free, p_orbit, 1e-4);

    TG_T_CASE("switching free -> orbit keeps the eye and looks at what was ahead");
    camera_move(&c, v3(1, 0, -1), 0.5f, false);
    p_free = camera_position(&c);
    camera_set_mode(&c, CAMERA_MODE_ORBIT);
    TG_EXPECT_V3_NEAR(camera_position(&c), p_free, 1e-3);
    {
        /* The new pivot must lie ahead of the camera, not behind it. */
        V3 to_pivot = v3_sub(c.pivot, camera_position(&c));
        TG_EXPECT(v3_dot(v3_norm_or(to_pivot, v3_zero()), camera_forward(&c)) > 0.9f);
    }

    TG_T_CASE("setting the current mode again is a no-op");
    {
        V3 p = camera_position(&c);
        camera_set_mode(&c, CAMERA_MODE_ORBIT);
        TG_EXPECT_V3_NEAR(camera_position(&c), p, 0.0);
    }
}

static void test_depth_ordering(void) {
    Camera c;
    Aabb b = tree_bounds();
    M4 vp;
    V3 near_pt, far_pt;
    V4 cn, cf;

    TG_T_CASE("reverse-Z: a nearer point has a GREATER depth value");
    /* The whole depth pipeline (GREATER_EQUAL compare, cleared to 0) depends on
     * this sign. If it ever inverts, everything fails the depth test and the
     * screen goes empty -- a failure mode worth a dedicated assertion. */
    camera_init(&c);
    camera_set_aspect(&c, 1600, 900);
    camera_frame_bounds_reset_angle(&c, b, 0.08f, 0.0f, 0.0f);
    vp = camera_view_projection(&c);

    near_pt = v3_add(camera_position(&c), v3_scale(camera_forward(&c), 1.0f));
    far_pt = v3_add(camera_position(&c), v3_scale(camera_forward(&c), 100.0f));
    cn = m4_mul_v4(vp, v4(near_pt.x, near_pt.y, near_pt.z, 1.0f));
    cf = m4_mul_v4(vp, v4(far_pt.x, far_pt.y, far_pt.z, 1.0f));
    TG_EXPECT(cn.w > 0.0f && cf.w > 0.0f);
    TG_EXPECT_MSG((cn.z / cn.w) > (cf.z / cf.w),
                  "reverse-Z inverted: near %.6f, far %.6f",
                  (double)(cn.z / cn.w), (double)(cf.z / cf.w));

    TG_T_CASE("an infinite far plane never clips a distant point");
    {
        V3 very_far = v3_add(camera_position(&c),
                             v3_scale(camera_forward(&c), 1.0e7f));
        V4 cv = m4_mul_v4(vp, v4(very_far.x, very_far.y, very_far.z, 1.0f));
        TG_EXPECT(cv.w > 0.0f);
        TG_EXPECT(cv.z / cv.w >= 0.0f);
    }

    TG_T_CASE("a point behind the camera has negative w and is rejected");
    {
        V3 behind = v3_sub(camera_position(&c),
                           v3_scale(camera_forward(&c), 5.0f));
        V4 cb = m4_mul_v4(vp, v4(behind.x, behind.y, behind.z, 1.0f));
        TG_EXPECT(cb.w < 0.0f);
    }
}

static void test_long_session_stability(void) {
    Camera c;
    Aabb b = tree_bounds();
    TgRng r = tg_rng_substream(29, TG_RNG_GLOBAL, 4, 0);
    u32 i;

    TG_T_CASE("100k mixed interactions leave the camera finite and well formed");
    /* Stands in for a long inspection session. Orientation is rebuilt from
     * yaw/pitch every frame rather than integrated, so nothing should drift. */
    camera_init(&c);
    camera_frame_bounds(&c, b, 0.08f);
    for (i = 0; i < 100000; ++i) {
        u32 action = tg_rng_below(&r, 5);
        switch (action) {
        case 0: camera_orbit(&c, tg_rng_range(&r, -60.0f, 60.0f),
                             tg_rng_range(&r, -60.0f, 60.0f)); break;
        case 1: camera_pan(&c, tg_rng_range(&r, -80.0f, 80.0f),
                           tg_rng_range(&r, -80.0f, 80.0f)); break;
        case 2: camera_dolly(&c, tg_rng_range(&r, -3.0f, 3.0f)); break;
        case 3: camera_set_aspect(&c, 100 + tg_rng_below(&r, 4000),
                                  100 + tg_rng_below(&r, 3000)); break;
        default: (void)camera_set_fov_degrees(&c, tg_rng_range(&r, 10.0f, 100.0f));
                 break;
        }
    }
    {
        M4 vp = camera_view_projection(&c);
        int k;
        for (k = 0; k < 16; ++k) {
            TG_EXPECT_MSG(tg_finitef(vp.m[k]), "matrix element %d not finite", k);
        }
    }
    TG_EXPECT(tg_finitef(c.distance) && c.distance > 0.0f);
    TG_EXPECT(v3_finite(c.pivot));
    TG_EXPECT(c.pitch >= -CAMERA_PITCH_LIMIT - 1e-6f &&
              c.pitch <= CAMERA_PITCH_LIMIT + 1e-6f);
    TG_EXPECT(c.fov_y * TG_RAD2DEG_F >= CAMERA_FOV_MIN_DEG - 1e-3f);
    TG_EXPECT(c.fov_y * TG_RAD2DEG_F <= CAMERA_FOV_MAX_DEG + 1e-3f);
    TG_EXPECT_NEAR(v3_len(camera_forward(&c)), 1.0f, 1e-4);

    TG_T_CASE("reset restores a known good framed state");
    camera_reset(&c, b);
    TG_EXPECT(camera_bounds_fully_visible(&c, b));
    TG_EXPECT_NEAR(c.fov_y * TG_RAD2DEG_F, CAMERA_FOV_DEFAULT_DEG, 1e-4);
    TG_EXPECT_EQ_U64(c.mode, CAMERA_MODE_ORBIT);
    TG_EXPECT_EQ_U64(c.projection, CAMERA_PROJ_PERSPECTIVE);
}

void test_suite_camera(void) {
    test_defaults();
    test_basis();
    test_pitch_clamp_no_flip();
    test_orbit_keeps_pivot();
    test_framing();
    test_scale_adaptive_behaviour();
    test_aspect_robustness();
    test_pick_ray();
    test_mode_switch_no_jump();
    test_depth_ordering();
    test_long_session_stability();
}
