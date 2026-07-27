#include "test_util.h"
#include "test_suites.h"

#include "../src/core/rng.h"

static void test_vector_basics(void) {
    V3 a = v3(1.0f, 2.0f, 3.0f);
    V3 b = v3(-4.0f, 5.0f, 6.0f);

    TG_T_CASE("vector algebra");
    TG_EXPECT_NEAR(v3_dot(a, b), -4.0f + 10.0f + 18.0f, 1e-6);
    TG_EXPECT_V3_NEAR(v3_cross(v3(1, 0, 0), v3(0, 1, 0)), v3(0, 0, 1), 1e-6);
    TG_EXPECT_NEAR(v3_len(v3(3, 4, 0)), 5.0f, 1e-6);
    TG_EXPECT_NEAR(v3_dist(v3(1, 0, 0), v3(4, 4, 0)), 5.0f, 1e-6);

    TG_T_CASE("degenerate normalise returns the fallback, never NaN");
    TG_EXPECT_V3_NEAR(v3_norm_or(v3_zero(), v3(0, 1, 0)), v3(0, 1, 0), 0.0);
    TG_EXPECT(v3_finite(v3_norm_or(v3_zero(), v3(0, 1, 0))));
    /* A vector far below the degeneracy threshold must also be rejected rather
     * than normalised into garbage. */
    TG_EXPECT_V3_NEAR(v3_norm_or(v3(1e-20f, 0, 0), v3(1, 0, 0)), v3(1, 0, 0), 0.0);

    TG_T_CASE("any_perpendicular is perpendicular and unit for many inputs");
    {
        TgRng r = tg_rng_substream(1234, TG_RNG_GLOBAL, 0, 0);
        int i;
        for (i = 0; i < 500; ++i) {
            V3 n = tg_rng_unit_sphere(&r);
            V3 p = v3_any_perpendicular(n);
            TG_EXPECT_NEAR(v3_len(p), 1.0f, 1e-5);
            TG_EXPECT_NEAR(v3_dot(p, n), 0.0f, 1e-5);
        }
        /* And for exact axis directions, where a naive implementation degenerates. */
        TG_EXPECT_NEAR(v3_dot(v3_any_perpendicular(v3(1, 0, 0)), v3(1, 0, 0)), 0.0f, 1e-6);
        TG_EXPECT_NEAR(v3_dot(v3_any_perpendicular(v3(0, 1, 0)), v3(0, 1, 0)), 0.0f, 1e-6);
        TG_EXPECT_NEAR(v3_dot(v3_any_perpendicular(v3(0, 0, 1)), v3(0, 0, 1)), 0.0f, 1e-6);
    }

    TG_T_CASE("angle_between is stable at 0 and pi");
    TG_EXPECT_NEAR(v3_angle_between(v3(0, 1, 0), v3(0, 1, 0)), 0.0f, 1e-5);
    TG_EXPECT_NEAR(v3_angle_between(v3(0, 1, 0), v3(0, -1, 0)), TG_PI_F, 1e-5);
    TG_EXPECT_NEAR(v3_angle_between(v3(1, 0, 0), v3(0, 1, 0)), TG_PI_F * 0.5f, 1e-5);
    /* Near-parallel: acos(dot) would lose all precision here. */
    {
        V3 u = v3_norm_or(v3(1.0f, 1e-4f, 0.0f), v3(1, 0, 0));
        f32 ang = v3_angle_between(v3(1, 0, 0), u);
        TG_EXPECT_NEAR(ang, 1e-4f, 1e-6);
    }
}

