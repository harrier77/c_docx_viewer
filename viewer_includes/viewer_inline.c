/*
 * viewer_inline.c - gestione edit inline (prototipo in-memory) con debounce.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 *
 * Il typing viene applicato SUBITO a g_inline_run_text (shadow UI-only,
 * nessun ld_run_* e nessun request_render per carattere). Un timer Win32
 * (LD_INLINE_DEBOUNCE_MS) invia un solo JOB_RUN_APPLY_EDIT con il testo
 * finale del run. Il job e' quindi una sostituzione completa del run, non
 * un'operazione per tasto: insert/backspace/delete consecutivi vengono
 * naturalmente coalesciati dal fatto che il shadow e' gia' aggiornato.
 *
 * Non viene toccata la semantica pubblica delle funzioni ld_run_* ne'
 * l'implementazione del caret/undo: qui cambia solo la macchina degli edit.
 */

#include "litedocx_c.h"
#include <stdarg.h>

#if LD_INLINE_DEBUG_LOG
static void inline_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[INLINE] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#else
static void inline_log(const char *fmt, ...)
{
    (void)fmt;
}
#endif

/* ---------------- UTF-8 / UTF-16 helpers ---------------- */

static int inline_dup_len(const char *s)
{
    return s ? (int)strlen(s) : 0;
}

static char *inline_dup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Offset in byte dell'inizio del codepoint precedente (backspace UTF-8 safe). */
static int utf8_prev_char_offset(const char *s, int offset)
{
    if (!s || offset <= 0)
        return 0;
    int pos = offset - 1;
    while (pos > 0 && (((unsigned char)s[pos] & 0xC0) == 0x80))
        pos--;
    return pos;
}

/* Offset in byte del codepoint successivo (Delete UTF-8 safe). */
static int utf8_next_char_offset(const char *s, int offset, int len)
{
    if (!s || offset < 0 || offset >= len)
        return len;
    int pos = offset + 1;
    while (pos < len && (((unsigned char)s[pos] & 0xC0) == 0x80))
        pos++;
    return pos;
}

static int utf8_count_chars(const char *s)
{
    int n = 0;
    if (!s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
    {
        if ((*p & 0xC0) != 0x80)
            n++;
    }
    return n;
}

/* Codifica un codepoint Unicode in UTF-8 in out[5] (4 byte + NUL). */
static int inline_utf16_to_utf8(unsigned cp, char out[5])
{
    if (cp <= 0x7F)
    {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp <= 0x7FF)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp <= 0xFFFF)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    if (cp <= 0x10FFFF)
    {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
        return 4;
    }
    return inline_utf16_to_utf8(0xFFFD, out); /* cp non valido */
}

#define INLINE_IS_HIGH_SURROGATE(w) (((w) >= 0xD800) && ((w) <= 0xDBFF))
#define INLINE_IS_LOW_SURROGATE(w)  (((w) >= 0xDC00) && ((w) <= 0xDFFF))

/* ---------------- snapshot / timer / stato pending ---------------- */

static void inline_snapshot_clear(InlineSnapshot *s)
{
    if (!s)
        return;
    free(s->text);
    ZeroMemory(s, sizeof(*s));
}

/* Variante con ownership transfer del testo: permette di completare tutte
 * le allocazioni del job PRIMA di incrementare token/stato busy. */
static void inline_snapshot_set_owned(InlineSnapshot *s, char *owned_text,
                                      int para, int run, int run_off, int para_off,
                                      int caret_off, int caret_rs, int caret_re,
                                      int caret_x, unsigned epoch)
{
    if (!s)
    {
        free(owned_text);
        return;
    }
    free(s->text);
    s->text       = owned_text;
    s->para       = para;
    s->run        = run;
    s->run_offset = run_off;
    s->para_offset = para_off;
    s->caret_offset = caret_off;
    s->caret_run_start = caret_rs;
    s->caret_run_end = caret_re;
    s->caret_x    = caret_x;
    s->epoch      = epoch;
}

static void inline_snapshot_set(InlineSnapshot *s, const char *text,
                                int para, int run, int run_off, int para_off,
                                int caret_off, int caret_rs, int caret_re,
                                int caret_x, unsigned epoch)
{
    inline_snapshot_set_owned(s, inline_dup(text ? text : ""),
                              para, run, run_off, para_off,
                              caret_off, caret_rs, caret_re,
                              caret_x, epoch);
}

/* Azzera solo i contatori del batch corrente: i totali di sessione restano. */
static void inline_clear_batch(void)
{
    free(g_inline_buffer);
    g_inline_buffer = NULL;
    free(g_inline_last_char);
    g_inline_last_char = NULL;
    g_inline_pending_backspaces = 0;
    g_inline_pending_op = -1;
    g_inline_coalesce = 0;
}

