/* Avvia la modalita' inline sul run indicato dall'hit-test del caret.
 * Viene chiamato dopo aver ottenuto un ld_run_caret_hit valido. */
static void inline_begin(const ld_caret_hit* hit)
{
    if (!hit || hit->para_index < 0 || hit->run_index < 0)
        return;

    g_inline_active  = 1;
    g_inline_para    = hit->para_index;
    g_inline_run     = hit->run_index;
    g_inline_offset  = hit->char_offset; /* offset nel paragrafo (spazio normalizzato) */
    g_inline_coalesce = 0;
    free(g_inline_buffer);
    g_inline_buffer = NULL;
    free(g_inline_run_text);
    g_inline_run_text = NULL;

    /* Ottieni il testo del run dal paragrafo usando i boundary dal caret hit. */
    char* para_text = ld_paragraph_get_text(g_doc, hit->para_index);
    if (para_text && hit->run_char_start >= 0 && hit->run_char_end > hit->run_char_start)
    {
        int run_len = hit->run_char_end - hit->run_char_start;
        if (run_len > 0 && hit->run_char_start + run_len <= (int)strlen(para_text))
        {
            g_inline_run_text = (char*)malloc(run_len + 1);
            if (g_inline_run_text)
            {
                memcpy(g_inline_run_text, para_text + hit->run_char_start, run_len);
                g_inline_run_text[run_len] = '\0';
            }
        }
    }
    if (para_text) ld_free_string(para_text);

    fprintf(stderr, "[INLINE] begin para=%d run=%d offset=%d run_text='%s'\n",
            g_inline_para, g_inline_run, g_inline_offset, g_inline_run_text ? g_inline_run_text : "");
}