static void test_rotate_toward(void) {
    TG_T_CASE("rotate_toward respects the angular budget");
    {
        V3 from = v3(0, 1, 0);
        V3 to = v3(1, 0, 0);
        V3 r = v3_rotate_toward(from, to, 0.1f);
        TG_EXPECT_NEAR(v3_len(r), 1.0f, 1e-5);
        TG_EXPECT_NEAR(v3_angle_between(from, r), 0.1f, 1e-5);
        /* The step must be in the plane spanned by from and to. */
        TG_EXPECT_NEAR(r.z, 0.0f, 1e-6);
    }

    TG_T_CASE("rotate_toward snaps when the target is within budget");
    {
        V3 from = v3(0, 1, 0);
        V3 to = v3_norm_or(v3(0.05f, 1, 0), from);
        V3 r = v3_rotate_toward(from, to, 1.0f);
        TG_EXPECT_V3_NEAR(r, to, 1e-6);
    }

    TG_T_CASE("rotate_toward handles the antiparallel case without NaN");
    {
        V3 r = v3_rotate_toward(v3(0, 1, 0), v3(0, -1, 0), 0.25f);
        TG_EXPECT(v3_finite(r));
        TG_EXPECT_NEAR(v3_len(r), 1.0f, 1e-5);
        TG_EXPECT_NEAR(v3_angle_between(v3(0, 1, 0), r), 0.25f, 1e-4);
    }

    TG_T_CASE("zero budget is a no-op");
    TG_EXPECT_V3_NEAR(v3_rotate_toward(v3(0, 1, 0), v3(1, 0, 0), 0.0f), v3(0, 1, 0), 0.0);
}

static void test_quaternion(void) {
    TgRng r = tg_rng_substream(99, TG_RNG_GLOBAL, 1, 0);
    int i;

    TG_T_CASE("quaternion rotation matches axis-angle expectation");
    {
        Quat q = quat_from_axis_angle(v3(0, 1, 0), TG_PI_F * 0.5f);
        TG_EXPECT_V3_NEAR(quat_rotate(q, v3(1, 0, 0)), v3(0, 0, -1), 1e-5);
    }

    TG_T_CASE("quaternion rotation preserves length and composes correctly");
    for (i = 0; i < 400; ++i) {
        V3 axis = tg_rng_unit_sphere(&r);
        f32 ang = tg_rng_range(&r, -TG_PI_F, TG_PI_F);
        Quat q = quat_from_axis_angle(axis, ang);
        V3 v = tg_rng_unit_sphere(&r);
        V3 rv = quat_rotate(q, v);
        TG_EXPECT_NEAR(v3_len(rv), 1.0f, 1e-4);
        /* Rotation about the axis leaves the axis component unchanged. */
        TG_EXPECT_NEAR(v3_dot(rv, axis), v3_dot(v, axis), 1e-4);
    }

    TG_T_CASE("quat_from_to maps from onto to, including antiparallel");
    for (i = 0; i < 400; ++i) {
        V3 a = tg_rng_unit_sphere(&r);
        V3 b = tg_rng_unit_sphere(&r);
        Quat q = quat_from_to(a, b);
        TG_EXPECT_V3_NEAR(quat_rotate(q, a), b, 1e-3);
    }
    {
        V3 a = v3(0, 1, 0);
        Quat q = quat_from_to(a, v3_neg(a));
        V3 got = quat_rotate(q, a);
        TG_EXPECT(v3_finite(got));
        TG_EXPECT_V3_NEAR(got, v3_neg(a), 1e-4);
    }

    TG_T_CASE("quaternion matrix agrees with direct rotation");
    for (i = 0; i < 200; ++i) {
        V3 axis = tg_rng_unit_sphere(&r);
        f32 ang = tg_rng_range(&r, -TG_PI_F, TG_PI_F);
        Quat q = quat_from_axis_angle(axis, ang);
        M4 m = m4_from_quat(q);
        V3 v = tg_rng_unit_sphere(&r);
        TG_EXPECT_V3_NEAR(m4_transform_dir(m, v), quat_rotate(q, v), 1e-4);
    }

    TG_T_CASE("slerp endpoints and shortest arc");
    {
        Quat a = quat_from_axis_angle(v3(0, 1, 0), 0.0f);
        Quat b = quat_from_axis_angle(v3(0, 1, 0), 1.0f);
        Quat mid = quat_slerp(a, b, 0.5f);
        V3 v = v3(1, 0, 0);
        TG_EXPECT_V3_NEAR(quat_rotate(quat_slerp(a, b, 0.0f), v), quat_rotate(a, v), 1e-5);
        TG_EXPECT_V3_NEAR(quat_rotate(quat_slerp(a, b, 1.0f), v), quat_rotate(b, v), 1e-5);
        TG_EXPECT_V3_NEAR(quat_rotate(mid, v),
                          quat_rotate(quat_from_axis_angle(v3(0, 1, 0), 0.5f), v), 1e-4);
    }
    {
        /* Negated quaternion is the same rotation; slerp must take the short arc
         * and not spin the long way round. */
        Quat a = quat_identity();
        Quat b = quat_scale(quat_from_axis_angle(v3(0, 1, 0), 0.4f), -1.0f);
        Quat mid = quat_slerp(a, b, 0.5f);
        V3 v = v3(1, 0, 0);
        TG_EXPECT_V3_NEAR(quat_rotate(mid, v),
                          quat_rotate(quat_from_axis_angle(v3(0, 1, 0), 0.2f), v), 1e-3);
    }
}

