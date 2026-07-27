/* test_util.h -- minimal assertion harness for the portable test suite.
 *
 * Design goals, in order:
 *  - A failing check reports the expression, the file, the line, and the actual
 *    values. A bare "assertion failed" is useless during regression triage.
 *  - Checks do NOT abort. One run reports every failure in every suite, so a
 *    single build/run cycle gives the full picture.
 *  - No dependency on the engine beyond Layer 0.
 */
#ifndef TG_TEST_UTIL_H
#define TG_TEST_UTIL_H

#include "../src/core/core_types.h"
#include "../src/core/math3d.h"

#include <stdio.h>
#include <string.h>

extern u32 tg_test_checks;
extern u32 tg_test_failures;
extern u32 tg_test_cases;
extern const char *tg_test_case_name;

void tg_test_case(const char *name);
void tg_test_report_failure(const char *file, int line, const char *fmt, ...);
int  tg_test_summary(void);

/* Silences engine log output around a test that deliberately triggers an error
 * path. Muting is explicit and scoped so that an UNEXPECTED warning still
 * reaches the report -- the harness must never hide diagnostics by default. */
void tg_test_logs_mute(void);
void tg_test_logs_unmute(void);

#define TG_T_CASE(name) tg_test_case(name)

#define TG_EXPECT(cond)                                                        \
    do {                                                                       \
        tg_test_checks++;                                                       \
        if (!(cond)) {                                                          \
            tg_test_report_failure(__FILE__, __LINE__, "expected: %s", #cond);   \
        }                                                                       \
    } while (0)

#define TG_EXPECT_MSG(cond, ...)                                               \
    do {                                                                       \
        tg_test_checks++;                                                       \
        if (!(cond)) {                                                          \
            tg_test_report_failure(__FILE__, __LINE__, __VA_ARGS__);             \
        }                                                                       \
    } while (0)

#define TG_EXPECT_EQ_U64(a, b)                                                 \
    do {                                                                       \
        u64 ta_ = (u64)(a), tb_ = (u64)(b);                                     \
        tg_test_checks++;                                                        \
        if (ta_ != tb_) {                                                        \
            tg_test_report_failure(__FILE__, __LINE__,                           \
                "%s == %s : got %llu, expected %llu", #a, #b,                    \
                (unsigned long long)ta_, (unsigned long long)tb_);               \
        }                                                                       \
    } while (0)

#define TG_EXPECT_EQ_I64(a, b)                                                 \
    do {                                                                       \
        i64 ta_ = (i64)(a), tb_ = (i64)(b);                                     \
        tg_test_checks++;                                                        \
        if (ta_ != tb_) {                                                        \
            tg_test_report_failure(__FILE__, __LINE__,                           \
                "%s == %s : got %lld, expected %lld", #a, #b,                    \
                (long long)ta_, (long long)tb_);                                 \
        }                                                                       \
    } while (0)

#define TG_EXPECT_NEAR(a, b, tol)                                              \
    do {                                                                       \
        f64 ta_ = (f64)(a), tb_ = (f64)(b), tt_ = (f64)(tol);                   \
        f64 d_ = ta_ - tb_;                                                      \
        if (d_ < 0.0) { d_ = -d_; }                                              \
        tg_test_checks++;                                                        \
        if (!(d_ <= tt_)) {                                                      \
            tg_test_report_failure(__FILE__, __LINE__,                           \
                "%s ~= %s : got %.9g, expected %.9g (|d|=%.3g > %.3g)",          \
                #a, #b, ta_, tb_, d_, tt_);                                      \
        }                                                                       \
    } while (0)

#define TG_EXPECT_V3_NEAR(a, b, tol)                                           \
    do {                                                                       \
        V3 va_ = (a), vb_ = (b);                                                \
        TG_EXPECT_NEAR(va_.x, vb_.x, tol);                                       \
        TG_EXPECT_NEAR(va_.y, vb_.y, tol);                                       \
        TG_EXPECT_NEAR(va_.z, vb_.z, tol);                                       \
    } while (0)

#define TG_EXPECT_OK(expr)                                                     \
    do {                                                                       \
        TgResult r_ = (expr);                                                   \
        tg_test_checks++;                                                        \
        if (r_ != TG_OK) {                                                       \
            tg_test_report_failure(__FILE__, __LINE__, "%s returned %s", #expr,   \
                                   tg_result_name(r_));                          \
        }                                                                       \
    } while (0)

#define TG_EXPECT_ERR(expr, expected)                                          \
    do {                                                                       \
        TgResult r_ = (expr);                                                   \
        tg_test_checks++;                                                        \
        if (r_ != (expected)) {                                                  \
            tg_test_report_failure(__FILE__, __LINE__,                           \
                "%s returned %s, expected %s", #expr, tg_result_name(r_),        \
                tg_result_name(expected));                                       \
        }                                                                       \
    } while (0)

#endif /* TG_TEST_UTIL_H */
