#include "hash.h"

#include <string.h>

u64 tg_hash64_bytes(u64 h, const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u64 i;
    if (p == NULL) { return h; }
    for (i = 0; i < size; ++i) {
        h ^= p[i];
        h *= TG_FNV64_PRIME;
    }
    return tg_mix64(h);
}

u64 tg_hash64_string(u64 h, const char *s) {
    if (s == NULL) { return h; }
    return tg_hash64_bytes(h, s, (u64)strlen(s));
}

/* Sentinels folded in place of non-finite values so a fingerprint over a
 * corrupted mesh is still a well-defined, stable number that tests can compare,
 * while saw_non_finite records that the data was invalid. */
#define TG_FP_SENTINEL_NAN 0xFFFFFFFFu
#define TG_FP_SENTINEL_INF 0xFFFFFFFEu

void tg_fp_add_f32(TgFingerprint *f, f32 v) {
    u32 bits;
    TG_ASSERT(f != NULL);

    if (v != v) {
        f->saw_non_finite = true;
        bits = TG_FP_SENTINEL_NAN;
    } else if (v > 3.402823466e38f || v < -3.402823466e38f) {
        f->saw_non_finite = true;
        bits = TG_FP_SENTINEL_INF;
    } else {
        /* Fold -0.0 to +0.0. Signed zero is numerically equal but has a
         * different bit pattern, and it can legitimately appear or disappear
         * from an unrelated change in expression order. It must not be reported
         * as a geometry change. */
        if (v == 0.0f) { v = 0.0f; }
        memcpy(&bits, &v, sizeof bits);
    }
    f->value = tg_hash64_u32(f->value, bits);
    f->element_count++;
}

void tg_fp_add_u32(TgFingerprint *f, u32 v) {
    TG_ASSERT(f != NULL);
    f->value = tg_hash64_u32(f->value, v);
    f->element_count++;
}

void tg_fp_add_u64(TgFingerprint *f, u64 v) {
    TG_ASSERT(f != NULL);
    f->value = tg_hash64_u64(f->value, v);
    f->element_count++;
}

void tg_fp_add_bytes(TgFingerprint *f, const void *data, u64 size) {
    TG_ASSERT(f != NULL);
    f->value = tg_hash64_bytes(f->value, data, size);
    f->element_count++;
}

void tg_fp_add_f32_array(TgFingerprint *f, const f32 *v, u64 count) {
    u64 i;
    TG_ASSERT(f != NULL);
    if (v == NULL) { return; }
    for (i = 0; i < count; ++i) { tg_fp_add_f32(f, v[i]); }
}

void tg_fp_add_u32_array(TgFingerprint *f, const u32 *v, u64 count) {
    u64 i;
    TG_ASSERT(f != NULL);
    if (v == NULL) { return; }
    for (i = 0; i < count; ++i) { tg_fp_add_u32(f, v[i]); }
}
