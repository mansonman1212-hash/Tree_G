/* camera.h -- inspection camera. Pure math, no device state.
 *
 * Layer 1, deliberately NOT in the render layer. The project requires proof that
 * the finalised tree never changes while the camera moves, and that proof is
 * only possible without a GPU if the camera is portable and testable. Nothing in
 * this file can reach a mesh: it takes an Aabb by value and returns matrices.
 *
 * PERSPECTIVE POLICY
 *   Default vertical field of view is 42 degrees, inside the restrained
 *   inspection range the project mandates. The adjustable range is clamped to
 *   [20, 75] degrees so that an accidental extreme value cannot produce the
 *   fisheye "melting" look, and framing works by moving the camera to a correct
 *   distance rather than by widening the lens.
 *
 * DEPTH POLICY
 *   Reverse-Z with an infinite far plane by default. This removes far-plane
 *   clipping as a failure mode entirely while putting the best floating-point
 *   precision nearest the camera, which is where close bark inspection happens.
 *
 * ORIENTATION POLICY
 *   Yaw and pitch, with pitch hard-clamped just inside the poles. This cannot
 *   gimbal-flip and cannot accumulate drift, which an incrementally multiplied
 *   quaternion can. Rotations are still built through quaternions to keep the
 *   basis exactly orthonormal.
 */
#ifndef TG_CAMERA_H
#define TG_CAMERA_H

#include "../core/math3d.h"

typedef enum CameraMode {
    /* Orbits a fixed pivot. The default for inspecting a single object. */
    CAMERA_MODE_ORBIT = 0,
    /* Free flight: WASD plus mouse look. For getting inside a crown. */
    CAMERA_MODE_FREE,
    CAMERA_MODE_COUNT
} CameraMode;

typedef enum CameraProjection {
    CAMERA_PROJ_PERSPECTIVE = 0,
    CAMERA_PROJ_ORTHOGRAPHIC,
    CAMERA_PROJ_COUNT
} CameraProjection;

#define CAMERA_FOV_MIN_DEG      20.0f
#define CAMERA_FOV_MAX_DEG      75.0f
#define CAMERA_FOV_DEFAULT_DEG  42.0f
/* Pitch is clamped this far from vertical. Large enough that the orbit basis
 * stays well conditioned, small enough to allow a near-top-down root view. */
#define CAMERA_PITCH_LIMIT      1.5533430f /* 89 degrees in radians */

typedef struct Camera {
    CameraMode       mode;
    CameraProjection projection;

    V3  pivot;      /* orbit centre, world space                             */
    f32 distance;   /* orbit radius                                          */
    f32 yaw;        /* radians, 0 looks along -Z                             */
    f32 pitch;      /* radians, clamped to +/- CAMERA_PITCH_LIMIT            */

    V3  free_position; /* used in CAMERA_MODE_FREE                           */

    f32 fov_y;      /* radians, clamped to the range above                   */
    f32 near_z;
    f32 far_z;      /* used only when use_infinite_far is false              */
    bool use_infinite_far;

    f32 ortho_half_height;

    /* Scene scale, set by camera_set_scene_bounds. Drives movement speed, the
     * near plane and zoom limits, so that inspecting a 0.4 m seedling and a 30 m
     * mature tree both feel the same. Without this, a fixed speed makes one
     * unusable and the other glacial. */
    f32 scene_scale;

    f32 move_speed_scale;   /* user multiplier, default 1                    */
    f32 look_sensitivity;   /* radians per unit of pointer delta             */

    f32 aspect;             /* width / height                                */
} Camera;

/* Initialises to the documented defaults with an identity-safe state: valid
 * matrices even before any bounds are supplied. */
void camera_init(Camera *c);

/* Records the scene scale used for adaptive speed and clipping. Safe to call
 * with an empty box (falls back to unit scale). Does NOT move the camera. */
void camera_set_scene_bounds(Camera *c, Aabb bounds);

void camera_set_aspect(Camera *c, u32 pixel_width, u32 pixel_height);
/* Clamps into the safe range and returns the value actually applied. */
f32  camera_set_fov_degrees(Camera *c, f32 degrees);

/* Current eye position, derived for orbit mode and stored for free mode. */
V3 camera_position(const Camera *c);
/* Unit basis of the camera in world space. */
V3 camera_forward(const Camera *c);
V3 camera_right(const Camera *c);
V3 camera_up(const Camera *c);

M4 camera_view_matrix(const Camera *c);
M4 camera_projection_matrix(const Camera *c);
M4 camera_view_projection(const Camera *c);

/* --------------------------------------------------------------------------
 * Interaction. Every function takes normalised input deltas and applies the
 * adaptive scaling internally, so callers never hard-code a speed.
 * -------------------------------------------------------------------------- */

/* Orbit by pointer delta. In free mode this is mouse-look instead. */
void camera_orbit(Camera *c, f32 dx, f32 dy);
/* Pans in the camera's screen plane. Distance-proportional so the grabbed point
 * tracks the pointer at any zoom level. */
void camera_pan(Camera *c, f32 dx, f32 dy);
/* Moves toward or away from the pivot. Multiplicative, so zoom feels uniform
 * across scales, and bounded by the scene scale rather than by a hard-coded
 * limit that would make close inspection impossible. */
void camera_dolly(Camera *c, f32 steps);
/* Free-flight translation in camera space; `dt` in seconds. */
void camera_move(Camera *c, V3 local_dir, f32 dt, bool fast);

/* Places the camera so `bounds` is fully visible with `margin` (0.1 = 10% slack),
 * without changing the field of view. Preserves the current yaw/pitch so framing
 * does not jump the viewing angle. */
void camera_frame_bounds(Camera *c, Aabb bounds, f32 margin);
/* Frames while also choosing a pleasant default viewing angle. Used by the
 * "frame whole tree", "frame roots" and "frame crown" commands. */
void camera_frame_bounds_reset_angle(Camera *c, Aabb bounds, f32 margin,
                                     f32 yaw, f32 pitch);
void camera_reset(Camera *c, Aabb bounds);

/* Switches mode while preserving the apparent viewpoint, so toggling does not
 * teleport the user. */
void camera_set_mode(Camera *c, CameraMode mode);

/* Builds a picking ray through a pixel. `px`,`py` are pixel centres in a
 * `width` x `height` viewport with the origin at the TOP-LEFT, matching the
 * platform convention. */
Ray camera_pick_ray(const Camera *c, f32 px, f32 py, u32 width, u32 height);

/* True when every corner of `bounds` projects inside the view frustum. Used by
 * the framing regression test. */
bool camera_bounds_fully_visible(const Camera *c, Aabb bounds);

#endif /* TG_CAMERA_H */
