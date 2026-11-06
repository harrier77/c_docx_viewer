#include "../zip.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    char *buf = NULL, *xml = NULL;
    size_t len = 0;
    zip_arc z;

    if (argc < 2) return 2;
    f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    if (!zip_open(&z, buf, (size_t)n)) { printf("zip_open fail\n"); return 1; }
    if (!zip_read(&z, "word/document.xml", &xml, &len)) { printf("zip_read fail\n"); return 1; }
    printf("document.xml: %lu bytes\n", (unsigned long)len);
    fwrite(xml, 1, len, stdout);
    free(xml);
    free(buf);
    return 0;
}
