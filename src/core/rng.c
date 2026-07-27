#include "rng.h"

#include "hash.h"

#define PCG_MULT 6364136223846793005ull

const char *tg_rng_purpose_name(TgRngPurpose p) {
    switch (p) {
    case TG_RNG_GLOBAL:             return "global";
    case TG_RNG_TRUNK_FORM:         return "trunk_form";
    case TG_RNG_TRUNK_LEAN:         return "trunk_lean";
    case TG_RNG_CROWN_ENVELOPE:     return "crown_envelope";
    case TG_RNG_ATTRACTOR_CLOUD:    return "attractor_cloud";
    case TG_RNG_BUD_PLACEMENT:      return "bud_placement";
    case TG_RNG_BUD_FATE:           return "bud_fate";
    case TG_RNG_SHOOT_EXTENSION:    return "shoot_extension";
    case TG_RNG_SHOOT_DIRECTION:    return "shoot_direction";
    case TG_RNG_BRANCH_ANGLE:       return "branch_angle";
    case TG_RNG_INTERNODE_LENGTH:   return "internode_length";
    case TG_RNG_MORTALITY:          return "mortality";
    case TG_RNG_RADIUS_JITTER:      return "radius_jitter";
    case TG_RNG_CROSS_SECTION:      return "cross_section";
    case TG_RNG_MECHANICS:          return "mechanics";
    case TG_RNG_ROOT_INITIATION:    return "root_initiation";
    case TG_RNG_ROOT_DIRECTION:     return "root_direction";
    case TG_RNG_ROOT_BRANCHING:     return "root_branching";
    case TG_RNG_BARK_FIELD:         return "bark_field";
    case TG_RNG_BARK_PLATE:         return "bark_plate";
    case TG_RNG_BARK_PEEL:          return "bark_peel";
    case TG_RNG_LEAF_PLACEMENT:     return "leaf_placement";
    case TG_RNG_LEAF_SHAPE:         return "leaf_shape";
    case TG_RNG_LEAF_VENATION:      return "leaf_venation";
    case TG_RNG_LEAF_DAMAGE:        return "leaf_damage";
    case TG_RNG_NEEDLE_PLACEMENT:   return "needle_placement";
    case TG_RNG_NEEDLE_SHAPE:       return "needle_shape";
    case TG_RNG_DAMAGE_EVENTS:      return "damage_events";
    case TG_RNG_WOUND_FORM:         return "wound_form";
    case TG_RNG_KNOT_FORM:          return "knot_form";
    case TG_RNG_MATERIAL_VARIATION: return "material_variation";
    case TG_RNG_PURPOSE_COUNT:      break;
    }
    return "unknown";
}

void tg_rng_seed(TgRng *r, u64 seed, u64 stream) {
    TG_CHECK(r != NULL);
    r->state = 0u;
    r->inc = (stream << 1u) | 1u; /* must be odd for a full-period stream */
    (void)tg_rng_u32(r);
    r->state += seed;
    (void)tg_rng_u32(r);
}

TgRng tg_rng_substream(u64 root_seed, TgRngPurpose purpose, u32 id, u32 sub) {
    TgRng r;
    u64 h = TG_FNV64_OFFSET;
    /* Mix all four components so that neighbouring ids do not produce
     * correlated streams -- a real hazard when ids are sequential and the
     * stream selector is used directly. */
    h = tg_hash64_u64(h, root_seed);
    h = tg_hash64_u32(h, (u32)purpose);
    h = tg_hash64_u32(h, id);
    h = tg_hash64_u32(h, sub);
    tg_rng_seed(&r, h, tg_mix64(h ^ 0xD1B54A32D192ED03ull));
    return r;
}