static int inline_has_pending(void)
{
    if (g_inline_coalesce ||
        (g_inline_buffer && g_inline_buffer[0]) ||
        g_inline_pending_backspaces > 0 ||
        g_inline_pending_op >= 0)
        return 1;
    /* Condizione finale di sicurezza: il shadow e' diverso dall'ultima
     * baseline confermata (utili se i contatori sono stati azzerati dal
     * flush while il job e' ancora in volo). */
    return (g_inline_run_text && g_inline_applied.text &&
            strcmp(g_inline_run_text, g_inline_applied.text) != 0);
}

static void inline_cancel_timer(void)
{
    if (!g_inline_timer)
        return;
    KillTimer(g_hwnd, g_inline_timer);
    g_inline_timer = 0;
    g_inline_timer_retry = 0;
    inline_log("timer cancelled");
}

/* (Ri)arma l'unico timer della sessione. Ogni modifica effettiva del testo
 * richiama questa funzione con retry=0: il timer precedente viene ucciso e
 * ne viene armato uno nuovo con il pieno ritardo di debounce. Il flush
 * scatta quindi solo dopo LD_INLINE_DEBOUNCE_MS dall'ULTIMO input
 * (trailing-edge), non a intervalli dal primo tasto.
 * retry=1 viene usato quando lo scheduler e' occupato: ritardo breve
 * (LD_INLINE_RETRY_MS), senza toccare testo ne' offset.
 * Ritorna 1 se il timer e' armato, 0 in caso di fallimento. */
static int inline_schedule_timer(int retry)
{
    if (!g_inline_active || !g_hwnd || !g_doc)
        return 0;

    /* Trailing-edge: azzera sempre il timer prima di riarmarlo. */
    inline_cancel_timer();

    unsigned delay = retry ? (unsigned)LD_INLINE_RETRY_MS
                           : (unsigned)LD_INLINE_DEBOUNCE_MS;
    g_inline_timer = SetTimer(g_hwnd, LD_INLINE_TIMER_ID, delay, NULL);
    if (g_inline_timer)
    {
        g_inline_timer_retry = retry;
        inline_log("timer %s id=%u delay=%ums",
                   retry ? "RETRY armed" : "restarted",
                   (unsigned)g_inline_timer, delay);
        return 1;
    }

    /* Nessun timer: il testo resta integralmente nei buffer shadow. */
    g_inline_timer_retry = 0;
    inline_log("SetTimer FAILED (%lu) - pending text kept",
               (unsigned long)GetLastError());
    return 0;
}

/* Recovery quando neppure il timer puo' essere armato. Non si azzera nulla:
 * se siamo IDLE si richiede un flush immediato (gestito dal chiamante),
 * altrimenti si segna il batch come deferred da riprendere a IDLE. Niente
 * PostMessage immediati ne' retry periodici non limitati. */
static void inline_handle_schedule_failure(void)
{
    if (!g_inline_active || !inline_has_pending())
        return;
    if (g_doc_state == DOC_STATE_IDLE && !g_render_busy && !g_load_active &&
        g_pending_apply_token == 0)
    {
        g_inline_flush_retry_now = 1;
        inline_log("schedule failed while IDLE: immediate flush requested");
    }
    else
    {
        g_inline_flush_deferred = 1;
        inline_log("schedule failed while busy: flush deferred until IDLE");
    }
}

/* Tentativo singolo di flush. Ritorna 1 solo se un job e' stato davvero
 * accodato (o eseguito in fallback sincrono). from_timer: 1=WM_TIMER,
 * 2=flush differito/recovery, 0=chiamata esplicita. */
