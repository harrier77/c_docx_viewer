/*
 * viewer_save.c - salvataggio DOCX: document.xml + ricostruzione ZIP con miniz.
 *
 * Modulo del viewer d2dview. Incluso da maind2dv.c (build unity):
 * condivide tipi, macro e stato globale definiti nel file principale.
 */

static void save_docx_file(const char *path)
{
    // fprintf(stderr, "[SAVE_DEBUG] 1. save_docx_file called with path='%s', g_doc=%p\n", path ? path : "NULL", g_doc);
    if (!path || !path[0] || !g_doc)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: Invalid path or NULL g_doc\n");
        MessageBoxA(g_hwnd, "Nessun documento da salvare.", "d2dview", MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* Ottieni il word/document.xml completo e aggiornato dalla DLL.
     * Questo include la dichiarazione XML, il root <w:document> con tutti i
     * namespace, il <w:body> con tutti i paragrafi (inclusi gli eventuali
     * aggiornamenti via ld_paragraph_set_ooxml), e i tag di chiusura. */
    log_msg("[SAVE] Attempting to save document to: %s\n", path);

    char *document_xml = NULL;
    int got = ld_get_document_xml(g_doc, &document_xml);
    // fprintf(stderr, "[SAVE_DEBUG] 2. ld_get_document_xml returned %d, xml=%p\n", got, (void *)document_xml);

    if (!got || !document_xml)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: Failed to get document.xml: %s\n", ld_last_error());
        MessageBoxA(g_hwnd,
                    "Impossibile ottenere il contenuto del documento.",
                    "d2dview - Salva", MB_OK | MB_ICONERROR);
        if (document_xml) ld_free_string(document_xml);
        return;
    }

    size_t doc_xml_len = strlen(document_xml);
    // fprintf(stderr, "[SAVE_DEBUG] 3. document.xml length: %zu bytes\n", doc_xml_len);

    /* === Ricostruzione dell'archivio ZIP su file temporaneo === */
    // fprintf(stderr, "[SAVE_DEBUG] 4. Rebuilding DOCX archive with new document.xml\n");

    char *tmp_path = malloc(strlen(path) + 8);
    if (!tmp_path)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: malloc failed for tmp path\n");
        ld_free_string(document_xml);
        return;
    }
    strcpy(tmp_path, path);
    strcat(tmp_path, ".tmp");

    mz_zip_archive src, dst;
    mz_zip_zero_struct(&src);
    mz_zip_zero_struct(&dst);

    if (!mz_zip_reader_init_file(&src, path, 0))
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_reader_init_file failed\n");
        free(tmp_path);
        ld_free_string(document_xml);
        return;
    }

    /* Trova l'indice di word/document.xml nell'archivio originale */
    int doc_index = mz_zip_reader_locate_file(&src, "word/document.xml", NULL, 0);
    if (doc_index < 0)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: word/document.xml not found in archive\n");
        mz_zip_reader_end(&src);
        remove(tmp_path);
        free(tmp_path);
        ld_free_string(document_xml);
        return;
    }

    /* === Ricostruisci l'archivio su file temporaneo === */
    if (!mz_zip_writer_init_file(&dst, tmp_path, 0))
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_writer_init_file failed\n");
        mz_zip_reader_end(&src);
        remove(tmp_path);
        free(tmp_path);
        ld_free_string(document_xml);
        return;
    }

    int ok = MZ_TRUE;
    mz_uint nfiles = mz_zip_reader_get_num_files(&src);
    for (mz_uint f = 0; f < nfiles; f++)
    {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&src, f, &stat))
        {
            // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_reader_file_stat failed on %u\n", f);
            ok = MZ_FALSE;
            break;
        }

        /* word/document.xml viene sostituito col contenuto completo dalla DLL */
        if (f == doc_index)
        {
            if (!mz_zip_writer_add_mem(&dst, "word/document.xml", document_xml, doc_xml_len, MZ_BEST_COMPRESSION))
            {
                // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_writer_add_mem failed\n");
                ok = MZ_FALSE;
            }
            continue;
        }

        /* Copia tutti gli altri file come-sono (riutilizza i dati gia' compressi) */
        if (!mz_zip_writer_add_from_zip_reader(&dst, &src, f))
        {
            // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_writer_add_from_zip_reader failed on '%s'\n", stat.m_filename);
            ok = MZ_FALSE;
            break;
        }
    }

    if (ok && !mz_zip_writer_finalize_archive(&dst))
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: mz_zip_writer_finalize_archive failed\n");
        ok = MZ_FALSE;
    }

    mz_zip_writer_end(&dst);
    mz_zip_reader_end(&src);
    ld_free_string(document_xml);

    if (!ok)
    {
        remove(tmp_path);
        free(tmp_path);
        MessageBoxA(g_hwnd,
                    "Errore durante il salvataggio del documento.",
                    "d2dview - Salva", MB_OK | MB_ICONERROR);
        return;
    }

    /* Sostituisci l'archivio originale con quello ricostruito */
    if (remove(path) != 0)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: cannot delete old file '%s' (in uso?)\n", path);
        remove(tmp_path);
        free(tmp_path);
        MessageBoxA(g_hwnd,
                    "Impossibile sovrascrivere il file (aperto da un altro programma?).",
                    "d2dview - Salva", MB_OK | MB_ICONERROR);
        return;
    }

    if (rename(tmp_path, path) != 0)
    {
        // fprintf(stderr, "[SAVE_DEBUG] ERROR: rename tmp -> '%s' failed\n", path);
        remove(tmp_path);
        free(tmp_path);
        MessageBoxA(g_hwnd,
                    "Errore durante il salvataggio del documento.",
                    "d2dview - Salva", MB_OK | MB_ICONERROR);
        return;
    }
    free(tmp_path);

    // fprintf(stderr, "[SAVE_DEBUG] 5. DOCX rebuilt successfully with miniz\n");

    /* Forza un aggiornamento dell'interfaccia */
    g_layout_epoch++;
    InvalidateRect(g_hwnd, NULL, FALSE);

    // fprintf(stderr, "[SAVE_DEBUG] 6. Save completed\n");
    MessageBoxA(g_hwnd, "Documento salvato correttamente.", "d2dview - Salva", MB_OK | MB_ICONINFORMATION);
}