static void test_matrix(void) {
    TgRng r = tg_rng_substream(7, TG_RNG_GLOBAL, 2, 0);
    int i;

    TG_T_CASE("identity and multiplication order (M*v, P*V*M)");
    {
        M4 id = m4_identity();
        M4 t = m4_translation(v3(1, 2, 3));
        M4 s = m4_scale(v3(2, 2, 2));
        TG_EXPECT_V3_NEAR(m4_transform_point(id, v3(4, 5, 6)), v3(4, 5, 6), 1e-6);
        /* Composition t*s must scale first, then translate. */
        TG_EXPECT_V3_NEAR(m4_transform_point(m4_mul(t, s), v3(1, 1, 1)), v3(3, 4, 5), 1e-6);
        /* And s*t must translate first, then scale. */
        TG_EXPECT_V3_NEAR(m4_transform_point(m4_mul(s, t), v3(1, 1, 1)), v3(4, 6, 8), 1e-6);
    }

    TG_T_CASE("translation lands in the fourth column (column-major layout)");
    {
        M4 t = m4_translation(v3(7, 8, 9));
        TG_EXPECT_NEAR(t.m[12], 7.0f, 0.0);
        TG_EXPECT_NEAR(t.m[13], 8.0f, 0.0);
        TG_EXPECT_NEAR(t.m[14], 9.0f, 0.0);
        TG_EXPECT_NEAR(M4_AT(t, 0, 3), 7.0f, 0.0);
    }

    TG_T_CASE("general inverse round-trips");
    for (i = 0; i < 200; ++i) {
        Quat q = quat_from_axis_angle(tg_rng_unit_sphere(&r),
                                     tg_rng_range(&r, -3.0f, 3.0f));
        M4 m = m4_mul(m4_from_quat_translation(q, tg_rng_in_unit_ball(&r)),
                      m4_scale(v3(tg_rng_range(&r, 0.5f, 2.0f),
                                  tg_rng_range(&r, 0.5f, 2.0f),
                                  tg_rng_range(&r, 0.5f, 2.0f))));
        M4 inv;
        TG_EXPECT(m4_inverse(m, &inv));
        {
            V3 p = tg_rng_in_unit_ball(&r);
            TG_EXPECT_V3_NEAR(m4_transform_point(inv, m4_transform_point(m, p)), p, 1e-3);
        }
    }

    TG_T_CASE("singular matrix inversion is refused");
    {
        M4 sing = m4_scale(v3(1, 0, 1));
        M4 out = m4_identity();
        TG_EXPECT(!m4_inverse(sing, &out));
    }

    TG_T_CASE("rigid inverse matches general inverse");
    for (i = 0; i < 100; ++i) {
        Quat q = quat_from_axis_angle(tg_rng_unit_sphere(&r),
                                     tg_rng_range(&r, -3.0f, 3.0f));
        M4 m = m4_from_quat_translation(q, tg_rng_in_unit_ball(&r));
        M4 gi;
        M4 ri = m4_inverse_rigid(m);
        TG_EXPECT(m4_inverse(m, &gi));
        {
            V3 p = tg_rng_in_unit_ball(&r);
            TG_EXPECT_V3_NEAR(m4_transform_point(ri, p), m4_transform_point(gi, p), 1e-4);
        }
    }
}