static int inline_flush_attempt(int from_timer)
{
    if (!g_inline_active || !g_doc || !g_inline_run_text)
        return 0;

    /* Il timer e' sempre consumato, anche se non c'e' nulla da spedire:
     * SetTimer con hWnd e' periodico, non one-shot. */
    inline_cancel_timer();

    if (!inline_has_pending())
    {
        if (from_timer == 1)
            inline_log("timer fired with no accumulated chars: no job");
        return 0;
    }

    if (g_load_active || g_render_busy || g_doc_state != DOC_STATE_IDLE ||
        g_pending_apply_token != 0)
    {
        /* Un solo edit in volo al massimo. Il batch resta intatto:
         * riproviamo tra poco con un retry breve, senza inviare un altro
         * job e senza toccare testo/offset. */
        inline_log("retry reason: state=%d busy=%d load=%d token=%u",
                   g_doc_state, g_render_busy, g_load_active,
                   g_pending_apply_token);
        if (!inline_schedule_timer(1))
            g_inline_flush_deferred = 1; /* occupato: niente flush immediato */
        return 0;
    }

    char *final_text = inline_dup(g_inline_run_text);
    char *snap_text = NULL;
    char *marked = NULL;
    Job *j = NULL;

    if (!final_text)
        goto flush_fail;

    /* Marker 0xFF 0xFF: il worker riconosce il testo completo del run. */
    size_t text_len = strlen(final_text);
    marked = (char *)malloc(text_len + 3);
    if (!marked)
        goto flush_fail;
    marked[0] = (char)0xFF;
    marked[1] = (char)0xFF;
    memcpy(marked + 2, final_text, text_len + 1);

    if (g_worker_started)
    {
        j = alloc_job(JOB_RUN_APPLY_EDIT);
        if (!j)
            goto flush_fail;
    }

    /* Anche la copia dello snapshot deve riuscire PRIMA di toccare
     * token/stato: in caso di errore il batch resta intatto e ritentabile. */
    snap_text = inline_dup(final_text);
    if (!snap_text)
        goto flush_fail;

    /* Tutte le allocazioni sono riuscite: da qui in poi non si torna indietro. */
    unsigned token = ++g_apply_token;
    g_pending_apply_token = token;
    g_pending_apply_epoch = g_layout_epoch;

    /* Snapshot di cio' che il worker sta per ricevere: serve a riconoscere
     * il risultato e a capire se l'utente ha digitato ancora nel frattempo. */
    inline_snapshot_clear(&g_inline_job);
    inline_snapshot_set_owned(&g_inline_job, snap_text,
                              g_inline_para, g_inline_run,
                              g_inline_run_offset, g_inline_offset,
                              g_caret_offset, g_caret_run_start, g_caret_run_end,
                              g_caret_x, g_pending_apply_epoch);
    snap_text = NULL; /* ownership trasferita a g_inline_job */
    g_inline_jobs_total++;
    g_inline_commit_in_flight = 1;
    g_inline_commit_closed = 0;

    inline_log("JOB SENT: chars_total=%u chars_batch=%d dels=%d "
               "para=%d run=%d bytes=%d gen=%u epoch=%u token=%u "
               "(jobs=%u / chars=%u)",
               g_inline_chars_total,
               utf8_count_chars(g_inline_buffer),
               g_inline_pending_backspaces,
               g_inline_para, g_inline_run, (int)text_len,
               g_load_gen, g_pending_apply_epoch, token,
               g_inline_jobs_total, g_inline_chars_total);

    g_render_busy = 1;
    g_render_doc = g_doc;
    g_doc_state = DOC_STATE_APPLYING;

    if (!g_worker_started)
    {
        /* Fallback sincrono: stesso percorso di JOB_RUN_APPLY_EDIT. */
        EditResult r;
        ZeroMemory(&r, sizeof(r));
        r.doc   = g_doc;
        r.gen   = g_load_gen;
        r.epoch = g_pending_apply_epoch;
        r.token = token;
        r.ok = ld_run_set_text(g_doc, g_inline_para, g_inline_run, final_text);
        if (!r.ok)
        {
            const char *err = ld_last_error();
            lstrcpynA(r.error, err ? err : "", sizeof(r.error));
        }
        free(marked);
        free(final_text);
        on_edit_applied(&r); /* passa da inline_handle_result */
    }
    else
    {
        j->doc = g_doc;
        j->gen = g_load_gen;
        j->epoch = g_pending_apply_epoch;
        j->token = token;
        j->para_index = g_inline_para;
        j->run_index = g_inline_run;
        j->text = marked;
        enqueue_job(j);
        free(final_text);
    }

    inline_clear_batch();
    g_inline_flush_deferred = 0;
    g_inline_flush_retry_now = 0;
    return 1;

flush_fail:
    /* Ogni buffer viene liberato una sola volta; token/stato/busy intatti. */
    free(j);
    free(marked);
    free(snap_text);
    free(final_text);
    inline_log("alloc failure: shadow kept, no token/busy change");
    if (!inline_schedule_timer(1))
        inline_handle_schedule_failure();
    return 0;
}

/* Wrapper con guardia anti-ricorsione: se il recovery richiede un flush
 * immediato mentre siamo gia' dentro un tentativo, esegue al massimo un
 * secondo giro. Niente loop infiniti ne' PostMessage immediati. */
static int inline_flush_pending(int from_timer)
{
    int rc = 0;
    int tries = 0;
    do
    {
        g_inline_flush_retry_now = 0;
        rc = inline_flush_attempt(from_timer);
    } while (g_inline_flush_retry_now && ++tries < 2);
    return rc;
}

/* Richiamato solo alle transizioni a IDLE gia' esistenti (completamento
 * edit/render, gestione errore, resume): consuma un flush rimasto deferred
 * senza riarmare il debounce completo, perche' non c'e' stato un nuovo input. */
static int inline_resume_deferred(void)
{
    if (!g_inline_active)
        return 0;
    if (!g_inline_flush_deferred && !g_inline_flush_retry_now)
        return 0;
    if (!inline_has_pending())
    {
        g_inline_flush_deferred = 0;
        g_inline_flush_retry_now = 0;
        return 0;
    }
    if (g_load_active || g_render_busy || g_doc_state != DOC_STATE_IDLE ||
        g_pending_apply_token != 0)
        return 0; /* riprova alla prossima transizione */

    g_inline_flush_deferred = 0;
    g_inline_flush_retry_now = 0;
    inline_log("deferred flush resumed (no new input)");
    return inline_flush_pending(2);
}

