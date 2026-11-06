#include "zip.h"
#include "puff.h"

#include <stdlib.h>
#include <string.h>

#define ZIP_MAX_ENTRY (64u * 1024u * 1024u)
#define SIG_EOCD      0x06054b50u
#define SIG_CENTRAL   0x02014b50u
#define SIG_LOCAL     0x04034b50u

static unsigned le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

int zip_open(zip_arc *z, const void *data, size_t size)
{
    if (!z || !data || size < 22)
        return 0;
    z->data = (const unsigned char *)data;
    z->size = size;
    return 1;
}

static int find_eocd(const zip_arc *z, size_t *out)
{
    size_t start, i;

    if (z->size < 22)
        return 0;
    start = z->size > 65557 ? z->size - 65557 : 0;
    for (i = z->size - 22 + 1; i-- > start; ) {
        if (le32(z->data + i) == SIG_EOCD) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

int zip_read(zip_arc *z, const char *name, char **out, size_t *out_len)
{
    size_t eocd, cdoff, pos, want;
    unsigned long entries, i;
    const unsigned char *d;

    *out = NULL;
    *out_len = 0;

    if (!z || !z->data || !find_eocd(z, &eocd))
        return 0;

    d = z->data;
    entries = le16(d + eocd + 10);
    cdoff = (size_t)le32(d + eocd + 16);
    if (cdoff >= z->size || entries == 0 || entries == 0xFFFF)
        return 0;

    want = strlen(name);
    pos = cdoff;

    for (i = 0; i < entries; i++) {
        unsigned method, nlen, elen, clen;
        unsigned long csize, usize, lho;
        const unsigned char *namep;

        if (pos + 46 > z->size || le32(d + pos) != SIG_CENTRAL)
            return 0;

        method = le16(d + pos + 10);
        csize  = le32(d + pos + 20);
        usize  = le32(d + pos + 24);
        nlen   = le16(d + pos + 28);
        elen   = le16(d + pos + 30);
        clen   = le16(d + pos + 32);
        lho    = le32(d + pos + 42);
        namep  = d + pos + 46;

        if (pos + 46 + nlen > z->size)
            return 0;

        if (nlen == want && memcmp(namep, name, want) == 0) {
            const unsigned char *src;
            size_t dataoff;
            char *dst;
            unsigned long destlen, srclen;
            int rc;

            if (csize == 0xFFFFFFFFUL || usize == 0xFFFFFFFFUL)
                return 0;                        /* ZIP64 not supported */
            if (usize > ZIP_MAX_ENTRY)
                return 0;
            if (lho + 30 > z->size || le32(d + lho) != SIG_LOCAL)
                return 0;

            dataoff = (size_t)lho + 30 + le16(d + lho + 26) + le16(d + lho + 28);
            if (dataoff > z->size || csize > z->size - dataoff)
                return 0;

            dst = (char *)malloc((size_t)usize + 1);
            if (!dst)
                return 0;

            src = d + dataoff;
            if (method == 0) {
                if (csize != usize) {
                    free(dst);
                    return 0;
                }
                if (usize)
                    memcpy(dst, src, (size_t)usize);
                rc = 0;
            } else if (method == 8) {
                destlen = usize;
                srclen = csize;
                rc = puff((unsigned char *)dst, &destlen, src, &srclen);
                if (rc == 0 && destlen != usize)
                    rc = -1;
            } else {
                free(dst);
                return 0;
            }

            if (rc != 0) {
                free(dst);
                return 0;
            }

            dst[usize] = 0;
            *out = dst;
            *out_len = (size_t)usize;
            return 1;
        }

        pos += 46 + nlen + elen + clen;
    }
    return 0;
}
