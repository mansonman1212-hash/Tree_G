#include "test_util.h"
#include "test_suites.h"

#include "../src/core/log.h"
#include "../src/core/mem.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

u32 tg_test_checks;
u32 tg_test_failures;
u32 tg_test_cases;
const char *tg_test_case_name = "<none>";

static const char *g_current_suite = "<none>";
static u32 g_suite_failures_at_start;

void tg_test_case(const char *name) {
    tg_test_case_name = name != NULL ? name : "<unnamed>";
    tg_test_cases++;
}

void tg_test_report_failure(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    tg_test_failures++;
    fprintf(stderr, "FAIL  %s / %s\n      %s:%d\n      ",
            g_current_suite, tg_test_case_name, file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int tg_test_summary(void) {
    printf("\n----------------------------------------------------------\n");
    printf("cases: %u   checks: %u   failures: %u\n",
           tg_test_cases, tg_test_checks, tg_test_failures);
    if (tg_test_failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return 1;
}

typedef void (*SuiteFn)(void);

typedef struct Suite {
    const char *name;
    SuiteFn fn;
} Suite;

static void run_suite(const Suite *s) {
    g_current_suite = s->name;
    g_suite_failures_at_start = tg_test_failures;
    printf("== %-22s ", s->name);
    fflush(stdout);
    s->fn();
    if (tg_test_failures == g_suite_failures_at_start) {
        printf("ok\n");
    } else {
        printf("FAILED (%u)\n", tg_test_failures - g_suite_failures_at_start);
    }
}

/* Suppress engine log output below WARN during tests so that expected-failure
 * paths do not bury the test report, while still surfacing anything unexpected. */
static bool g_logs_muted;

static void quiet_sink(TgLogLevel level, const char *subsystem, const char *line,
                       void *user) {
    TG_UNUSED(user);
    if (g_logs_muted) { return; }
    if (level >= TG_LOG_WARN) {
        fprintf(stderr, "      [log %d] %s: %s\n", (int)level, subsystem, line);
    }
}

void tg_test_logs_mute(void) { g_logs_muted = true; }
void tg_test_logs_unmute(void) { g_logs_muted = false; }

int main(int argc, char **argv) {
    static const Suite suites[] = {
        { "core/mem",        test_suite_mem },
        { "core/math3d",     test_suite_math },
        { "core/rng",        test_suite_rng },
        { "core/hash",       test_suite_hash },
    };
    const char *filter = NULL;
    size_t i;
    int rc;
    TgAllocStats stats;

    if (argc > 1) { filter = argv[1]; }

    tg_log_init(TG_LOG_WARN);
    tg_log_set_sink(quiet_sink, NULL);
    tg_mem_reset_stats();

    printf("Tree_G portable test suite\n");
    printf("----------------------------------------------------------\n");

    for (i = 0; i < TG_COUNTOF(suites); ++i) {
        if (filter != NULL && strstr(suites[i].name, filter) == NULL) { continue; }
        run_suite(&suites[i]);
    }

    /* Leak gate: every suite must release everything it allocated. This catches
     * a whole class of ownership defects at zero cost. */
    stats = tg_mem_stats();
    g_current_suite = "harness";
    tg_test_case("no leaked allocations");
    TG_EXPECT_MSG(stats.live_bytes == 0,
                  "%llu bytes still live after all suites",
                  (unsigned long long)stats.live_bytes);

    rc = tg_test_summary();
    printf("peak CPU bytes during tests: %llu\n",
           (unsigned long long)stats.peak_bytes);
    return rc;
}