u32 tg_rng_u32(TgRng *r) {
    u64 old = r->state;
    u32 xorshifted, rot;
    TG_ASSERT(r != NULL);
    r->state = old * PCG_MULT + r->inc;
    /* PCG-XSH-RR output function. */
    xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
    rot = (u32)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

u64 tg_rng_u64(TgRng *r) {
    u64 hi = (u64)tg_rng_u32(r);
    u64 lo = (u64)tg_rng_u32(r);
    return (hi << 32) | lo;
}

f32 tg_rng_f32(TgRng *r) {
    /* Top 24 bits scaled by 2^-24: uniform on the 24-bit grid in [0,1), and
     * provably never equal to 1.0f. */
    u32 v = tg_rng_u32(r) >> 8;
    return (f32)v * (1.0f / 16777216.0f);
}

f32 tg_rng_range(TgRng *r, f32 lo, f32 hi) {
    if (!(hi > lo)) { return lo; }
    return lo + (hi - lo) * tg_rng_f32(r);
}

f32 tg_rng_signed(TgRng *r) { return tg_rng_f32(r) * 2.0f - 1.0f; }

u32 tg_rng_below(TgRng *r, u32 n) {
    /* Lemire, "Fast Random Integer Generation in an Interval". The rejection
     * threshold removes modulo bias exactly. */
    u32 x, l;
    u64 m;
    if (n == 0) { return 0; }
    x = tg_rng_u32(r);
    m = (u64)x * (u64)n;
    l = (u32)m;
    if (l < n) {
        u32 t = (u32)(-(i32)n) % n; /* 2^32 mod n */
        while (l < t) {
            x = tg_rng_u32(r);
            m = (u64)x * (u64)n;
            l = (u32)m;
        }
    }
    return (u32)(m >> 32);
}

bool tg_rng_chance(TgRng *r, f32 probability) {
    if (probability <= 0.0f) { return false; }
    if (probability >= 1.0f) { return true; }
    return tg_rng_f32(r) < probability;
}

f32 tg_rng_normal(TgRng *r) {
    /* Marsaglia polar method. Loop is bounded in practice (acceptance ~78.5%);
     * a hard iteration cap guarantees termination even on a pathological stream. */
    int guard;
    for (guard = 0; guard < 64; ++guard) {
        f32 u = tg_rng_signed(r);
        f32 v = tg_rng_signed(r);
        f32 s = u * u + v * v;
        if (s > 0.0f && s < 1.0f) {
            f32 f = sqrtf(-2.0f * logf(s) / s);
            return u * f;
        }
    }
    return 0.0f;
}

f32 tg_rng_normal_clamped(TgRng *r, f32 mean, f32 stddev, f32 max_sigma) {
    f32 z = tg_rng_normal(r);
    if (max_sigma > 0.0f) { z = tg_clampf(z, -max_sigma, max_sigma); }
    return mean + z * stddev;
}

V3 tg_rng_unit_sphere(TgRng *r) {
    /* Sample z uniformly in [-1,1] and the azimuth uniformly: this is the exact
     * uniform distribution on the sphere (Archimedes' theorem), with no
     * rejection, so the number of draws is fixed and parallel-safe. */
    f32 z = tg_rng_signed(r);
    f32 phi = tg_rng_f32(r) * TG_TAU_F;
    f32 rr = sqrtf(tg_maxf(0.0f, 1.0f - z * z));
    return v3(rr * cosf(phi), rr * sinf(phi), z);
}

V3 tg_rng_in_unit_ball(TgRng *r) {
    V3 d = tg_rng_unit_sphere(r);
    /* r = u^(1/3) gives uniform volume density. */
    f32 u = tg_rng_f32(r);
    f32 rad = powf(u, 1.0f / 3.0f);
    return v3_scale(d, rad);
}

V3 tg_rng_cone(TgRng *r, V3 axis, f32 max_angle) {
    V3 a = v3_norm_or(axis, v3(0.0f, 1.0f, 0.0f));
    V3 n, b;
    f32 cos_max, cos_theta, sin_theta, phi;

    if (max_angle <= 0.0f) { return a; }
    if (max_angle >= TG_PI_F) { return tg_rng_unit_sphere(r); }

    /* Uniform in solid angle: cos(theta) uniform in [cos(max), 1]. */
    cos_max = cosf(max_angle);
    cos_theta = tg_lerpf(cos_max, 1.0f, tg_rng_f32(r));
    sin_theta = sqrtf(tg_maxf(0.0f, 1.0f - cos_theta * cos_theta));
    phi = tg_rng_f32(r) * TG_TAU_F;

    n = v3_any_perpendicular(a);
    b = v3_cross(a, n);
    return v3_norm_or(
        v3_add(v3_scale(a, cos_theta),
               v3_add(v3_scale(n, sin_theta * cosf(phi)),
                      v3_scale(b, sin_theta * sinf(phi)))),
        a);
}
