/* math3d.h -- vector, matrix, quaternion and frame mathematics.
 *
 * Layer 0. Portable C17.
 *
 * BINDING CONVENTIONS (see docs/research.md 7.2). Violating any of these is a
 * defect, not a style difference:
 *
 *   World         right-handed, +Y up, -Z forward, metres.
 *   Matrix store  column-major: m[col*4 + row]. M4_AT(m,row,col) accesses it.
 *   Vector convention  column vectors, transform is M * v.
 *   Composition   clip = P * V * M * v_model.
 *   Quaternion    (x,y,z,w), unit, right-handed, rotates as q*v*conj(q).
 *   Angles        radians. Degrees appear only at UI boundaries.
 *   Clip depth    reverse-Z: near maps to 1, far maps to 0.
 *   Front face    counter-clockwise when seen from outside the solid.
 */
#ifndef TG_MATH3D_H
#define TG_MATH3D_H

#include "core_types.h"

#include <math.h>

#define TG_PI        3.14159265358979323846
#define TG_PI_F      3.14159265358979323846f
#define TG_TAU_F     6.28318530717958647692f
#define TG_DEG2RAD_F 0.01745329251994329577f
#define TG_RAD2DEG_F 57.2957795130823208768f

/* Golden angle, the divergence of spiral phyllotaxis: 360 * (1 - 1/phi).
 * Value in degrees is 137.50776405003785; kept in radians here. */
#define TG_GOLDEN_ANGLE_F 2.39996322972865332f

/* Tolerances. TG_EPS_F is a general float comparison slack; TG_TINY_F is the
 * threshold below which a length is treated as degenerate. Both are chosen for
 * geometry in metres at tree scale (1e-3 m to 1e2 m). */
#define TG_EPS_F  1e-6f
#define TG_TINY_F 1e-12f

/* ------------------------------------------------------------------------- */
/* Scalars                                                                   */
/* ------------------------------------------------------------------------- */
static inline f32 tg_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 tg_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 tg_absf(f32 a) { return a < 0.0f ? -a : a; }
static inline f32 tg_signf(f32 a) { return a < 0.0f ? -1.0f : 1.0f; }

static inline f32 tg_clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline f32 tg_saturatef(f32 v) { return tg_clampf(v, 0.0f, 1.0f); }
static inline f32 tg_lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

/* Maps v from [a,b] to [0,1], clamped. Returns 0 for a degenerate interval. */
static inline f32 tg_remap01f(f32 v, f32 a, f32 b) {
    f32 d = b - a;
    if (tg_absf(d) < TG_TINY_F) { return 0.0f; }
    return tg_saturatef((v - a) / d);
}

static inline f32 tg_smoothstepf(f32 edge0, f32 edge1, f32 v) {
    f32 t = tg_remap01f(v, edge0, edge1);
    return t * t * (3.0f - 2.0f * t);
}

/* Smootherstep: C2 continuous. Used where a bark or collar profile must not
 * show a shading crease at the blend boundary. */
