#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Log state is process-global and single-writer by convention: the generation
 * worker and the UI thread both log, so writes to the counters are benign
 * races on counters only used for diagnostics. Deliberately not atomic to keep
 * Layer 0 free of platform threading headers; documented as diagnostic-only. */
static TgLogLevel g_min_level = TG_LOG_INFO;
static TgLogSink  g_sink;
static void      *g_sink_user;
static u64        g_warn_count;
static u64        g_error_count;

static const char *level_tag(TgLogLevel level) {
    switch (level) {
    case TG_LOG_TRACE: return "TRACE";
    case TG_LOG_INFO:  return "INFO ";
    case TG_LOG_WARN:  return "WARN ";
    case TG_LOG_ERROR: return "ERROR";
    case TG_LOG_LEVEL_COUNT: break;
    }
    return "?????";
}

const char *tg_result_name(TgResult r) {
    switch (r) {
    case TG_OK:                     return "TG_OK";
    case TG_ERR_OUT_OF_MEMORY:      return "TG_ERR_OUT_OF_MEMORY";
    case TG_ERR_OVERFLOW:           return "TG_ERR_OVERFLOW";
    case TG_ERR_INVALID_ARGUMENT:   return "TG_ERR_INVALID_ARGUMENT";
    case TG_ERR_INVALID_STATE:      return "TG_ERR_INVALID_STATE";
    case TG_ERR_LIMIT_EXCEEDED:     return "TG_ERR_LIMIT_EXCEEDED";
    case TG_ERR_VALIDATION_FAILED:  return "TG_ERR_VALIDATION_FAILED";
    case TG_ERR_CANCELLED:          return "TG_ERR_CANCELLED";
    case TG_ERR_NOT_FOUND:          return "TG_ERR_NOT_FOUND";
    case TG_ERR_NOT_SUPPORTED:      return "TG_ERR_NOT_SUPPORTED";
    case TG_ERR_IO:                 return "TG_ERR_IO";
    case TG_ERR_PLATFORM:           return "TG_ERR_PLATFORM";
    case TG_RESULT_COUNT:           break;
    }
    return "TG_ERR_UNKNOWN";
}

void tg_assert_fail(const char *expr, const char *file, int line, const char *msg) {
    fflush(stdout);
    if (msg != NULL) {
        fprintf(stderr, "\nFATAL: assertion failed: %s\n  at %s:%d\n  %s\n",
                expr, file, line, msg);
    } else {
        fprintf(stderr, "\nFATAL: assertion failed: %s\n  at %s:%d\n",
                expr, file, line);
    }
    fflush(stderr);
    abort();
}

void tg_log_init(TgLogLevel min_level) {
    g_min_level  = min_level;
    g_sink       = NULL;
    g_sink_user  = NULL;
    g_warn_count = 0;
    g_error_count = 0;
}

void tg_log_set_sink(TgLogSink sink, void *user) {
    g_sink = sink;
    g_sink_user = user;
}

void tg_log_set_min_level(TgLogLevel min_level) { g_min_level = min_level; }
TgLogLevel tg_log_min_level(void) { return g_min_level; }
u64 tg_log_warning_count(void) { return g_warn_count; }
u64 tg_log_error_count(void) { return g_error_count; }
void tg_log_reset_counters(void) { g_warn_count = 0; g_error_count = 0; }

static void emit(TgLogLevel level, const char *subsystem, const char *line) {
    if (level >= TG_LOG_ERROR) {
        g_error_count++;
    } else if (level >= TG_LOG_WARN) {
        g_warn_count++;
    }
    if (level < g_min_level) { return; }
    if (g_sink != NULL) {
        g_sink(level, subsystem, line, g_sink_user);
    } else {
        FILE *out = (level >= TG_LOG_WARN) ? stderr : stdout;
        fprintf(out, "[%s] %-12s %s\n", level_tag(level), subsystem, line);
    }
}

void tg_logf(TgLogLevel level, const char *subsystem, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    int n;

    if (subsystem == NULL) { subsystem = "?"; }
    if (fmt == NULL) { fmt = ""; }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        /* Encoding failure: report it rather than dropping the message. */
        memcpy(buf, "<log format error>", sizeof "<log format error>");
    }
    emit(level, subsystem, buf);
}

void tg_log_failure(const char *subsystem, const char *operation, TgResult code,
                    bool previous_state_intact, const char *fmt, ...) {
    char detail[768];
    char line[1024];
    va_list ap;

    detail[0] = '\0';
    if (fmt != NULL && fmt[0] != '\0') {
        va_start(ap, fmt);
        (void)vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }

    (void)snprintf(line, sizeof line,
                   "operation='%s' code=%s previous_tree=%s%s%s",
                   operation != NULL ? operation : "?",
                   tg_result_name(code),
                   previous_state_intact ? "intact" : "none",
                   detail[0] != '\0' ? " detail=" : "",
                   detail);
    emit(TG_LOG_ERROR, subsystem != NULL ? subsystem : "?", line);
}
