#include "math3d.h"

/* ------------------------------------------------------------------------- */
/* Vectors                                                                   */
/* ------------------------------------------------------------------------- */

V3 v3_any_perpendicular(V3 n) {
    /* Pick the axis least aligned with n. Crossing with that axis maximises the
     * magnitude of the result, avoiding cancellation. */
    V3 axis;
    f32 ax = tg_absf(n.x), ay = tg_absf(n.y), az = tg_absf(n.z);
    if (ax <= ay && ax <= az) {
        axis = v3(1.0f, 0.0f, 0.0f);
    } else if (ay <= az) {
        axis = v3(0.0f, 1.0f, 0.0f);
    } else {
        axis = v3(0.0f, 0.0f, 1.0f);
    }
    return v3_norm_or(v3_cross(n, axis), v3(1.0f, 0.0f, 0.0f));
}

V3 v3_rotate_axis(V3 v, V3 axis, f32 angle) {
    f32 c = cosf(angle);
    f32 s = sinf(angle);
    V3 k = v3_norm_or(axis, v3(0.0f, 1.0f, 0.0f));
    /* Rodrigues: v*cos + (k x v)*sin + k*(k.v)*(1-cos) */
    return v3_add(v3_add(v3_scale(v, c), v3_scale(v3_cross(k, v), s)),
                  v3_scale(k, v3_dot(k, v) * (1.0f - c)));
}

f32 v3_angle_between(V3 a, V3 b) {
    /* atan2(|a x b|, a.b) is stable at both 0 and pi, unlike acos(dot). */
    f32 cr = v3_len(v3_cross(a, b));
    f32 dt = v3_dot(a, b);
    if (cr < TG_TINY_F && tg_absf(dt) < TG_TINY_F) { return 0.0f; }
    return atan2f(cr, dt);
}

V3 v3_slerp_unit(V3 a, V3 b, f32 t) {
    f32 d = tg_clampf(v3_dot(a, b), -1.0f, 1.0f);
    f32 theta, sin_theta;
    if (d > 0.99995f) {
        return v3_norm_or(v3_lerp(a, b, t), a);
    }
    if (d < -0.99995f) {
        /* Antiparallel: no unique arc. Rotate about an arbitrary perpendicular
         * so the result is well defined and continuous in t. */
        V3 axis = v3_any_perpendicular(a);
        return v3_rotate_axis(a, axis, TG_PI_F * t);
    }
    theta = acosf(d);
    sin_theta = sinf(theta);
    return v3_add(v3_scale(a, sinf((1.0f - t) * theta) / sin_theta),
                  v3_scale(b, sinf(t * theta) / sin_theta));
}

V3 v3_rotate_toward(V3 v, V3 target, f32 max_angle) {
    V3 axis;
    f32 angle;
    if (max_angle <= 0.0f) { return v; }
    angle = v3_angle_between(v, target);
    if (angle <= max_angle) {
        /* Reaching the target exactly is correct and avoids jitter. */
        return v3_norm_or(target, v);
    }
    axis = v3_cross(v, target);
    if (v3_len_sq(axis) < TG_TINY_F) {
        /* Parallel (angle 0, already handled) or antiparallel. */
        axis = v3_any_perpendicular(v);
    }
    return v3_norm_or(v3_rotate_axis(v, axis, max_angle), v);
}

/* ------------------------------------------------------------------------- */
/* Quaternion                                                                */
/* ------------------------------------------------------------------------- */

Quat quat_normalize(Quat q) {
    f32 l2 = quat_dot(q, q);
    if (l2 < TG_TINY_F) { return quat_identity(); }
    return quat_scale(q, 1.0f / sqrtf(l2));
}

