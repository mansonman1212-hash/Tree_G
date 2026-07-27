/* core_types.h -- fundamental types, results, checked arithmetic.
 *
 * Layer 0. Portable C17. Must not include any platform header.
 */
#ifndef TG_CORE_TYPES_H
#define TG_CORE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;

/* Sentinel for "no organ / no index". Chosen as U32_MAX so that an accidental
 * use as an array index is caught immediately by bounds assertions rather than
 * silently reading element 0. */
#define TG_INVALID_ID 0xFFFFFFFFu

/* ------------------------------------------------------------------------- */
/* Result codes.                                                             */
/*                                                                           */
/* Every fallible function returns TgResult and writes outputs through        */
/* pointers. TG_OK is zero so `if (r) { handle error }` is correct.           */
/* ------------------------------------------------------------------------- */
typedef enum TgResult {
    TG_OK = 0,
    TG_ERR_OUT_OF_MEMORY,
    TG_ERR_OVERFLOW,
    TG_ERR_INVALID_ARGUMENT,
    TG_ERR_INVALID_STATE,
    TG_ERR_LIMIT_EXCEEDED,
    TG_ERR_VALIDATION_FAILED,
    TG_ERR_CANCELLED,
    TG_ERR_NOT_FOUND,
    TG_ERR_NOT_SUPPORTED,
    TG_ERR_IO,
    TG_ERR_PLATFORM,
    TG_RESULT_COUNT
} TgResult;

const char *tg_result_name(TgResult r);

/* ------------------------------------------------------------------------- */
/* Assertions.                                                               */
/*                                                                           */
/* TG_ASSERT is active in debug builds only and documents an invariant the    */
/* architecture is supposed to make impossible. It is never used to validate  */
/* external input -- that is what TgResult is for.                           */
/*                                                                           */
/* TG_CHECK is always active and aborts. Reserved for conditions where        */
/* continuing would corrupt memory.                                          */
/* ------------------------------------------------------------------------- */
void tg_assert_fail(const char *expr, const char *file, int line, const char *msg);

#define TG_CHECK(expr)                                                        \
    do {                                                                      \
        if (!(expr)) {                                                        \
            tg_assert_fail(#expr, __FILE__, __LINE__, NULL);                  \
        }                                                                     \
    } while (0)

#define TG_CHECK_MSG(expr, msg)                                               \
    do {                                                                      \
        if (!(expr)) {                                                        \
            tg_assert_fail(#expr, __FILE__, __LINE__, (msg));                 \
        }                                                                     \
    } while (0)

#if defined(TG_DEBUG) && TG_DEBUG
#define TG_ASSERT(expr)          TG_CHECK(expr)
#define TG_ASSERT_MSG(expr, msg) TG_CHECK_MSG(expr, msg)
#else
#define TG_ASSERT(expr)          ((void)0)
#define TG_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define TG_STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

/* ------------------------------------------------------------------------- */
/* Small utilities.                                                          */
/* ------------------------------------------------------------------------- */
#define TG_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define TG_UNUSED(x)  ((void)(x))

#if defined(_MSC_VER)
#define TG_NOINLINE __declspec(noinline)
#define TG_FORCEINLINE __forceinline
#else
#define TG_NOINLINE __attribute__((noinline))
#define TG_FORCEINLINE __attribute__((always_inline)) inline
#endif

static inline u32 tg_min_u32(u32 a, u32 b) { return a < b ? a : b; }
static inline u32 tg_max_u32(u32 a, u32 b) { return a > b ? a : b; }
static inline u64 tg_min_u64(u64 a, u64 b) { return a < b ? a : b; }
static inline u64 tg_max_u64(u64 a, u64 b) { return a > b ? a : b; }
static inline i32 tg_min_i32(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 tg_max_i32(i32 a, i32 b) { return a > b ? a : b; }

static inline u32 tg_clamp_u32(u32 v, u32 lo, u32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline i32 tg_clamp_i32(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------------- */
/* Checked arithmetic.                                                       */
/*                                                                           */
/* Directive requires every multiplication/addition that can overflow to be   */
/* checked. Every geometry count and byte size goes through these.           */
/* ------------------------------------------------------------------------- */
static inline bool tg_ckd_add_u64(u64 a, u64 b, u64 *out) {
    u64 s = a + b;
    if (s < a) { return false; }
    *out = s;
    return true;
}

static inline bool tg_ckd_mul_u64(u64 a, u64 b, u64 *out) {
    if (a != 0 && b > UINT64_MAX / a) { return false; }
    *out = a * b;
    return true;
}

static inline bool tg_ckd_add_u32(u32 a, u32 b, u32 *out) {
    u64 s = (u64)a + (u64)b;
    if (s > UINT32_MAX) { return false; }
    *out = (u32)s;
    return true;
}

static inline bool tg_ckd_mul_u32(u32 a, u32 b, u32 *out) {
    u64 p = (u64)a * (u64)b;
    if (p > UINT32_MAX) { return false; }
    *out = (u32)p;
    return true;
}

/* Round `v` up to a power-of-two `align`. Returns false on overflow. */
static inline bool tg_align_up_u64(u64 v, u64 align, u64 *out) {
    u64 sum;
    TG_ASSERT(align != 0 && (align & (align - 1)) == 0);
    if (!tg_ckd_add_u64(v, align - 1, &sum)) { return false; }
    *out = sum & ~(align - 1);
    return true;
}

static inline bool tg_is_power_of_two_u64(u64 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

#endif /* TG_CORE_TYPES_H */