static void test_look_at(void) {
    TG_T_CASE("look_at places the target on -Z in view space");
    {
        M4 v = m4_look_at_rh(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
        V3 p = m4_transform_point(v, v3(0, 0, 0));
        TG_EXPECT_NEAR(p.x, 0.0f, 1e-5);
        TG_EXPECT_NEAR(p.y, 0.0f, 1e-5);
        TG_EXPECT_NEAR(p.z, -5.0f, 1e-5);
    }

    TG_T_CASE("look_at keeps world up on view +Y and world right on view +X");
    {
        M4 v = m4_look_at_rh(v3(0, 0, 5), v3(0, 0, 0), v3(0, 1, 0));
        TG_EXPECT_V3_NEAR(m4_transform_dir(v, v3(0, 1, 0)), v3(0, 1, 0), 1e-5);
        TG_EXPECT_V3_NEAR(m4_transform_dir(v, v3(1, 0, 0)), v3(1, 0, 0), 1e-5);
    }

    TG_T_CASE("look_at handles degenerate up parallel to view direction");
    {
        M4 v = m4_look_at_rh(v3(0, 5, 0), v3(0, 0, 0), v3(0, 1, 0));
        V3 p = m4_transform_point(v, v3(0, 0, 0));
        TG_EXPECT(v3_finite(p));
        TG_EXPECT_NEAR(p.z, -5.0f, 1e-4);
    }

    TG_T_CASE("look_at with coincident eye and target does not produce NaN");
    {
        M4 v = m4_look_at_rh(v3(1, 1, 1), v3(1, 1, 1), v3(0, 1, 0));
        V3 p = m4_transform_point(v, v3(2, 2, 2));
        TG_EXPECT(v3_finite(p));
    }
}

/* Projects a view-space depth `d` (positive, in front of the camera) and returns
 * the resulting NDC z. This is the exact quantity the depth buffer receives. */
static f32 project_depth(M4 p, f32 d) {
    V4 clip = m4_mul_v4(p, v4(0.0f, 0.0f, -d, 1.0f));
    return clip.z / clip.w;
}

static void test_projection_reverse_z(void) {
    const f32 near_z = 0.05f;
    const f32 far_z = 500.0f;
    M4 p = m4_perspective_reverse_z(45.0f * TG_DEG2RAD_F, 16.0f / 9.0f, near_z, far_z);

    TG_T_CASE("finite reverse-Z: near maps to 1, far maps to 0");
    TG_EXPECT_NEAR(project_depth(p, near_z), 1.0f, 1e-5);
    TG_EXPECT_NEAR(project_depth(p, far_z), 0.0f, 1e-5);

    TG_T_CASE("reverse-Z depth is monotonically decreasing with distance");
    {
        f32 prev = project_depth(p, near_z);
        int i;
        for (i = 1; i <= 200; ++i) {
            f32 d = near_z + (far_z - near_z) * ((f32)i / 200.0f);
            f32 z = project_depth(p, d);
            TG_EXPECT_MSG(z < prev, "depth not decreasing at d=%.4f (%.9f >= %.9f)",
                          (double)d, (double)z, (double)prev);
            prev = z;
        }
    }

    TG_T_CASE("reverse-Z concentrates precision near the camera");
    {
        /* Half the [0,1] depth range must be spent within a small fraction of
         * the view distance. This is the whole point of reverse-Z; if it fails,
         * the projection has been silently rebuilt as conventional Z. */
        f32 z_at_2x_near = project_depth(p, near_z * 2.0f);
        TG_EXPECT_MSG(z_at_2x_near > 0.45f && z_at_2x_near < 0.55f,
                      "z at 2x near = %.6f, expected ~0.5", (double)z_at_2x_near);
    }

    TG_T_CASE("aspect ratio scales x only");
    {
        M4 p1 = m4_perspective_reverse_z(45.0f * TG_DEG2RAD_F, 1.0f, near_z, far_z);
        M4 p2 = m4_perspective_reverse_z(45.0f * TG_DEG2RAD_F, 2.0f, near_z, far_z);
        TG_EXPECT_NEAR(M4_AT(p1, 1, 1), M4_AT(p2, 1, 1), 1e-6);
        TG_EXPECT_NEAR(M4_AT(p1, 0, 0), M4_AT(p2, 0, 0) * 2.0f, 1e-5);
    }

    TG_T_CASE("vertical FOV matches the requested angle");
    {
        f32 fov = 40.0f * TG_DEG2RAD_F;
        M4 pf = m4_perspective_reverse_z(fov, 1.0f, near_z, far_z);
        /* A point at the top edge of the frustum at depth d has
         * y = d * tan(fov/2) and must land exactly at NDC y = 1. */
        f32 d = 10.0f;
        f32 y = d * tanf(fov * 0.5f);
        V4 clip = m4_mul_v4(pf, v4(0.0f, y, -d, 1.0f));
        TG_EXPECT_NEAR(clip.y / clip.w, 1.0f, 1e-5);
    }

    TG_T_CASE("infinite reverse-Z: near maps to 1, distance tends to 0");
    {
        M4 pi_ = m4_perspective_reverse_z_infinite(45.0f * TG_DEG2RAD_F, 1.6f, near_z);
        TG_EXPECT_NEAR(project_depth(pi_, near_z), 1.0f, 1e-6);
        TG_EXPECT_NEAR(project_depth(pi_, 1.0e6f), 0.0f, 1e-6);
        TG_EXPECT(project_depth(pi_, 1000.0f) > 0.0f);
        /* Monotonic, and never clipped away: an infinite far plane must never
         * produce a negative depth for any positive view distance. */
        TG_EXPECT(project_depth(pi_, 1.0e9f) >= 0.0f);
    }

    TG_T_CASE("orthographic reverse-Z: near maps to 1, far maps to 0");
    {
        M4 po = m4_ortho_reverse_z(4.0f, 3.0f, 1.0f, 100.0f);
        V4 cn = m4_mul_v4(po, v4(0.0f, 0.0f, -1.0f, 1.0f));
        V4 cf = m4_mul_v4(po, v4(0.0f, 0.0f, -100.0f, 1.0f));
        TG_EXPECT_NEAR(cn.z / cn.w, 1.0f, 1e-6);
        TG_EXPECT_NEAR(cf.z / cf.w, 0.0f, 1e-6);
        /* Half-extents map to NDC +/-1. */
        {
            V4 ce = m4_mul_v4(po, v4(4.0f, 3.0f, -50.0f, 1.0f));
            TG_EXPECT_NEAR(ce.x / ce.w, 1.0f, 1e-6);
            TG_EXPECT_NEAR(ce.y / ce.w, 1.0f, 1e-6);
        }
    }
}

static void test_frames(void) {
    TG_T_CASE("frame_make is orthonormal and right-handed");
    {
        Frame f = frame_make(v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
        TG_EXPECT(frame_is_orthonormal(f, 1e-5f));
        TG_EXPECT_V3_NEAR(f.t, v3(0, 1, 0), 1e-6);
        TG_EXPECT_NEAR(v3_dot(f.n, v3(1, 0, 0)), 1.0f, 1e-5);
    }

    TG_T_CASE("frame_make survives a reference hint parallel to the tangent");
    {
        Frame f = frame_make(v3(0, 0, 0), v3(0, 1, 0), v3(0, 1, 0));
        TG_EXPECT(frame_is_orthonormal(f, 1e-5f));
    }

    TG_T_CASE("frame_make survives a zero tangent");
    {
        Frame f = frame_make(v3(0, 0, 0), v3_zero(), v3(1, 0, 0));
        TG_EXPECT(frame_is_orthonormal(f, 1e-5f));
    }

    TG_T_CASE("RMF stays orthonormal along a strongly curved, twisting axis");
    {
        /* A helix with varying pitch: has non-zero curvature and torsion
         * everywhere, which is the case a naive Frenet frame handles worst. */
        Frame f = frame_make(v3(1, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
        int i;
        const int n = 400;
        for (i = 1; i <= n; ++i) {
            f32 s = (f32)i / (f32)n * 8.0f * TG_PI_F;
            V3 p = v3(cosf(s) * (1.0f + 0.1f * s), 0.35f * s, sinf(s) * (1.0f + 0.1f * s));
            V3 prev = f.origin;
            f = frame_propagate_rmf(f, p, v3_sub(p, prev));
            TG_EXPECT_MSG(frame_is_orthonormal(f, 2e-4f),
                          "frame lost orthonormality at step %d", i);
        }
    }

    TG_T_CASE("RMF has no twist on a straight axis");
    {
        Frame f = frame_make(v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
        V3 n0 = f.n;
        int i;
        for (i = 1; i <= 100; ++i) {
            V3 p = v3(0.0f, (f32)i * 0.1f, 0.0f);
            f = frame_propagate_rmf(f, p, v3(0, 1, 0));
        }
        /* Zero curvature: the reference direction must be exactly preserved.
         * A Frenet frame is undefined here; an incremental-rotation frame would
         * drift. */
        TG_EXPECT_V3_NEAR(f.n, n0, 1e-5);
    }

    TG_T_CASE("RMF total twist over a closed-ish curve stays bounded");
    {
        /* Traverse a planar arc out and back. Because RMF minimises rotation
         * about the tangent, returning along the reversed path must return the
         * normal to (approximately) its starting orientation. This is the
         * property that keeps bark grain from spiralling. */
        Frame f = frame_make(v3(1, 0, 0), v3(0, 0, 1), v3(1, 0, 0));
        V3 n0 = f.n;
        int i;
        const int n = 200;
        for (i = 1; i <= n; ++i) {
            f32 a = (f32)i / (f32)n * TG_PI_F;
            V3 p = v3(cosf(a), 0.0f, sinf(a));
            V3 t = v3(-sinf(a), 0.0f, cosf(a));
            f = frame_propagate_rmf(f, p, t);
        }
        for (i = n - 1; i >= 0; --i) {
            f32 a = (f32)i / (f32)n * TG_PI_F;
            V3 p = v3(cosf(a), 0.0f, sinf(a));
            V3 t = v3(sinf(a), 0.0f, -cosf(a));
            f = frame_propagate_rmf(f, p, t);
        }
        /* The tangent is now reversed, so the normal may be reflected; compare
         * the absolute alignment. */
        TG_EXPECT_NEAR(tg_absf(v3_dot(f.n, n0)), 1.0f, 5e-3);
    }

    TG_T_CASE("RMF handles a duplicated point without producing NaN");
    {
        Frame f = frame_make(v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
        f = frame_propagate_rmf(f, v3(0, 0, 0), v3(0, 1, 0));
        TG_EXPECT(frame_is_orthonormal(f, 1e-4f));
        f = frame_propagate_rmf(f, v3(0, 0, 0), v3(1, 0, 0));
        TG_EXPECT(frame_is_orthonormal(f, 1e-4f));
    }

    TG_T_CASE("ring points lie on the circle and wind CCW seen from +t");
    {
        Frame f = frame_make(v3(0, 5, 0), v3(0, 1, 0), v3(1, 0, 0));
        V3 p0 = frame_ring_point(f, 0.0f, 2.0f);
        V3 p1 = frame_ring_point(f, TG_PI_F * 0.5f, 2.0f);
        TG_EXPECT_NEAR(v3_dist(p0, f.origin), 2.0f, 1e-5);
        TG_EXPECT_NEAR(v3_dist(p1, f.origin), 2.0f, 1e-5);
        /* cross(p0-o, p1-o) must point along +t for CCW winding. */
        {
            V3 c = v3_cross(v3_sub(p0, f.origin), v3_sub(p1, f.origin));
            TG_EXPECT(v3_dot(c, f.t) > 0.0f);
        }
        TG_EXPECT_NEAR(frame_angle_of(f, frame_ring_dir(f, 1.0f)), 1.0f, 1e-5);
    }

    TG_T_CASE("frame_twist rotates the reference by exactly the given angle");
    {
        Frame f = frame_make(v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0));
        Frame g = frame_twist(f, 0.7f);
        TG_EXPECT(frame_is_orthonormal(g, 1e-5f));
        TG_EXPECT_NEAR(frame_angle_of(f, g.n), 0.7f, 1e-5);
    }
}

static void test_aabb(void) {
    TG_T_CASE("aabb accumulation and queries");
    {
        Aabb b = aabb_empty();
        TG_EXPECT(aabb_is_empty(b));
        TG_EXPECT_NEAR(aabb_diagonal(b), 0.0f, 0.0);
        TG_EXPECT_NEAR(aabb_surface_area(b), 0.0f, 0.0);
        b = aabb_add_point(b, v3(-1, -2, -3));
        b = aabb_add_point(b, v3(4, 5, 6));
        TG_EXPECT(!aabb_is_empty(b));
        TG_EXPECT_V3_NEAR(aabb_center(b), v3(1.5f, 1.5f, 1.5f), 1e-6);
        TG_EXPECT_V3_NEAR(aabb_extent(b), v3(5, 7, 9), 1e-6);
        TG_EXPECT(aabb_contains(b, v3(0, 0, 0)));
        TG_EXPECT(!aabb_contains(b, v3(10, 0, 0)));
        TG_EXPECT_NEAR(aabb_surface_area(b), 2.0f * (35.0f + 63.0f + 45.0f), 1e-4);
    }

    TG_T_CASE("aabb union with empty is identity");
    {
        Aabb e = aabb_empty();
        Aabb b = aabb_add_point(aabb_add_point(aabb_empty(), v3(0, 0, 0)), v3(1, 1, 1));
        TG_EXPECT_V3_NEAR(aabb_union(e, b).mn, b.mn, 0.0);
        TG_EXPECT_V3_NEAR(aabb_union(b, e).mx, b.mx, 0.0);
    }

    TG_T_CASE("aabb overlap");
    {
        Aabb a = aabb_add_point(aabb_add_point(aabb_empty(), v3(0, 0, 0)), v3(1, 1, 1));
        Aabb b = aabb_add_point(aabb_add_point(aabb_empty(), v3(1, 1, 1)), v3(2, 2, 2));
        Aabb c = aabb_add_point(aabb_add_point(aabb_empty(), v3(2, 2, 2)), v3(3, 3, 3));
        TG_EXPECT(aabb_overlaps(a, b));
        TG_EXPECT(!aabb_overlaps(a, c));
    }
}

static void test_ray(void) {
    Ray r;
    f32 t, u, v;

    r.origin = v3(0.25f, 0.25f, 1.0f);
    r.dir = v3(0, 0, -1);
    r.t_min = 0.0f;
    r.t_max = 100.0f;

    TG_T_CASE("ray hits a front-facing triangle");
    /* CCW when seen from +Z, so its normal points at the ray origin. */
    TG_EXPECT(ray_triangle(r, v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0), true, &t, &u, &v));
    TG_EXPECT_NEAR(t, 1.0f, 1e-5);
    TG_EXPECT_NEAR(u, 0.25f, 1e-5);
    TG_EXPECT_NEAR(v, 0.25f, 1e-5);

    TG_T_CASE("backface culling rejects the reversed winding");
    TG_EXPECT(!ray_triangle(r, v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0), true, &t, &u, &v));
    TG_EXPECT(ray_triangle(r, v3(0, 0, 0), v3(0, 1, 0), v3(1, 0, 0), false, &t, &u, &v));

    TG_T_CASE("ray misses outside the triangle");
    r.origin = v3(0.9f, 0.9f, 1.0f);
    TG_EXPECT(!ray_triangle(r, v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0), false, &t, &u, &v));

    TG_T_CASE("degenerate triangle is rejected, not divided by zero");
    r.origin = v3(0.25f, 0.25f, 1.0f);
    TG_EXPECT(!ray_triangle(r, v3(0, 0, 0), v3(1, 0, 0), v3(2, 0, 0), false, &t, &u, &v));

    TG_T_CASE("t_min / t_max are honoured");
    r.t_min = 2.0f;
    TG_EXPECT(!ray_triangle(r, v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0), false, &t, &u, &v));
    r.t_min = 0.0f;
    r.t_max = 0.5f;
    TG_EXPECT(!ray_triangle(r, v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0), false, &t, &u, &v));

    TG_T_CASE("triangle area and normal");
    TG_EXPECT_NEAR(triangle_area(v3(0, 0, 0), v3(2, 0, 0), v3(0, 3, 0)), 3.0f, 1e-5);
    TG_EXPECT_V3_NEAR(
        v3_norm_or(triangle_normal_unnormalized(v3(0, 0, 0), v3(1, 0, 0), v3(0, 1, 0)),
                   v3_zero()),
        v3(0, 0, 1), 1e-6);

    TG_T_CASE("ray/aabb slab test including axis-parallel rays");
    {
        Aabb b = aabb_add_point(aabb_add_point(aabb_empty(), v3(-1, -1, -1)), v3(1, 1, 1));
        Ray q;
        f32 tn;
        q.origin = v3(0, 0, 5);
        q.dir = v3(0, 0, -1);
        q.t_min = 0.0f;
        q.t_max = 100.0f;
        TG_EXPECT(ray_aabb(q, b, &tn));
        TG_EXPECT_NEAR(tn, 4.0f, 1e-5);

        /* Parallel to the slab and outside it: must miss without dividing by 0. */
        q.origin = v3(0, 5, 0);
        q.dir = v3(1, 0, 0);
        TG_EXPECT(!ray_aabb(q, b, &tn));

        /* Parallel and inside. */
        q.origin = v3(-5, 0, 0);
        q.dir = v3(1, 0, 0);
        TG_EXPECT(ray_aabb(q, b, &tn));
        TG_EXPECT_NEAR(tn, 4.0f, 1e-5);

        /* Origin inside the box. */
        q.origin = v3(0, 0, 0);
        q.dir = v3(0, 1, 0);
        q.t_min = 0.0f;
        TG_EXPECT(ray_aabb(q, b, &tn));
    }
}

