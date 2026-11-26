#ifndef D2D_DEBUG_LOG_H
#define D2D_DEBUG_LOG_H

/*
 * Single build switch for all developer diagnostics (viewer, litedocx,
 * litehtml). Release builds leave D2D_DEBUG_LOGS at 0: the macros below
 * expand to ((void)0), so arguments are not evaluated at all. No string
 * building, no formatting and no console I/O are left in production.
 */
#ifndef D2D_DEBUG_LOGS
#define D2D_DEBUG_LOGS 0
#endif

#ifndef TIMING_LOGS
#define TIMING_LOGS 0
#endif

#include <stdio.h>

#if D2D_DEBUG_LOGS
#define DLOG(fmt, ...) do { fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); } while (0)
#define DLOGSTR(msg)   do { fputs(msg, stderr); fflush(stderr); } while (0)
#if defined(_WIN32)
#define DLOG_W(msg)    do { OutputDebugStringA(msg); } while (0)
#else
#define DLOG_W(msg)    ((void)0)
#endif
#else
#define DLOG(fmt, ...) ((void)0)
#define DLOGSTR(msg)   ((void)0)
#define DLOG_W(msg)    ((void)0)
#endif

#if TIMING_LOGS
#define TLOG(fmt, ...) do { fprintf(stderr, "[TIMING] " fmt, ##__VA_ARGS__); fflush(stderr); } while (0)
#else
#define TLOG(fmt, ...) ((void)0)
#endif

#endif /* D2D_DEBUG_LOG_H */
