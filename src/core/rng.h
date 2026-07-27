/* rng.h -- deterministic pseudo-random number generation.
 *
 * Layer 0. Portable C17.
 *
 * DETERMINISM CONTRACT
 * --------------------
 * The engine must reproduce identical geometry for identical inputs, including
 * when stages run in parallel. Two mechanisms make that possible, and using
 * them is mandatory:
 *
 *  1. SUBSTREAMS, NOT A GLOBAL STREAM. Randomness for an organ is drawn from a
 *     stream derived from (root_seed, purpose, stable_id). Because the stream
 *     depends only on immutable identity, the values an organ receives do not
 *     depend on how many organs were processed before it, or on which thread
 *     processed it. tg_rng_substream is the only sanctioned way to obtain a
 *     generator inside a parallel stage.
 *
 *  2. PURPOSE TAGS. Every draw site declares what it is for. Two different
 *     properties of the same organ therefore consume independent streams. This
 *     is what allows a bark parameter to be added later without shifting every
 *     subsequent random value in the tree -- a change that would otherwise
 *     silently alter every previously generated tree for the same seed.
 *
 * The generator is PCG-XSH-RR 64/32 (O'Neill). Chosen over a Mersenne Twister
 * for tiny state, over rand() because rand() is implementation-defined and
 * therefore useless for a reproducibility contract, and over xorshift because
 * PCG's output function passes stronger statistical tests at this size.
 */
#ifndef TG_RNG_H
#define TG_RNG_H

#include "core_types.h"
#include "math3d.h"

typedef struct TgRng {
    u64 state;
    u64 inc; /* stream selector; always odd */
} TgRng;

/* Purpose tags. Add new tags at the END so that existing seeds keep producing
 * the same trees. Never reorder or remove. */
typedef enum TgRngPurpose {
    TG_RNG_GLOBAL = 0,
    TG_RNG_TRUNK_FORM,
    TG_RNG_TRUNK_LEAN,
    TG_RNG_CROWN_ENVELOPE,
    TG_RNG_ATTRACTOR_CLOUD,
    TG_RNG_BUD_PLACEMENT,
    TG_RNG_BUD_FATE,
    TG_RNG_SHOOT_EXTENSION,
    TG_RNG_SHOOT_DIRECTION,
    TG_RNG_BRANCH_ANGLE,
    TG_RNG_INTERNODE_LENGTH,
    TG_RNG_MORTALITY,
    TG_RNG_RADIUS_JITTER,
    TG_RNG_CROSS_SECTION,
    TG_RNG_MECHANICS,
    TG_RNG_ROOT_INITIATION,
    TG_RNG_ROOT_DIRECTION,
    TG_RNG_ROOT_BRANCHING,
    TG_RNG_BARK_FIELD,
    TG_RNG_BARK_PLATE,
    TG_RNG_BARK_PEEL,
    TG_RNG_LEAF_PLACEMENT,
    TG_RNG_LEAF_SHAPE,
    TG_RNG_LEAF_VENATION,
    TG_RNG_LEAF_DAMAGE,
    TG_RNG_NEEDLE_PLACEMENT,
    TG_RNG_NEEDLE_SHAPE,
    TG_RNG_DAMAGE_EVENTS,
    TG_RNG_WOUND_FORM,
    TG_RNG_KNOT_FORM,
    TG_RNG_MATERIAL_VARIATION,
    TG_RNG_PURPOSE_COUNT
} TgRngPurpose;

const char *tg_rng_purpose_name(TgRngPurpose p);

/* Direct seeding. Use for a single serial stream (for example a test). */
void tg_rng_seed(TgRng *r, u64 seed, u64 stream);

/* The sanctioned derivation. `id` is a stable identity (organ id, vertex index,
 * work-item index); `sub` further distinguishes multiple independent draws for
 * the same (purpose, id) pair -- pass 0 when there is only one. */
TgRng tg_rng_substream(u64 root_seed, TgRngPurpose purpose, u32 id, u32 sub);

u32 tg_rng_u32(TgRng *r);
u64 tg_rng_u64(TgRng *r);

/* Uniform in [0,1). Exactly representable: uses the top 24 bits, so the result
 * is a multiple of 2^-24 and can never round to 1.0f. */
f32 tg_rng_f32(TgRng *r);

/* Uniform in [lo, hi). Returns lo when hi <= lo rather than producing a
 * reversed or NaN range. */
f32 tg_rng_range(TgRng *r, f32 lo, f32 hi);

/* Uniform in [-1, 1). */
f32 tg_rng_signed(TgRng *r);

/* Unbiased integer in [0, n). Uses Lemire's multiply-shift with rejection;
 * modulo would bias low values, which is visible as directional bias in
 * phyllotaxis and attractor sampling. Returns 0 for n == 0. */
u32 tg_rng_below(TgRng *r, u32 n);

bool tg_rng_chance(TgRng *r, f32 probability);

/* Standard normal, Marsaglia polar method. Stateless across calls (both
 * variates are generated but only one returned) so that determinism does not
 * depend on call parity. */
f32 tg_rng_normal(TgRng *r);

/* Normal clamped to +/- `max_sigma` standard deviations. Used wherever an
 * unbounded tail would produce a biologically impossible value. */
f32 tg_rng_normal_clamped(TgRng *r, f32 mean, f32 stddev, f32 max_sigma);

/* Uniformly distributed on the unit sphere (Marsaglia). */
V3 tg_rng_unit_sphere(TgRng *r);

/* Uniform inside the unit ball, by radius warping rather than rejection so the
 * number of draws is fixed and parallel-safe. */
V3 tg_rng_in_unit_ball(TgRng *r);

/* Uniform direction within `max_angle` of unit `axis`. Correct solid-angle
 * distribution (cosine of the polar angle sampled uniformly), not a naive
 * uniform-angle sample which would over-concentrate near the axis. */
V3 tg_rng_cone(TgRng *r, V3 axis, f32 max_angle);

#endif /* TG_RNG_H */