/* ---------------- sessione inline ---------------- */

/* Avvia la modalita' inline sul run indicato dall'hit-test del caret. */
static void inline_begin(const ld_caret_hit* hit)
{
    if (!hit || hit->para_index < 0 || hit->run_index < 0)
        return;

    /* Un cambio di run/paragrafo non e' coalescibile: chiude la sessione
     * precedente dopo aver spedito (se possibile) il suo batch. */
    if (g_inline_active)
    {
        int run_changed = (g_inline_para != hit->para_index ||
                           g_inline_run != hit->run_index);
        inline_log("begin on %d/%d (active=%d run_changed=%d pending=%d)",
                   hit->para_index, hit->run_index, g_inline_active,
                   run_changed, inline_has_pending());
        if (run_changed && inline_has_pending() &&
            !g_pending_apply_token && !g_render_busy && !g_load_active &&
            g_doc_state == DOC_STATE_IDLE)
        {
            inline_flush_pending(0);
        }
        inline_end(run_changed ? "run changed" : "restart");
    }

    g_inline_active     = 1;
    g_inline_para       = hit->para_index;
    g_inline_run        = hit->run_index;
    g_inline_offset     = hit->char_offset;       /* offset nel paragrafo */
    g_inline_run_offset = hit->run_char_offset;   /* offset dentro il run */
    if (g_inline_run_offset < 0)
        g_inline_run_offset = 0; /* hit-test puo' restituire -1 */

    inline_cancel_timer();
    inline_clear_batch();
    inline_snapshot_clear(&g_inline_applied);
    /* Un commit della sessione precedente puo' essere ancora in volo: il suo
     * snapshot serve a riconoscerne il risultato tardivo e non va cancellato. */
    if (!g_inline_commit_in_flight)
        inline_snapshot_clear(&g_inline_job);
    free(g_inline_run_text);
    g_inline_run_text = NULL;
    free(g_inline_last_char);
    g_inline_last_char = NULL;
    g_inline_char_w = 0;
    g_inline_high_surrogate = 0;
    g_inline_chars_total = 0;
    g_inline_jobs_total = 0;
    g_inline_flush_deferred = 0;  /* nuovo input: riparte il trailing-edge */
    g_inline_flush_retry_now = 0;

    /* Testo del run dal paragrafo, usando i boundary del caret hit. */
    char *para_text = ld_paragraph_get_text(g_doc, hit->para_index);
    if (para_text && hit->run_char_start >= 0 &&
        hit->run_char_end >= hit->run_char_start)
    {
        int run_len = hit->run_char_end - hit->run_char_start;
        if (run_len > 0 &&
            hit->run_char_start + run_len <= (int)strlen(para_text))
        {
            g_inline_run_text = (char *)malloc((size_t)run_len + 1);
            if (g_inline_run_text)
            {
                memcpy(g_inline_run_text, para_text + hit->run_char_start,
                       (size_t)run_len);
                g_inline_run_text[run_len] = '\0';
            }
        }
        else if (run_len == 0)
        {
            /* Run vuoto: buffer vuoto, inline_apply_edit fa strlen(). */
            g_inline_run_text = inline_dup("");
            g_inline_run_offset = 0;
        }
    }
    if (para_text)
        ld_free_string(para_text);
    if (!g_inline_run_text)
        g_inline_run_text = inline_dup("");

    /* Baseline: cio' che il documento contiene davvero a inizio sessione. */
    inline_snapshot_set(&g_inline_applied, g_inline_run_text,
                        g_inline_para, g_inline_run,
                        g_inline_run_offset, g_inline_offset,
                        g_caret_offset, g_caret_run_start, g_caret_run_end,
                        g_caret_x, g_layout_epoch);

    inline_log("begin done para=%d run=%d run_off=%d run_text='%s'",
               g_inline_para, g_inline_run, g_inline_run_offset,
               g_inline_run_text ? g_inline_run_text : "");
}

/* Termina la modalita' inline. I caratteri non ancora confermati vengono
 * annullati (comportamento storico del buffer su Escape/click altrove).
 * Se un job e' gia' in volo non puo' essere interrotto: il suo snapshot e
 * i flag di commit vengono conservati per consumarne il risultato tardivo
 * senza riattivare sessione/caret, e senza liberare busy/token qui. */