Quat quat_mul(Quat a, Quat b) {
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

Quat quat_from_axis_angle(V3 axis_unit, f32 angle) {
    Quat q;
    f32 h = angle * 0.5f;
    f32 s = sinf(h);
    V3 a = v3_norm_or(axis_unit, v3(0.0f, 1.0f, 0.0f));
    q.x = a.x * s;
    q.y = a.y * s;
    q.z = a.z * s;
    q.w = cosf(h);
    return q;
}

Quat quat_from_to(V3 from_unit, V3 to_unit) {
    Quat q;
    V3 c;
    f32 d = v3_dot(from_unit, to_unit);
    if (d >= 1.0f - TG_EPS_F) { return quat_identity(); }
    if (d <= -1.0f + TG_EPS_F) {
        /* 180 degrees: any perpendicular axis is valid; choose deterministically. */
        V3 axis = v3_any_perpendicular(from_unit);
        return quat_from_axis_angle(axis, TG_PI_F);
    }
    c = v3_cross(from_unit, to_unit);
    q.x = c.x;
    q.y = c.y;
    q.z = c.z;
    q.w = 1.0f + d;
    return quat_normalize(q);
}

V3 quat_rotate(Quat q, V3 v) {
    /* v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v) */
    V3 u = v3(q.x, q.y, q.z);
    V3 tmp = v3_add(v3_cross(u, v), v3_scale(v, q.w));
    return v3_add(v, v3_scale(v3_cross(u, tmp), 2.0f));
}

Quat quat_slerp(Quat a, Quat b, f32 t) {
    f32 d = quat_dot(a, b);
    f32 theta, s, sa, sb;
    Quat bb = b;
    if (d < 0.0f) {
        /* Take the shorter arc. */
        bb = quat_scale(b, -1.0f);
        d = -d;
    }
    if (d > 0.99995f) {
        Quat r;
        r.x = tg_lerpf(a.x, bb.x, t);
        r.y = tg_lerpf(a.y, bb.y, t);
        r.z = tg_lerpf(a.z, bb.z, t);
        r.w = tg_lerpf(a.w, bb.w, t);
        return quat_normalize(r);
    }
    theta = acosf(tg_clampf(d, -1.0f, 1.0f));
    s = sinf(theta);
    sa = sinf((1.0f - t) * theta) / s;
    sb = sinf(t * theta) / s;
    {
        Quat r;
        r.x = a.x * sa + bb.x * sb;
        r.y = a.y * sa + bb.y * sb;
        r.z = a.z * sa + bb.z * sb;
        r.w = a.w * sa + bb.w * sb;
        return quat_normalize(r);
    }
}

/* ------------------------------------------------------------------------- */
/* Matrices                                                                  */
/* ------------------------------------------------------------------------- */

M4 m4_identity(void) {
    M4 r;
    int i;
    for (i = 0; i < 16; ++i) { r.m[i] = 0.0f; }
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

M4 m4_mul(M4 a, M4 b) {
    M4 r;
    int col, row;
    for (col = 0; col < 4; ++col) {
        for (row = 0; row < 4; ++row) {
            f32 s = 0.0f;
            int k;
            for (k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

V4 m4_mul_v4(M4 a, V4 v) {
    V4 r;
    r.x = a.m[0] * v.x + a.m[4] * v.y + a.m[8]  * v.z + a.m[12] * v.w;
    r.y = a.m[1] * v.x + a.m[5] * v.y + a.m[9]  * v.z + a.m[13] * v.w;
    r.z = a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w;
    r.w = a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w;
    return r;
}

V3 m4_transform_point(M4 a, V3 p) {
    V4 h = m4_mul_v4(a, v4(p.x, p.y, p.z, 1.0f));
    if (tg_absf(h.w) > TG_TINY_F && tg_absf(h.w - 1.0f) > TG_EPS_F) {
        f32 inv = 1.0f / h.w;
        return v3(h.x * inv, h.y * inv, h.z * inv);
    }
    return v3(h.x, h.y, h.z);
}

V3 m4_transform_dir(M4 a, V3 d) {
    return v3(a.m[0] * d.x + a.m[4] * d.y + a.m[8]  * d.z,
              a.m[1] * d.x + a.m[5] * d.y + a.m[9]  * d.z,
              a.m[2] * d.x + a.m[6] * d.y + a.m[10] * d.z);
}

M4 m4_translation(V3 t) {
    M4 r = m4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

M4 m4_scale(V3 s) {
    M4 r = m4_identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

M4 m4_from_quat(Quat q) {
    M4 r = m4_identity();
    Quat u = quat_normalize(q);
    f32 xx = u.x * u.x, yy = u.y * u.y, zz = u.z * u.z;
    f32 xy = u.x * u.y, xz = u.x * u.z, yz = u.y * u.z;
    f32 wx = u.w * u.x, wy = u.w * u.y, wz = u.w * u.z;

    M4_AT(r, 0, 0) = 1.0f - 2.0f * (yy + zz);
    M4_AT(r, 1, 0) = 2.0f * (xy + wz);
    M4_AT(r, 2, 0) = 2.0f * (xz - wy);

    M4_AT(r, 0, 1) = 2.0f * (xy - wz);
    M4_AT(r, 1, 1) = 1.0f - 2.0f * (xx + zz);
    M4_AT(r, 2, 1) = 2.0f * (yz + wx);

    M4_AT(r, 0, 2) = 2.0f * (xz + wy);
    M4_AT(r, 1, 2) = 2.0f * (yz - wx);
    M4_AT(r, 2, 2) = 1.0f - 2.0f * (xx + yy);
    return r;
}

M4 m4_from_quat_translation(Quat q, V3 t) {
    M4 r = m4_from_quat(q);
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

M4 m4_transpose(M4 a) {
    M4 r;
    int row, col;
    for (col = 0; col < 4; ++col) {
        for (row = 0; row < 4; ++row) {
            r.m[col * 4 + row] = a.m[row * 4 + col];
        }
    }
    return r;
}

bool m4_inverse(M4 a, M4 *out) {
    const f32 *m = a.m;
    f32 inv[16];
    f32 det;
    int i;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (tg_absf(det) < 1e-20f || !tg_finitef(det)) { return false; }
    det = 1.0f / det;
    for (i = 0; i < 16; ++i) { out->m[i] = inv[i] * det; }
    return true;
}

M4 m4_inverse_rigid(M4 a) {
    M4 r = m4_identity();
    V3 t = v3(a.m[12], a.m[13], a.m[14]);
    V3 c0 = v3(a.m[0], a.m[1], a.m[2]);
    V3 c1 = v3(a.m[4], a.m[5], a.m[6]);
    V3 c2 = v3(a.m[8], a.m[9], a.m[10]);

    TG_ASSERT_MSG(tg_nearf(v3_len_sq(c0), 1.0f, 1e-3f) &&
                  tg_nearf(v3_len_sq(c1), 1.0f, 1e-3f) &&
                  tg_nearf(v3_len_sq(c2), 1.0f, 1e-3f),
                  "m4_inverse_rigid requires an orthonormal rotation part");

    /* R^T */
    M4_AT(r, 0, 0) = c0.x; M4_AT(r, 0, 1) = c0.y; M4_AT(r, 0, 2) = c0.z;
    M4_AT(r, 1, 0) = c1.x; M4_AT(r, 1, 1) = c1.y; M4_AT(r, 1, 2) = c1.z;
    M4_AT(r, 2, 0) = c2.x; M4_AT(r, 2, 1) = c2.y; M4_AT(r, 2, 2) = c2.z;
    /* -R^T * t */
    r.m[12] = -v3_dot(c0, t);
    r.m[13] = -v3_dot(c1, t);
    r.m[14] = -v3_dot(c2, t);
    return r;
}

M4 m4_look_at_rh(V3 eye, V3 target, V3 up) {
    M4 r = m4_identity();
    V3 fwd = v3_sub(target, eye);
    V3 f, s, u;

    if (v3_len_sq(fwd) < TG_TINY_F) {
        /* Degenerate: keep orientation identity, translate only. Returning NaNs
         * here would silently corrupt the whole frame. */
        return m4_translation(v3_neg(eye));
    }
    f = v3_norm_or(fwd, v3(0.0f, 0.0f, -1.0f));
    s = v3_cross(f, up);
    if (v3_len_sq(s) < TG_EPS_F) {
        /* up parallel to view direction: choose a deterministic substitute. */
        s = v3_cross(f, v3_any_perpendicular(f));
    }
    s = v3_norm_or(s, v3(1.0f, 0.0f, 0.0f));
    u = v3_cross(s, f);

    /* View basis rows are (s, u, -f); RH view space has -Z forward. */
    M4_AT(r, 0, 0) = s.x;  M4_AT(r, 0, 1) = s.y;  M4_AT(r, 0, 2) = s.z;
    M4_AT(r, 1, 0) = u.x;  M4_AT(r, 1, 1) = u.y;  M4_AT(r, 1, 2) = u.z;
    M4_AT(r, 2, 0) = -f.x; M4_AT(r, 2, 1) = -f.y; M4_AT(r, 2, 2) = -f.z;
    M4_AT(r, 0, 3) = -v3_dot(s, eye);
    M4_AT(r, 1, 3) = -v3_dot(u, eye);
    M4_AT(r, 2, 3) =  v3_dot(f, eye);
    return r;
}

/* Derivation for the reverse-Z RH forms is recorded here because getting the
 * sign conventions wrong is a classic silent depth failure.
 *
 * View space: the camera looks down -Z, so a visible point has z_v < 0. Let
 * d = -z_v > 0 be the positive view depth. We require
 *
 *     z_ndc(d = near) = 1     and     z_ndc(d = far) = 0.
 *
 * With w_clip = -z_v = d and z_clip = A*z_v + B, we get
 * z_ndc = (B - A*d)/d. Solving the two constraints gives
 *
 *     A = near / (far - near),        B = near*far / (far - near).
 *
 * As far -> infinity, A -> 0 and B -> near, giving z_ndc = near/d. */
M4 m4_perspective_reverse_z(f32 fov_y_radians, f32 aspect, f32 near_z, f32 far_z) {
    M4 r;
    f32 f, a, b;
    int i;

    TG_CHECK(near_z > 0.0f);
    TG_CHECK(far_z > near_z);
    TG_CHECK(aspect > 0.0f);
    TG_CHECK(fov_y_radians > 0.0f && fov_y_radians < TG_PI_F);

    for (i = 0; i < 16; ++i) { r.m[i] = 0.0f; }
    f = 1.0f / tanf(fov_y_radians * 0.5f);
    a = near_z / (far_z - near_z);
    b = near_z * far_z / (far_z - near_z);

    M4_AT(r, 0, 0) = f / aspect;
    M4_AT(r, 1, 1) = f;
    M4_AT(r, 2, 2) = a;
    M4_AT(r, 2, 3) = b;
    M4_AT(r, 3, 2) = -1.0f;
    return r;
}

M4 m4_perspective_reverse_z_infinite(f32 fov_y_radians, f32 aspect, f32 near_z) {
    M4 r;
    f32 f;
    int i;

    TG_CHECK(near_z > 0.0f);
    TG_CHECK(aspect > 0.0f);
    TG_CHECK(fov_y_radians > 0.0f && fov_y_radians < TG_PI_F);

    for (i = 0; i < 16; ++i) { r.m[i] = 0.0f; }
    f = 1.0f / tanf(fov_y_radians * 0.5f);

    M4_AT(r, 0, 0) = f / aspect;
    M4_AT(r, 1, 1) = f;
    M4_AT(r, 2, 2) = 0.0f;
    M4_AT(r, 2, 3) = near_z;
    M4_AT(r, 3, 2) = -1.0f;
    return r;
}

M4 m4_ortho_reverse_z(f32 half_width, f32 half_height, f32 near_z, f32 far_z) {
    M4 r = m4_identity();
    TG_CHECK(half_width > 0.0f && half_height > 0.0f);
    TG_CHECK(far_z > near_z);

    M4_AT(r, 0, 0) = 1.0f / half_width;
    M4_AT(r, 1, 1) = 1.0f / half_height;
    /* z_v in [-far, -near] maps to [0, 1] with near -> 1. */
    M4_AT(r, 2, 2) = 1.0f / (far_z - near_z);
    M4_AT(r, 2, 3) = far_z / (far_z - near_z);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Frames                                                                    */
/* ------------------------------------------------------------------------- */

Frame frame_make(V3 origin, V3 tangent, V3 reference_hint) {
    Frame f;
    V3 n;
    f.origin = origin;
    f.t = v3_norm_or(tangent, v3(0.0f, 1.0f, 0.0f));
    n = v3_reject_unit(reference_hint, f.t);
    if (v3_len_sq(n) < 1e-8f) {
        n = v3_any_perpendicular(f.t);
    }
    f.n = v3_norm_or(n, v3_any_perpendicular(f.t));
    f.b = v3_cross(f.t, f.n);
    /* Re-orthogonalise n from the exact cross product so the frame is
     * orthonormal to float precision even if the hint was nearly parallel. */
    f.b = v3_norm_or(f.b, v3_any_perpendicular(f.t));
    f.n = v3_cross(f.b, f.t);
    return f;
}

Frame frame_propagate_rmf(Frame prev, V3 next_origin, V3 next_tangent) {
    /* Double reflection method (Wang, Juttler, Zheng & Liu, ACM TOG 2008,
     * "Computation of rotation minimizing frames"), algorithm as published:
     *
     *   v1 = x_{i+1} - x_i
     *   R1 reflects in the plane with normal v1: applied to r_i and t_i
     *   v2 = t_{i+1} - t_i^L
     *   R2 reflects in the plane with normal v2: applied to r_i^L
     *
     * Reflections preserve orthonormality exactly in exact arithmetic and are
     * well conditioned in float, which is why this is preferred over
     * incremental rotate-by-minimal-angle.
     */
    Frame out;
    V3 t0 = prev.t;
    V3 r0 = prev.n;
    V3 v1, tL, rL, v2, t1;
    f32 c1, c2;

    t1 = v3_norm_or(next_tangent, t0);
    v1 = v3_sub(next_origin, prev.origin);
    c1 = v3_len_sq(v1);

    if (c1 < TG_TINY_F) {
        /* Coincident points: no translation, so only the tangent change matters.
         * Fall back to the minimal rotation carrying t0 to t1. */
        Quat q = quat_from_to(t0, t1);
        out.origin = next_origin;
        out.t = t1;
        out.n = v3_norm_or(v3_reject_unit(quat_rotate(q, prev.n), t1),
                           v3_any_perpendicular(t1));
        out.b = v3_cross(out.t, out.n);
        return out;
    }

    rL = v3_sub(r0, v3_scale(v1, (2.0f / c1) * v3_dot(v1, r0)));
    tL = v3_sub(t0, v3_scale(v1, (2.0f / c1) * v3_dot(v1, t0)));

    v2 = v3_sub(t1, tL);
    c2 = v3_len_sq(v2);
    if (c2 < TG_TINY_F) {
        /* tL already equals t1; the second reflection is the identity. */
        out.origin = next_origin;
        out.t = t1;
        out.n = v3_norm_or(v3_reject_unit(rL, t1), v3_any_perpendicular(t1));
        out.b = v3_cross(out.t, out.n);
        return out;
    }

    out.origin = next_origin;
    out.t = t1;
    out.n = v3_sub(rL, v3_scale(v2, (2.0f / c2) * v3_dot(v2, rL)));
    /* Re-orthonormalise against accumulated float drift. Over a long axis this
     * is what keeps cross-sections from slowly shearing. */
    out.n = v3_norm_or(v3_reject_unit(out.n, out.t), v3_any_perpendicular(out.t));
    out.b = v3_cross(out.t, out.n);
    return out;
}

Frame frame_twist(Frame f, f32 angle) {
    Frame out;
    f32 c = cosf(angle), s = sinf(angle);
    out.origin = f.origin;
    out.t = f.t;
    out.n = v3_add(v3_scale(f.n, c), v3_scale(f.b, s));
    out.b = v3_cross(out.t, out.n);
    out.n = v3_cross(out.b, out.t);
    return out;
}

bool frame_is_orthonormal(Frame f, f32 tol) {
    if (!v3_finite(f.t) || !v3_finite(f.n) || !v3_finite(f.b)) { return false; }
    if (!tg_nearf(v3_len(f.t), 1.0f, tol)) { return false; }
    if (!tg_nearf(v3_len(f.n), 1.0f, tol)) { return false; }
    if (!tg_nearf(v3_len(f.b), 1.0f, tol)) { return false; }
    if (!tg_nearf(v3_dot(f.t, f.n), 0.0f, tol)) { return false; }
    if (!tg_nearf(v3_dot(f.t, f.b), 0.0f, tol)) { return false; }
    if (!tg_nearf(v3_dot(f.n, f.b), 0.0f, tol)) { return false; }
    /* Handedness: cross(t,n) must equal b, not -b. A flipped frame would invert
     * triangle winding for every cross-section built from it. */
    {
        V3 c = v3_cross(f.t, f.n);
        if (v3_dot(c, f.b) < 0.0f) { return false; }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Ray / triangle                                                            */
/* ------------------------------------------------------------------------- */

V3 triangle_normal_unnormalized(V3 a, V3 b, V3 c) {
    return v3_cross(v3_sub(b, a), v3_sub(c, a));
}

bool ray_triangle(Ray r, V3 a, V3 b, V3 c, bool cull_backface,
                  f32 *out_t, f32 *out_u, f32 *out_v) {
    V3 e1 = v3_sub(b, a);
    V3 e2 = v3_sub(c, a);
    V3 pv = v3_cross(r.dir, e2);
    f32 det = v3_dot(e1, pv);
    f32 inv_det, u, vv, t;
    V3 tv, qv;

    if (cull_backface) {
        if (det < TG_TINY_F) { return false; }
    } else {
        if (tg_absf(det) < TG_TINY_F) { return false; }
    }

    inv_det = 1.0f / det;
    tv = v3_sub(r.origin, a);
    u = v3_dot(tv, pv) * inv_det;
    if (u < 0.0f || u > 1.0f) { return false; }

    qv = v3_cross(tv, e1);
    vv = v3_dot(r.dir, qv) * inv_det;
    if (vv < 0.0f || u + vv > 1.0f) { return false; }

    t = v3_dot(e2, qv) * inv_det;
    if (t < r.t_min || t > r.t_max) { return false; }

    if (out_t != NULL) { *out_t = t; }
    if (out_u != NULL) { *out_u = u; }
    if (out_v != NULL) { *out_v = vv; }
    return true;
}

bool ray_aabb(Ray r, Aabb box, f32 *out_t_near) {
    f32 tmin = r.t_min;
    f32 tmax = r.t_max;
    int axis;
    const f32 *bmn = &box.mn.x;
    const f32 *bmx = &box.mx.x;
    const f32 *o = &r.origin.x;
    const f32 *d = &r.dir.x;

    for (axis = 0; axis < 3; ++axis) {
        f32 t0, t1;
        if (tg_absf(d[axis]) < TG_TINY_F) {
            /* Ray parallel to this slab: reject only if the origin is outside. */
            if (o[axis] < bmn[axis] || o[axis] > bmx[axis]) { return false; }
            continue;
        }
        {
            f32 inv = 1.0f / d[axis];
            t0 = (bmn[axis] - o[axis]) * inv;
            t1 = (bmx[axis] - o[axis]) * inv;
        }
        if (t0 > t1) { f32 tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > tmin) { tmin = t0; }
        if (t1 < tmax) { tmax = t1; }
        if (tmin > tmax) { return false; }
    }
    if (out_t_near != NULL) { *out_t_near = tmin; }
    return true;
}