static void test_scalar_helpers(void) {
    TG_T_CASE("clamp / lerp / remap / smoothstep");
    TG_EXPECT_NEAR(tg_clampf(5.0f, 0.0f, 1.0f), 1.0f, 0.0);
    TG_EXPECT_NEAR(tg_clampf(-5.0f, 0.0f, 1.0f), 0.0f, 0.0);
    TG_EXPECT_NEAR(tg_lerpf(2.0f, 4.0f, 0.25f), 2.5f, 1e-6);
    TG_EXPECT_NEAR(tg_remap01f(5.0f, 0.0f, 10.0f), 0.5f, 1e-6);
    /* Degenerate interval must not divide by zero. */
    TG_EXPECT_NEAR(tg_remap01f(5.0f, 3.0f, 3.0f), 0.0f, 0.0);
    TG_EXPECT_NEAR(tg_smoothstepf(0.0f, 1.0f, 0.0f), 0.0f, 0.0);
    TG_EXPECT_NEAR(tg_smoothstepf(0.0f, 1.0f, 1.0f), 1.0f, 0.0);
    TG_EXPECT_NEAR(tg_smoothstepf(0.0f, 1.0f, 0.5f), 0.5f, 1e-6);
    TG_EXPECT_NEAR(tg_smootherstepf(0.0f, 1.0f, 0.5f), 0.5f, 1e-6);
    TG_EXPECT_NEAR(tg_smootherstepf(0.0f, 1.0f, 1.0f), 1.0f, 1e-6);

    TG_T_CASE("finite detection");
    TG_EXPECT(tg_finitef(0.0f));
    TG_EXPECT(tg_finitef(-1e30f));
    TG_EXPECT(!tg_finitef(1.0f / 0.0f * 0.0f)); /* NaN */
    {
        volatile f32 zero = 0.0f;
        TG_EXPECT(!tg_finitef(1.0f / zero));
        TG_EXPECT(!tg_finitef(-1.0f / zero));
    }

    TG_T_CASE("wrap_pi");
    TG_EXPECT_NEAR(tg_wrap_pi(0.0f), 0.0f, 1e-6);
    TG_EXPECT_NEAR(tg_wrap_pi(TG_TAU_F), 0.0f, 1e-5);
    TG_EXPECT_NEAR(tg_wrap_pi(TG_PI_F + 0.1f), -TG_PI_F + 0.1f, 1e-5);
    TG_EXPECT_NEAR(tg_wrap_pi(-TG_TAU_F - 0.5f), -0.5f, 1e-5);

    TG_T_CASE("golden angle constant matches 137.5077640 degrees");
    TG_EXPECT_NEAR((f64)TG_GOLDEN_ANGLE_F * (f64)TG_RAD2DEG_F, 137.50776405, 1e-5);
}

void test_suite_math(void) {
    test_scalar_helpers();
    test_vector_basics();
    test_rotate_toward();
    test_quaternion();
    test_matrix();
    test_look_at();
    test_projection_reverse_z();
    test_frames();
    test_aabb();
    test_ray();
}
