/* hash.h -- deterministic 64-bit hashing and geometry fingerprints.
 *
 * Layer 0. Portable C17.
 *
 * Two distinct uses, deliberately separated:
 *
 *  1. tg_hash64_* : general byte/integer mixing. Used to derive RNG substreams
 *     and to key spatial buckets. Must be stable within a build; it is not a
 *     persisted format.
 *
 *  2. TgFingerprint / tg_fp_* : the geometry fingerprint used by the
 *     static-object regression test. Floats are canonicalised (-0.0 folded to
 *     +0.0) before hashing so that a legitimate signed-zero difference cannot
 *     produce a spurious "geometry changed" failure, and NaN is detected and
 *     reported rather than silently hashed.
 */
#ifndef TG_HASH_H
#define TG_HASH_H

#include "core_types.h"

#define TG_FNV64_OFFSET 0xcbf29ce484222325ull
#define TG_FNV64_PRIME  0x100000001b3ull

/* SplitMix64 finalizer. Strong avalanche, no state. */
static inline u64 tg_mix64(u64 z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline u64 tg_hash64_u64(u64 h, u64 v) {
    /* FNV-1a over the 8 bytes, then a mix pass so that low-entropy inputs such
     * as small sequential IDs still spread across the whole word. */
    int i;
    for (i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= TG_FNV64_PRIME;
    }
    return tg_mix64(h);
}

static inline u64 tg_hash64_u32(u64 h, u32 v) { return tg_hash64_u64(h, (u64)v); }

u64 tg_hash64_bytes(u64 h, const void *data, u64 size);
u64 tg_hash64_string(u64 h, const char *s);

/* --------------------------------------------------------------------------
 * Geometry fingerprint
 * -------------------------------------------------------------------------- */
typedef struct TgFingerprint {
    u64  value;
    u64  element_count; /* how many scalars/records were folded in         */
    bool saw_non_finite; /* true if any NaN or infinity was encountered    */
} TgFingerprint;

static inline TgFingerprint tg_fp_begin(u64 salt) {
    TgFingerprint f;
    f.value = tg_hash64_u64(TG_FNV64_OFFSET, salt);
    f.element_count = 0;
    f.saw_non_finite = false;
    return f;
}

/* Canonicalises the float, then folds its exact bit pattern in. Any non-finite
 * value sets saw_non_finite; it is still folded (as a fixed sentinel) so the
 * fingerprint remains well-defined. */
void tg_fp_add_f32(TgFingerprint *f, f32 v);
void tg_fp_add_u32(TgFingerprint *f, u32 v);
void tg_fp_add_u64(TgFingerprint *f, u64 v);
void tg_fp_add_bytes(TgFingerprint *f, const void *data, u64 size);
void tg_fp_add_f32_array(TgFingerprint *f, const f32 *v, u64 count);
void tg_fp_add_u32_array(TgFingerprint *f, const u32 *v, u64 count);

static inline bool tg_fp_equal(TgFingerprint a, TgFingerprint b) {
    return a.value == b.value && a.element_count == b.element_count;
}

#endif /* TG_HASH_H */
