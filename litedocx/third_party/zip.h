#ifndef ZIP_H
#define ZIP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zip_arc {
    const unsigned char *data;
    size_t size;
} zip_arc;

/* Point the archive at a memory image of the .docx (no copy is made). */
int zip_open(zip_arc *z, const void *data, size_t size);

/*
 * Extract one entry by name. On success *out is malloc'ed, NUL terminated and
 * *out_len is the uncompressed size. Returns 1 on success, 0 on failure.
 * Only STORE (0) and DEFLATE (8) are supported; entries are limited to 64 MB.
 */
int zip_read(zip_arc *z, const char *name, char **out, size_t *out_len);

#endif /* ZIP_H */

#ifdef __cplusplus
}
#endif
