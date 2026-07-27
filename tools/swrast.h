/* swrast.h -- CPU rasteriser used ONLY for validation captures.
 *
 * THIS IS A TEST INSTRUMENT AND IS NOT PART OF THE APPLICATION.
 *
 * The shipped renderer is Direct3D 12. This exists for one reason: the project
 * directive requires visual quality to be judged visually, and the development
 * environment has no GPU and no display. Without it, every statement about how
 * the tree LOOKS would be an unverifiable claim, and several open questions
 * (crown density, junction quality, bark relief) cannot be settled numerically.
 *
 * It deliberately shares the engine's conventions rather than inventing its own,
 * so that a capture exercises the same camera, the same reverse-Z depth ordering
 * and the same winding rule the real renderer will use. A convention error is
 * therefore visible here instead of surviving until Windows.
 *
 * It is NOT a second renderer: no shadows, no global illumination, no tone
 * mapping beyond a documented curve, and no claim of physical accuracy.
 */
#ifndef TG_SWRAST_H
#define TG_SWRAST_H

#include "../src/geom/camera.h"
#include "../src/geom/mesh.h"

typedef struct SwLight {
    V3  direction;      /* unit, direction the light travels TOWARD the scene */
    V3  colour;         /* linear radiance-ish, arbitrary units               */
    V3  sky_colour;     /* hemispheric ambient from above                     */
    V3  ground_colour;  /* hemispheric ambient from below                     */
} SwLight;

typedef struct SwTarget {
    u32  width, height;
    f32 *colour;   /* width*height*3, linear                                  */
    f32 *depth;    /* width*height, reverse-Z: GREATER is nearer              */
    u64  colour_bytes;
    u64  depth_bytes;
    V3   background_top;
    V3   background_bottom;
    /* Counters, so a capture that renders nothing says so instead of silently
     * producing an empty image that gets mistaken for a modelling failure. */
    u64  triangles_submitted;
    u64  triangles_clipped;
    u64  triangles_culled;
    u64  fragments_written;
} SwTarget;

TgResult sw_target_create(SwTarget *t, u32 width, u32 height);
void     sw_target_destroy(SwTarget *t);
void     sw_target_clear(SwTarget *t);

SwLight  sw_default_light(void);

/* Draws one mesh section with per-vertex normals and colours. Backface culling
 * follows the section's declared expectation, so an inside-out solid shows as
 * black holes here exactly as it would on the GPU. */
TgResult sw_draw_mesh_section(SwTarget *t, const Camera *cam, const Mesh *m,
                              MeshSection section, const SwLight *light);

/* Draws all used sections. */
TgResult sw_draw_mesh(SwTarget *t, const Camera *cam, const Mesh *m,
                      const SwLight *light);

/* Depth-tested line, for skeleton and debug overlays. */
void sw_draw_line(SwTarget *t, const Camera *cam, V3 a, V3 b, V3 colour);
/* Depth-tested screen-space point of the given pixel radius. */
void sw_draw_point(SwTarget *t, const Camera *cam, V3 p, f32 radius_px, V3 colour);

/* Wireframe overlay of a section, drawn slightly toward the camera so it is not
 * z-fighting with the surface it describes. */
TgResult sw_draw_wireframe(SwTarget *t, const Camera *cam, const Mesh *m,
                           MeshSection section, V3 colour);

/* Converts the linear buffer to 8-bit sRGB. `exposure` scales linear radiance
 * before tone mapping. The curve is a plain Reinhard followed by the sRGB
 * transfer function -- named honestly, not called "filmic". */
TgResult sw_resolve_srgb(const SwTarget *t, f32 exposure, u8 *out_rgb);

/* Renders the depth buffer as a visualisation, so depth errors are diagnosable
 * rather than merely suspected. Near is bright. */
TgResult sw_resolve_depth(const SwTarget *t, u8 *out_rgb);

#endif /* TG_SWRAST_H */
