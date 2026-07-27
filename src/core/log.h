/* log.h -- diagnostic logging.
 *
 * Layer 0. Portable C17.
 *
 * Every failure message must identify subsystem, operation, and code, per the
 * project's diagnostics requirement. TG_LOG_FAIL exists to make that the path
 * of least resistance.
 */
#ifndef TG_LOG_H
#define TG_LOG_H

#include "core_types.h"

typedef enum TgLogLevel {
    TG_LOG_TRACE = 0,
    TG_LOG_INFO,
    TG_LOG_WARN,
    TG_LOG_ERROR,
    TG_LOG_LEVEL_COUNT
} TgLogLevel;

/* Sink receives fully formatted lines without a trailing newline. */
typedef void (*TgLogSink)(TgLogLevel level, const char *subsystem,
                          const char *line, void *user);

void tg_log_init(TgLogLevel min_level);
void tg_log_set_sink(TgLogSink sink, void *user);
void tg_log_set_min_level(TgLogLevel min_level);
TgLogLevel tg_log_min_level(void);

/* Total lines emitted at TG_LOG_WARN or above since init. Used by tests to
 * assert that a "clean" run really was clean. */
u64 tg_log_warning_count(void);
u64 tg_log_error_count(void);
void tg_log_reset_counters(void);

void tg_logf(TgLogLevel level, const char *subsystem, const char *fmt, ...);

/* Structured failure line: subsystem, operation, result code, detail, and
 * whether recoverable state survives. */
void tg_log_failure(const char *subsystem, const char *operation, TgResult code,
                    bool previous_state_intact, const char *fmt, ...);

#define TG_LOG_TRACEF(sub, ...) tg_logf(TG_LOG_TRACE, (sub), __VA_ARGS__)
#define TG_LOG_INFOF(sub, ...)  tg_logf(TG_LOG_INFO, (sub), __VA_ARGS__)
#define TG_LOG_WARNF(sub, ...)  tg_logf(TG_LOG_WARN, (sub), __VA_ARGS__)
#define TG_LOG_ERRORF(sub, ...) tg_logf(TG_LOG_ERROR, (sub), __VA_ARGS__)

#endif /* TG_LOG_H */
