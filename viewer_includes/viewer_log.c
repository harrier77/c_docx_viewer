/*
 * viewer_log.c - logging su file e diagnostica console.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

#if ENABLE_FILE_LOGGING
static void init_log_path(void)
{
    if (g_log_path[0]) return;
    GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
    char *p = strrchr(g_log_path, '\\');
    if (p) p[1] = '\0';
    else g_log_path[0] = '\0';
    strcat(g_log_path, "logs.txt");
}
#endif

static void log_msg(const char *fmt, ...)
{
#if !ENABLE_FILE_LOGGING
    return;
#else
    if (!g_log_file) {
        init_log_path();
        g_log_file = fopen(g_log_path, "a");
        if (!g_log_file) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fflush(g_log_file);
#endif
}

/* Fine-grained switch, active only when CONSOLE_LOGGING is 1.
 * 0 = only the essential render-lifecycle message stays visible.
 * 1 = full table-layout trace set (diagnosis of the "first column text
 * in second column" bug; see REGRESSION WARNING below). */
#define TABLE_DEBUG 0

static void dbg_log(const char *fmt, ...)
{
#if CONSOLE_LOGGING && TABLE_DEBUG
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[DBG] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
#else
    (void)fmt;
#endif
}

#define LOG_JOB(kind, msg) log_msg("[JOB] %s: %s\n", #kind, msg)
#define LOG_RENDER(msg) log_msg("[RENDER] %s\n", msg)
#define LOG_PAINT(msg) log_msg("[PAINT] %s\n", msg)
#define LOG_SCROLL(msg) log_msg("[SCROLL] %s\n", msg)