static inline f32 tg_smootherstepf(f32 edge0, f32 edge1, f32 v) {
    f32 t = tg_remap01f(v, edge0, edge1);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline bool tg_finitef(f32 v) {
    /* isfinite() is a macro that may not accept float cleanly under /W4; the
     * explicit comparison avoids the warning and is exact for IEEE-754. */
    return !(v != v) && v <= 3.402823466e38f && v >= -3.402823466e38f;
}

static inline bool tg_nearf(f32 a, f32 b, f32 tol) { return tg_absf(a - b) <= tol; }

/* Wraps an angle to (-pi, pi]. */
static inline f32 tg_wrap_pi(f32 a) {
    f32 t = fmodf(a + TG_PI_F, TG_TAU_F);
    if (t < 0.0f) { t += TG_TAU_F; }
    return t - TG_PI_F;
}

/* ------------------------------------------------------------------------- */
/* Vectors                                                                   */
/* ------------------------------------------------------------------------- */
typedef struct V2 { f32 x, y; } V2;
typedef struct V3 { f32 x, y, z; } V3;
typedef struct V4 { f32 x, y, z, w; } V4;

TG_STATIC_ASSERT(sizeof(V3) == 12, "V3 must be tightly packed for vertex upload");
TG_STATIC_ASSERT(sizeof(V4) == 16, "V4 must be tightly packed");

static inline V2 v2(f32 x, f32 y) { V2 r; r.x = x; r.y = y; return r; }
static inline V3 v3(f32 x, f32 y, f32 z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
static inline V4 v4(f32 x, f32 y, f32 z, f32 w) { V4 r; r.x=x; r.y=y; r.z=z; r.w=w; return r; }

static inline V3 v3_splat(f32 s) { return v3(s, s, s); }
static inline V3 v3_zero(void) { return v3(0.0f, 0.0f, 0.0f); }
static inline V3 v3_up(void) { return v3(0.0f, 1.0f, 0.0f); }

static inline V2 v2_add(V2 a, V2 b) { return v2(a.x + b.x, a.y + b.y); }
static inline V2 v2_sub(V2 a, V2 b) { return v2(a.x - b.x, a.y - b.y); }
static inline V2 v2_scale(V2 a, f32 s) { return v2(a.x * s, a.y * s); }
static inline f32 v2_dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
static inline f32 v2_cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
static inline f32 v2_len_sq(V2 a) { return v2_dot(a, a); }
static inline f32 v2_len(V2 a) { return sqrtf(v2_dot(a, a)); }
static inline V2 v2_lerp(V2 a, V2 b, f32 t) {
    return v2(tg_lerpf(a.x, b.x, t), tg_lerpf(a.y, b.y, t));
}
static inline V2 v2_perp(V2 a) { return v2(-a.y, a.x); }
static inline V2 v2_norm_or(V2 a, V2 fallback) {
    f32 l2 = v2_len_sq(a);
    if (l2 < TG_TINY_F) { return fallback; }
    return v2_scale(a, 1.0f / sqrtf(l2));
}

static inline V3 v3_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3_mul(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline V3 v3_scale(V3 a, f32 s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 v3_neg(V3 a) { return v3(-a.x, -a.y, -a.z); }
static inline f32 v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3_cross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}
static inline f32 v3_len_sq(V3 a) { return v3_dot(a, a); }
static inline f32 v3_len(V3 a) { return sqrtf(v3_dot(a, a)); }
static inline f32 v3_dist(V3 a, V3 b) { return v3_len(v3_sub(a, b)); }
static inline f32 v3_dist_sq(V3 a, V3 b) { return v3_len_sq(v3_sub(a, b)); }
static inline V3 v3_lerp(V3 a, V3 b, f32 t) {
    return v3(tg_lerpf(a.x, b.x, t), tg_lerpf(a.y, b.y, t), tg_lerpf(a.z, b.z, t));
}
static inline V3 v3_min(V3 a, V3 b) {
    return v3(tg_minf(a.x, b.x), tg_minf(a.y, b.y), tg_minf(a.z, b.z));
}
static inline V3 v3_max(V3 a, V3 b) {
    return v3(tg_maxf(a.x, b.x), tg_maxf(a.y, b.y), tg_maxf(a.z, b.z));
}
static inline f32 v3_max_component(V3 a) { return tg_maxf(a.x, tg_maxf(a.y, a.z)); }
static inline bool v3_finite(V3 a) {
    return tg_finitef(a.x) && tg_finitef(a.y) && tg_finitef(a.z);
}

/* Normalise, or return `fallback` if the input is degenerate. There is no
 * unchecked normalise in this engine: silently producing NaN from a zero-length
 * growth direction is exactly the class of defect the project forbids. */
static inline V3 v3_norm_or(V3 a, V3 fallback) {
    f32 l2 = v3_len_sq(a);
    if (l2 < TG_TINY_F) { return fallback; }
    return v3_scale(a, 1.0f / sqrtf(l2));
}

/* Component of `a` orthogonal to unit vector `n`. */
static inline V3 v3_reject_unit(V3 a, V3 n) {
    return v3_sub(a, v3_scale(n, v3_dot(a, n)));
}

/* Reflect `a` in the plane through the origin with unit normal `n`. */
static inline V3 v3_reflect_plane(V3 a, V3 n) {
    return v3_sub(a, v3_scale(n, 2.0f * v3_dot(a, n)));
}

/* Any unit vector perpendicular to unit `n`, chosen deterministically to avoid
 * catastrophic cancellation. */
V3 v3_any_perpendicular(V3 n);

/* Rotate `v` about unit axis `axis` by `angle` radians (Rodrigues). */
V3 v3_rotate_axis(V3 v, V3 axis, f32 angle);

/* Interpolates direction along the shortest arc. Inputs must be unit. Falls
 * back to linear interpolation when nearly parallel. */
V3 v3_slerp_unit(V3 a, V3 b, f32 t);

/* Rotates unit `v` toward unit `target` by at most `max_angle` radians. This is
 * the primitive used for bounded tropism, so that a strong light or gravity
 * response can never produce an unbounded direction change in one step. */
V3 v3_rotate_toward(V3 v, V3 target, f32 max_angle);

/* Unsigned angle between two non-degenerate vectors, numerically stable near
 * 0 and pi (uses atan2 of cross/dot rather than acos of a clamped dot). */
f32 v3_angle_between(V3 a, V3 b);

/* ------------------------------------------------------------------------- */
/* Axis-aligned bounding box                                                 */
/* ------------------------------------------------------------------------- */
typedef struct Aabb { V3 mn, mx; } Aabb;

static inline Aabb aabb_empty(void) {
    Aabb b;
    b.mn = v3_splat(3.402823466e38f);
    b.mx = v3_splat(-3.402823466e38f);
    return b;
}
static inline bool aabb_is_empty(Aabb b) {
    return b.mn.x > b.mx.x || b.mn.y > b.mx.y || b.mn.z > b.mx.z;
}
static inline Aabb aabb_add_point(Aabb b, V3 p) {
    b.mn = v3_min(b.mn, p);
    b.mx = v3_max(b.mx, p);
    return b;
}
static inline Aabb aabb_union(Aabb a, Aabb b) {
    if (aabb_is_empty(a)) { return b; }
    if (aabb_is_empty(b)) { return a; }
    a.mn = v3_min(a.mn, b.mn);
    a.mx = v3_max(a.mx, b.mx);
    return a;
}
static inline V3 aabb_center(Aabb b) { return v3_scale(v3_add(b.mn, b.mx), 0.5f); }
static inline V3 aabb_extent(Aabb b) { return v3_sub(b.mx, b.mn); }
static inline f32 aabb_diagonal(Aabb b) {
    if (aabb_is_empty(b)) { return 0.0f; }
    return v3_len(aabb_extent(b));
}
static inline f32 aabb_surface_area(Aabb b) {
    V3 e;
    if (aabb_is_empty(b)) { return 0.0f; }
    e = aabb_extent(b);
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}
static inline Aabb aabb_expand(Aabb b, f32 r) {
    if (aabb_is_empty(b)) { return b; }
    b.mn = v3_sub(b.mn, v3_splat(r));
    b.mx = v3_add(b.mx, v3_splat(r));
    return b;
}
static inline bool aabb_contains(Aabb b, V3 p) {
    return p.x >= b.mn.x && p.x <= b.mx.x &&
           p.y >= b.mn.y && p.y <= b.mx.y &&
           p.z >= b.mn.z && p.z <= b.mx.z;
}
static inline bool aabb_overlaps(Aabb a, Aabb b) {
    return a.mn.x <= b.mx.x && a.mx.x >= b.mn.x &&
           a.mn.y <= b.mx.y && a.mx.y >= b.mn.y &&
           a.mn.z <= b.mx.z && a.mx.z >= b.mn.z;
}

/* ------------------------------------------------------------------------- */
/* Quaternion                                                                */
/* ------------------------------------------------------------------------- */
typedef struct Quat { f32 x, y, z, w; } Quat;

static inline Quat quat_identity(void) { Quat q; q.x=q.y=q.z=0.0f; q.w=1.0f; return q; }
static inline f32 quat_dot(Quat a, Quat b) {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}
static inline Quat quat_conj(Quat q) { Quat r; r.x=-q.x; r.y=-q.y; r.z=-q.z; r.w=q.w; return r; }
static inline Quat quat_scale(Quat q, f32 s) {
    Quat r; r.x=q.x*s; r.y=q.y*s; r.z=q.z*s; r.w=q.w*s; return r;
}
static inline f32 quat_len(Quat q) { return sqrtf(quat_dot(q, q)); }

Quat quat_normalize(Quat q);
Quat quat_mul(Quat a, Quat b);
Quat quat_from_axis_angle(V3 axis_unit, f32 angle);
/* Shortest-arc rotation taking unit `from` to unit `to`. Handles the
 * antiparallel case explicitly instead of producing a zero quaternion. */
Quat quat_from_to(V3 from_unit, V3 to_unit);
V3   quat_rotate(Quat q, V3 v);
Quat quat_slerp(Quat a, Quat b, f32 t);

/* ------------------------------------------------------------------------- */
/* 4x4 matrix, column-major                                                  */
/* ------------------------------------------------------------------------- */
typedef struct M4 { f32 m[16]; } M4;

/* row in [0,4), col in [0,4) */
#define M4_AT(mat, row, col) ((mat).m[(col) * 4 + (row)])

M4  m4_identity(void);
M4  m4_mul(M4 a, M4 b);                 /* returns a * b                     */
V4  m4_mul_v4(M4 a, V4 v);              /* a * v                             */
V3  m4_transform_point(M4 a, V3 p);     /* a * (p,1), divides by w if needed */
V3  m4_transform_dir(M4 a, V3 d);       /* upper-left 3x3 only               */
M4  m4_translation(V3 t);
M4  m4_scale(V3 s);
M4  m4_from_quat(Quat q);
M4  m4_from_quat_translation(Quat q, V3 t);
M4  m4_transpose(M4 a);
/* General inverse via cofactors. Returns false and leaves *out untouched when
 * the matrix is singular (|det| below tolerance). */
bool m4_inverse(M4 a, M4 *out);
/* Inverse of a rigid transform (rotation + translation only). Cheap and exact;
 * asserts orthonormality in debug builds. */
M4  m4_inverse_rigid(M4 a);

/* Right-handed look-at view matrix. `up` need not be orthogonal to the view
 * direction; it is orthogonalised. Returns identity-with-translation if eye and
 * target coincide, rather than producing NaNs. */
M4  m4_look_at_rh(V3 eye, V3 target, V3 up);

/* Reverse-Z right-handed perspective. Maps view-space depth `near` to clip
 * z/w = 1 and `far` to 0. Requires 0 < near < far. */
M4  m4_perspective_reverse_z(f32 fov_y_radians, f32 aspect, f32 near_z, f32 far_z);

/* Reverse-Z right-handed perspective with an infinite far plane: depth `near`
 * maps to 1 and depth -> infinity maps to 0. Preferred for inspection because
 * it removes the far-plane clipping failure mode entirely while keeping the
 * best float precision near the camera. */
M4  m4_perspective_reverse_z_infinite(f32 fov_y_radians, f32 aspect, f32 near_z);

/* Reverse-Z right-handed orthographic projection. */
M4  m4_ortho_reverse_z(f32 half_width, f32 half_height, f32 near_z, f32 far_z);

/* ------------------------------------------------------------------------- */
/* Frames along a curve                                                      */
/* ------------------------------------------------------------------------- */

/* An orthonormal frame attached to a point on an axis.
 *
 *   t  unit tangent, pointing distally (toward the tip)
 *   n  unit normal, the reference direction for cross-section angle 0
 *   b  unit binormal = cross(t, n)
 *
 * (n, b, t) is right-handed, so a cross-section generated as
 * n*cos(theta) + b*sin(theta) winds counter-clockwise when viewed from +t,
 * which yields outward-facing triangles under the project's CCW-front rule.
 */
typedef struct Frame {
    V3 origin;
    V3 t, n, b;
} Frame;

/* Builds a frame at `origin` with tangent `t`, choosing `n` deterministically.
 * Use only for the first frame of an axis; subsequent frames must be
 * propagated with frame_propagate_rmf so that the axis does not twist. */
Frame frame_make(V3 origin, V3 tangent, V3 reference_hint);

/* Rotation-minimising frame propagation by the double reflection method of
 * Wang, Juttler, Zheng & Liu (ACM TOG 2008). Two reflections carry the frame
 * from `prev` to the new point, producing a frame with no twist about the
 * tangent. Frenet frames are not used anywhere in this engine: they are
 * undefined at zero curvature and flip at inflections, both of which occur
 * constantly on tree axes.
 *
 * `next_tangent` need not be unit; it is normalised, falling back to the
 * previous tangent if degenerate. */
Frame frame_propagate_rmf(Frame prev, V3 next_origin, V3 next_tangent);

/* Rotates a frame about its own tangent. Used to apply a deliberate, recorded
 * twist (for example torsion in a wind-shaped trunk) on top of the
 * twist-free RMF, so that any rotation in the mesh is intentional. */
Frame frame_twist(Frame f, f32 angle);

/* Point on the unit circle of the frame's cross-section plane at angle theta,
 * scaled by radius, offset from origin. */
static inline V3 frame_ring_point(Frame f, f32 theta, f32 radius) {
    f32 c = cosf(theta), s = sinf(theta);
    return v3_add(f.origin,
                  v3_add(v3_scale(f.n, c * radius), v3_scale(f.b, s * radius)));
}

/* Direction (unit) in the cross-section plane at angle theta. */
static inline V3 frame_ring_dir(Frame f, f32 theta) {
    f32 c = cosf(theta), s = sinf(theta);
    return v3_add(v3_scale(f.n, c), v3_scale(f.b, s));
}

/* Angle of `dir` measured in the frame's cross-section plane, in (-pi, pi]. */
static inline f32 frame_angle_of(Frame f, V3 dir) {
    return atan2f(v3_dot(dir, f.b), v3_dot(dir, f.n));
}

bool frame_is_orthonormal(Frame f, f32 tol);

/* ------------------------------------------------------------------------- */
/* Ray / triangle                                                            */
/* ------------------------------------------------------------------------- */
typedef struct Ray { V3 origin, dir; f32 t_min, t_max; } Ray;

/* Watertight-ish Moller-Trumbore. `cull_backface` selects single-sided testing,
 * which picking uses for solid wood and disables for thin leaf blades.
 * Writes barycentric (u,v) and distance on hit. */
bool ray_triangle(Ray r, V3 a, V3 b, V3 c, bool cull_backface,
                  f32 *out_t, f32 *out_u, f32 *out_v);

/* Slab test. Returns the parametric entry distance in *out_t_near. */
bool ray_aabb(Ray r, Aabb box, f32 *out_t_near);

/* Newell's method: robust for non-planar polygons and returns a vector whose
 * length is twice the signed area. */
V3 triangle_normal_unnormalized(V3 a, V3 b, V3 c);
static inline f32 triangle_area(V3 a, V3 b, V3 c) {
    return 0.5f * v3_len(triangle_normal_unnormalized(a, b, c));
}

#endif /* TG_MATH3D_H */