static void inline_end(const char *why)
{
    inline_cancel_timer();
    if (!g_inline_active)
        return;

    int had_job = g_inline_commit_in_flight && g_pending_apply_token != 0 &&
                  g_inline_job.text != NULL;
    int had_pending = inline_has_pending();

    free(g_inline_run_text);
    if (had_job)
    {
        /* Sessione chiusa: nessun shadow locale da mostrare. Il documento
         * verra' aggiornato dal job in volo. */
        g_inline_run_text = NULL;
        g_inline_commit_closed = 1;
        /* g_inline_job resta intatto fino a inline_handle_result. */
    }
    else
    {
        g_inline_run_text = inline_dup(g_inline_applied.text
                                       ? g_inline_applied.text : "");
        inline_snapshot_clear(&g_inline_job);
    }
    inline_snapshot_clear(&g_inline_applied);
    inline_clear_batch();
    g_inline_high_surrogate = 0;

    g_inline_active     = 0;
    g_inline_para       = -1;
    g_inline_run        = -1;
    g_inline_offset     = -1;
    g_inline_run_offset = -1;
    g_inline_coalesce   = 0;
    g_inline_pending_backspaces = 0;
    g_inline_pending_op = -1;
    g_inline_char_w     = 0;
    g_inline_flush_deferred = 0;  /* modifiche locali annullate */
    g_inline_flush_retry_now = 0;

    inline_log("end (%s): pending=%d job_in_flight=%d chars=%u jobs=%u",
               why ? why : "?", had_pending, had_job,
               g_inline_chars_total, g_inline_jobs_total);
}

/* ---------------- edit locale + scheduling ---------------- */

static int inline_caret_step(void)
{
    return g_inline_char_w > 0 ? g_inline_char_w : 8;
}

/* Applica un singolo edit SOLO al shadow locale e (ri)apre il timer.
 * Nessun JOB_RUN_APPLY_EDIT e nessun request_render per carattere. */
static void inline_apply_edit(int op, const char* text)
{
    if (!g_inline_active || !g_doc || g_inline_para < 0 || g_inline_run < 0)
        return;

    if (!g_inline_run_text)
        g_inline_run_text = inline_dup("");
    if (!g_inline_run_text)
        return;

    int len = (int)strlen(g_inline_run_text);
    int step = inline_caret_step();

    if (op == LD_INLINE_OP_INSERT)
    {
        if (!text || !*text)
            return;

        int add_len = inline_dup_len(text);
        int ins_pos = (g_inline_run_offset >= 0 && g_inline_run_offset <= len)
                        ? g_inline_run_offset : len;
        char *new_text = (char *)malloc((size_t)len + (size_t)add_len + 1);
        if (!new_text)
            return;
        memcpy(new_text, g_inline_run_text, (size_t)ins_pos);
        memcpy(new_text + ins_pos, text, (size_t)add_len);
        memcpy(new_text + ins_pos + add_len,
               g_inline_run_text + ins_pos, (size_t)(len - ins_pos));
        new_text[len + add_len] = '\0';

        free(g_inline_run_text);
        g_inline_run_text = new_text;
        g_inline_run_offset = ins_pos + add_len;
        g_inline_offset += add_len;
        g_caret_offset += add_len;
        g_caret_run_end += add_len;
        /* Stima visiva: avanza di un passo-font per codepoint UTF-8. */
        g_caret_x += step * utf8_count_chars(text);

        size_t old_len = g_inline_buffer ? strlen(g_inline_buffer) : 0;
        char *new_buf = (char *)realloc(g_inline_buffer,
                                        old_len + (size_t)add_len + 1);
        if (new_buf)
        {
            g_inline_buffer = new_buf;
            memcpy(g_inline_buffer + old_len, text, (size_t)add_len + 1);
        }
        free(g_inline_last_char);
        g_inline_last_char = inline_dup(text);
        g_inline_chars_total += (unsigned)utf8_count_chars(text);

        inline_log("INSERT '%s' bytes=%d at run_off=%d -> len=%d x=%d",
                   text, add_len, ins_pos, len + add_len, g_caret_x);
    }
    else if (op == LD_INLINE_OP_BACKSPACE || op == LD_INLINE_OP_DELETE)
    {
        int del_pos, del_len;
        if (op == LD_INLINE_OP_BACKSPACE)
        {
            if (g_inline_run_offset <= 0 || len <= 0)
            {
                inline_log("BACKSPACE stop (empty run / start of run)");
                return; /* run vuoto o inizio run: stop, nessun job */
            }
            del_pos = utf8_prev_char_offset(g_inline_run_text,
                                            g_inline_run_offset);
            del_len = g_inline_run_offset - del_pos;
        }
        else
        {
            if (g_inline_run_offset >= len || len <= 0)
            {
                inline_log("DELETE stop (end of run)");
                return;
            }
            del_pos = g_inline_run_offset;
            del_len = utf8_next_char_offset(g_inline_run_text, del_pos, len)
                      - del_pos;
        }
        if (del_len <= 0 || del_pos < 0 || del_pos + del_len > len)
            return;

        /* Cancellazione sempre a confine di codepoint: del_len e' un
         *'intera sequenza UTF-8, quindi non si taglia mai a meta'. */
        memmove(g_inline_run_text + del_pos,
                g_inline_run_text + del_pos + del_len,
                (size_t)(len - (del_pos + del_len) + 1));
        if (op == LD_INLINE_OP_BACKSPACE)
            g_inline_run_offset = del_pos;

        g_inline_offset -= del_len;
        if (g_inline_offset < 0) g_inline_offset = 0;
        g_caret_offset -= del_len;
        if (g_caret_offset < 0) g_caret_offset = 0;
        g_caret_run_end -= del_len;
        if (g_caret_run_end < g_caret_run_start)
            g_caret_run_end = g_caret_run_start;

        if (op == LD_INLINE_OP_BACKSPACE)
        {
            g_caret_x -= step; /* stima: un codepoint rimosso */
            if (g_caret_x < 0) g_caret_x = 0;
        }

        g_inline_pending_backspaces++; /* conta i codepoint cancellati */
        inline_log("%s bytes=%d at run_off=%d -> len=%d x=%d",
                   op == LD_INLINE_OP_BACKSPACE ? "BACKSPACE" : "DELETE",
                   del_len, del_pos, len - del_len, g_caret_x);
    }
    else
    {
        return;
    }

    g_inline_pending_op = op;
    g_inline_coalesce = 1;

    /* Un nuovo input sostituisce eventuali retry deferred con un nuovo
     * trailing-edge: il testo shadow e' gia' aggiornato qui sopra. */
    g_inline_flush_deferred = 0;

    /* Nessun refresh del layout: solo il caret viene ridisegnato nella
     * posizione stimata. Il re-layout avverra' dopo il job coalescito.
     * Ogni tasto riavvia il timer: e' un debounce trailing-edge. */
    InvalidateRect(g_hwnd, NULL, FALSE);
    if (!inline_schedule_timer(0))
        inline_handle_schedule_failure();
    if (g_inline_flush_retry_now)
    {
        g_inline_flush_retry_now = 0;
        inline_log("SetTimer failed while IDLE: immediate flush");
        inline_flush_pending(2);
    }
}

/* ---------------- risultato del worker ---------------- */

/* Result con token corrente ma epoch/run diversi: non toccare il testo,
 * ma rilascia comunque lo stato di APPLYING per non bloccare la UI. */
static void inline_release_orphan_result(const EditResult *r)
{
    inline_log("ORPHAN result token=%u epoch=%u/%u run=%d/%d vs %d/%d",
               r->token, r->epoch, g_inline_job.epoch,
               g_inline_job.para, g_inline_job.run,
               g_inline_para, g_inline_run);
    g_pending_apply_token = 0;
    g_pending_apply_epoch = 0;
    g_render_busy = 0;
    g_render_doc = NULL;
    g_doc_state = DOC_STATE_IDLE;
    /* Il commit (anche di una sessione chiusa) e' stato consumato. */
    g_inline_commit_in_flight = 0;
    g_inline_commit_closed = 0;
    g_inline_flush_deferred = 0;
    g_inline_flush_retry_now = 0;
    inline_snapshot_clear(&g_inline_job);
    g_layout_complete = 0;
    g_first_rendered_page = -1;
    g_last_rendered_page = -1;
    g_render_pending_width = -1;
    g_render_pending_first_page = -1;
    g_render_pending_last_page = -1;
    g_layout_epoch++;
    request_render(layout_content_width());
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* Riporta il shadow all'ultima baseline confermata (edit fallito). */
static void inline_restore_applied(void)
{
    inline_cancel_timer();
    free(g_inline_run_text);
    g_inline_run_text = inline_dup(g_inline_applied.text
                                   ? g_inline_applied.text : "");
    g_inline_para       = g_inline_applied.para;
    g_inline_run        = g_inline_applied.run;
    g_inline_run_offset = g_inline_applied.run_offset;
    g_inline_offset     = g_inline_applied.para_offset;
    g_caret_offset      = g_inline_applied.caret_offset;
    g_caret_run_start   = g_inline_applied.caret_run_start;
    g_caret_run_end     = g_inline_applied.caret_run_end;
    g_caret_x           = g_inline_applied.caret_x;
    inline_snapshot_clear(&g_inline_job);
    inline_clear_batch();
}

/* Gestisce il risultato di un edit inline (chiamato da on_edit_applied).
 * Viene accettato anche se la sessione e' stata chiusa: in quel caso il
 * risultato viene solo consumato (busy/token rilasciati) e il documento
 * riceve il render necessario, senza toccare sessione/caret/shadow. */
static void inline_handle_result(EditResult* r)
{
    if (!r || !g_pending_apply_token)
        return;
    if (!g_inline_active && !g_inline_commit_in_flight)
        return;

    /* Stale: documento, generazione o token diversi -> si continua ad
     * attendere il risultato del job corrente. */
    if (r->doc != g_doc || r->gen != g_load_gen ||
        r->token != g_pending_apply_token)
        return;

    int detached = g_inline_commit_closed;
    int active = g_inline_active && !detached;

    /* Verifica esplicita di epoch e di run/paragrafo: un risultato che
     * appartiene a un layout o a un run diverso non puo' aggiornare il
     * shadow corrente. Per un commit "staccato" dalla sessione il run e'
     * quello dello snapshot del job, non quello della sessione nuova. */
    if (r->epoch != g_pending_apply_epoch ||
        r->epoch != g_inline_job.epoch ||
        !g_inline_job.text ||
        (active && (g_inline_job.para != g_inline_para ||
                    g_inline_job.run != g_inline_run)))
    {
        inline_release_orphan_result(r);
        return;
    }

    /* Rilascio dello stato del job ESATTAMENTE una volta. */
    g_inline_commit_in_flight = 0;
    g_inline_commit_closed = 0;
    g_pending_apply_token = 0;
    g_pending_apply_epoch = 0;
    g_render_busy = 0;
    g_render_doc = NULL;
    g_doc_state = DOC_STATE_IDLE;

    if (!active)
    {
        /* Sessione chiusa (Escape/click/cambio run/documento) o nuova
         * sessione apertasi prima di questo risultato: non ripristinare
         * testo, non riancorare, non liberare il token della sessione nuova. */
        inline_snapshot_clear(&g_inline_job);
        if (r->ok)
        {
            inline_log("late result token=%u (session closed): document updated",
                       r->token);
            g_layout_complete = 0;
            g_first_rendered_page = -1;
            g_last_rendered_page = -1;
            g_render_pending_width = -1;
            g_render_pending_first_page = -1;
            g_render_pending_last_page = -1;
            g_layout_epoch++;
            /* Se nel frattempo e' stata aperta una nuova sessione, il suo
             * caret va lasciato intatto: siamo tenuti a non toccare solo
             * quello della sessione chiusa. */
            if (!g_inline_active)
                viewer_caret_clear();
            request_render(layout_content_width());
            InvalidateRect(g_hwnd, NULL, TRUE);
        }
        else
        {
            inline_log("late result token=%u (session closed): edit failed '%s'",
                       r->token, r->error[0] ? r->error : "");
            MessageBoxA(g_hwnd, r->error[0] ? r->error : "Edit inline fallito",
                        "d2dview", MB_OK | MB_ICONERROR);
            resume_pending_render();
        }
        return;
    }

    if (r->ok)
    {
        /* Conferma: la baseline diventa cio' che il worker ha applicato. */
        InlineSnapshot confirmed = g_inline_job;
        ZeroMemory(&g_inline_job, sizeof(g_inline_job)); /* ownership transfer */
        free(g_inline_applied.text);
        g_inline_applied = confirmed;

        /* L'utente puo' aver digitato mentre il job era in volo: il shadow
         * locale e' piu' avanti. In quel caso NON viene riscritto: resta
         * pending e verra' spedito dal timer con un secondo job. */
        int has_newer = (g_inline_run_text &&
                         strcmp(g_inline_run_text, g_inline_applied.text) != 0);
        if (has_newer)
        {
            inline_log("edit ok token=%u: newer local text, re-arm debounce",
                       r->token);
            /* Job in volo concluso: il testo piu' recente viene inviato
             * soltanto allo scadere del trailing-edge, non in parallelo. */
            if (!inline_schedule_timer(0))
                inline_handle_schedule_failure();
        }
        else
        {
            inline_log("edit ok token=%u: batch confirmed, clear",
                       r->token);
            inline_clear_batch();
        }

        /* Reset paginazione e UN solo re-layout (on_render_done fa il
         * re-anchor del caret una sola volta). */
        g_layout_complete = 0;
        g_first_rendered_page = -1;
        g_last_rendered_page = -1;
        g_render_pending_width = -1;
        g_render_pending_first_page = -1;
        g_render_pending_last_page = -1;
        g_layout_epoch++;

        /* Un batch rimasto deferred per occupazione viene inviato ora, senza
         * riarmare il debounce pieno (non c'e' stato un nuovo input). In tal
         * caso il render di questo risultato e' superfluo: lo fara' il job
         * successivo. */
        if (inline_resume_deferred())
        {
            inline_log("deferred flush sent a new job: skip render for token=%u",
                       r->token);
            return;
        }

        /* Salva lo stato logico del caret (g_caret_x resta la stima
         * calcolata durante il typing; il re-anchor la sostituisce
         * con la posizione esatta calcolata dalla DLL). */
        int sp = g_caret_para, sr = g_caret_run, so = g_caret_offset;
        int rs = g_caret_run_start, re = g_caret_run_end;
        int sy = g_caret_y, sw = g_caret_w, sh = g_caret_h;
        viewer_caret_clear();
        g_caret_valid = 1;
        g_caret_para = sp;
        g_caret_run = sr;
        g_caret_offset = so;
        g_caret_run_start = rs;
        g_caret_run_end = re;
        g_caret_y = sy;
        g_caret_w = sw > 0 ? sw : 1;
        g_caret_h = sh;

        request_render(layout_content_width());
        InvalidateRect(g_hwnd, NULL, TRUE);
        inline_log("EDIT COMPLETE token=%u -> request_render (jobs=%u chars=%u)",
                   r->token, g_inline_jobs_total, g_inline_chars_total);
    }
    else
    {
        inline_log("EDIT FAILED token=%u: '%s' -> restore baseline",
                   r->token, r->error[0] ? r->error : "");
        inline_restore_applied();
        g_doc_state = DOC_STATE_IDLE;
        MessageBoxA(g_hwnd, r->error[0] ? r->error : "Edit inline fallito",
                    "d2dview", MB_OK | MB_ICONERROR);
        resume_pending_render();
    }
}

/* Callback WM_TIMER: unica autorita' che decide il flush del batch. */
static void inline_on_timer(void)
{
    if (!g_inline_active)
    {
        inline_cancel_timer();
        return;
    }
    inline_log("timer expired (mode=%s)",
               g_inline_timer_retry ? "retry" : "debounce");
    g_inline_timer_retry = 0;
    inline_flush_pending(1);
}

/* ---------------- input ---------------- */

static void inline_emit_codepoint(unsigned cp)
{
    char utf8[5] = {0};
    int len = inline_utf16_to_utf8(cp, utf8);
    if (len > 0)
        inline_apply_edit(LD_INLINE_OP_INSERT, utf8);
}

/* Gestione tasti per edit inline: Backspace, Delete, Escape.
 * Ritorna 1 se il tasto e' stato gestito (edit inline attivo), 0 altrimenti. */
static int inline_handle_key(WPARAM wp, LPARAM lp)
{
    (void)lp;
    if (!g_inline_active || !g_doc)
        return 0;

    if (wp == VK_BACK)
    {
        inline_apply_edit(LD_INLINE_OP_BACKSPACE, NULL);
        return 1;
    }
    if (wp == VK_DELETE)
    {
        inline_apply_edit(LD_INLINE_OP_DELETE, NULL);
        return 1;
    }
    if (wp == VK_ESCAPE)
    {
        /* Annulla i caratteri non confermati; se un job e' partito il suo
         * effetto resta (stesso comportamento del vecchio buffer). */
        inline_end("escape");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return 1;
    }
    return 0;
}

/* Gestione WM_CHAR: UTF-16 -> UTF-8, inclusa la coppia surrogata per
 * gli emoji fuori dal BMP. */
static int inline_handle_char(WPARAM wp)
{
    if (!g_inline_active || !g_doc)
        return 0;
    if (wp < 32)
        return 0;

    WCHAR wc = (WCHAR)wp;

    if (g_inline_high_surrogate)
    {
        if (INLINE_IS_LOW_SURROGATE(wc))
        {
            unsigned cp = 0x10000u +
                          (((unsigned)g_inline_high_surrogate - 0xD800u) << 10) +
                          ((unsigned)wc - 0xDC00u);
            g_inline_high_surrogate = 0;
            inline_emit_codepoint(cp); /* emoji: 4 byte UTF-8 */
            return 1;
        }
        /* Surrogato alto non seguito da uno basso: emissione di U+FFFD. */
        inline_emit_codepoint(0xFFFD);
        g_inline_high_surrogate = 0;
    }

    if (INLINE_IS_HIGH_SURROGATE(wc))
    {
        g_inline_high_surrogate = wc;
        return 1; /* attende il low surrogate del prossimo WM_CHAR */
    }
    if (INLINE_IS_LOW_SURROGATE(wc))
    {
        inline_emit_codepoint(0xFFFD); /* low surrogate isolato */
        return 1;
    }

    inline_emit_codepoint((unsigned)wc); /* accento, CJK, BMP */
    return 1;
}

/* Riancora il caret dopo un edit completato (layout finito).
 * Chiede alla DLL la posizione esatta dell'offset logico corrente
 * (ld_run_caret_pos): nessun hit-test per coordinate. */
static void inline_reanchor_caret(void)
{
    if (!g_inline_active || !g_doc || !g_caret_valid)
        return;

    if (!g_worker_started || g_doc_state != DOC_STATE_IDLE)
    {
        inline_log("reanchor skip");
        return;
    }

    Job* j = alloc_job(JOB_CARET_POS);
    if (!j)
        return;
    j->doc = g_doc;
    j->gen = g_load_gen;
    j->epoch = g_layout_epoch;
    j->para_index = g_inline_para;
    j->run_index = g_inline_run;
    j->run_char_offset = g_inline_run_offset;
    g_doc_state = DOC_STATE_HITTEST;
    enqueue_job(j);
    inline_log("reanchor: para=%d run=%d off=%d",
               g_inline_para, g_inline_run, g_inline_run_offset);
}
