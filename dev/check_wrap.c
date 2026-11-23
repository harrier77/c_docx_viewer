/* dev/check_wrap.c - diagnostic: verifica l'allineamento verticale dopo wrap.
 * uso: check_wrap.exe [docx] [width] [mode] [offset] [dll]
 *   mode = set | edit (insert) | back (backspace) | fwd (delete)
 */
#include "../litedocx/include/litedocx_c.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void *cb_create_font(const char *family, int size, int bold, int italic,
                            int underline, int strike, lh_font_metrics *fm)
{
    (void)family; (void)bold; (void)italic; (void)underline; (void)strike;
    if(fm) {
        fm->font_size   = size;
        /* metriche reali del viewer (Aptos 16px, vedi viewer_fonts.c) */
        fm->height      = (int)(((size * 0.8f) + (size * 0.2f) + (size * 0.15f)) * 1.15f);
        fm->ascent      = (int)(size * 0.8f + 0.5f);
        fm->descent     = (int)(size * 0.2f + 0.5f);
        fm->x_height    = (int)(size * 0.8f * 0.5f + 0.5f);
        fm->ch_width    = (int)(size * 0.55f);
        fm->draw_spaces = 0;
        fm->sub_shift   = size / 3;
        fm->super_shift = size / 3;
    }
    return (void *)(size_t)(size ? size : 1);
}
static void cb_delete_font(void *font) { (void)font; }
static int cb_text_width(const char *text, void *font)
{
    int size = (int)(size_t)font;
    return (int)strlen(text) * size / 2;
}
static void cb_draw_text(void *hdc, const char *text, void *font, unsigned rgb,
                         int x, int y, int w, int h)
{ (void)hdc;(void)text;(void)font;(void)rgb;(void)x;(void)y;(void)w;(void)h; }
static void cb_solid(void *hdc, int x, int y, int w, int h, unsigned rgb)
{ (void)hdc;(void)x;(void)y;(void)w;(void)h;(void)rgb; }
static void cb_borders(void *hdc, int x, int y, int w, int h,
                       int lw, int tw, int rw, int bw,
                       unsigned lc, unsigned tc, unsigned rc, unsigned bc)
{ (void)hdc;(void)x;(void)y;(void)w;(void)h;(void)lw;(void)tw;(void)rw;(void)bw;
  (void)lc;(void)tc;(void)rc;(void)bc; }

typedef struct { int x,y,w,h,run; } Box;

static int cmp_box(const void *a, const void *b)
{
    const Box *p = (const Box*)a, *q = (const Box*)b;
    if(p->y != q->y) return p->y - q->y;
    return p->x - q->x;
}

int main(int argc, char **argv)
{
    const char *path   = argc > 1 ? argv[1] : "dev/long_2runs.docx";
    int width          = argc > 2 ? atoi(argv[2]) : 600;
    const char *mode   = argc > 3 ? argv[3] : "set";
    int offset         = argc > 4 ? atoi(argv[4]) : 19;
    const char *dllpos = argc > 5 ? argv[5] : "litedocx\\litedocx.dll";
    HMODULE dll = LoadLibraryA(dllpos);
    void *(*p_open)(const char*, const ld_callbacks*, int, int);
    int (*p_render)(void*, int);
    int (*p_height)(void*);
    void (*p_destroy)(void*);
    char *(*p_para_text)(void*, int);
    void (*p_free)(char*);
    int (*p_set_text)(void*, int, int, const char*);
    int (*p_edit)(void*, int, int, int, int, const char*);
    int (*p_hit)(void*, int, int, ld_run_hit*);
    ld_callbacks cb;
    void *doc;
    int p = -1, i, n = 0, h, y, misalign = 0;
    char *txt;
    Box boxes[4096];

    if(!dll) { fprintf(stderr,"DLL non trovata: %s\n", dllpos); return 1; }
    p_render     = (int(*)(void*,int))GetProcAddress(dll,"ld_render");
    p_height     = (int(*)(void*))GetProcAddress(dll,"ld_height");
    p_destroy    = (void(*)(void*))GetProcAddress(dll,"ld_destroy");
    p_para_text  = (char*(*)(void*,int))GetProcAddress(dll,"ld_paragraph_get_text");
    p_free       = (void(*)(char*))GetProcAddress(dll,"ld_free_string");
    p_set_text   = (int(*)(void*,int,int,const char*))GetProcAddress(dll,"ld_run_set_text");
    p_edit       = (int(*)(void*,int,int,int,int,const char*))GetProcAddress(dll,"ld_run_edit");
    p_hit        = (int(*)(void*,int,int,ld_run_hit*))GetProcAddress(dll,"ld_run_hit_test");
    p_open       = (void*(*)(const char*, const ld_callbacks*, int, int))GetProcAddress(dll,"ld_open_file");
    if(!p_open||!p_render||!p_height||!p_destroy||!p_para_text||!p_free||!p_edit||!p_hit) {
        fprintf(stderr,"export mancanti\n"); return 1;
    }
    memset(&cb,0,sizeof(cb));
    cb.lh.create_font = cb_create_font;
    cb.lh.delete_font = cb_delete_font;
    cb.lh.text_width = cb_text_width;
    cb.lh.draw_text = cb_draw_text;
    cb.lh.draw_solid_fill = cb_solid;
    cb.lh.draw_borders = cb_borders;

    doc = p_open(path, &cb, 800, 600);
    if(!doc) { fprintf(stderr,"open failed\n"); return 1; }
    p_render(doc, width);

    for(i=0;i<200;i++) {
        txt = p_para_text(doc,i);
        if(txt) { if(strstr(txt,"Paragrafo numero 5 ")) { p=i; p_free(txt); break; } p_free(txt); }
    }
    if(p<0) { fprintf(stderr,"paragrafo non trovato\n"); return 1; }

    if(!strcmp(mode,"set")) {
        if(!p_set_text(doc,p,0,"Paragrafo numero 5 come va")) { fprintf(stderr,"set_text failed\n"); return 1; }
    } else {
        int op = !strcmp(mode,"back") ? 1 : (!strcmp(mode,"fwd") ? 2 : 0);
        const char *ins = !strcmp(mode,"edit") ? "come va" : "";
        if(!p_edit(doc,p,0,offset,op,ins)) { fprintf(stderr,"run_edit failed (%s off=%d)\n",mode,offset); return 1; }
    }
    p_render(doc, width);

    h = p_height(doc);
    for(y=0;y<h && n<4096;y+=2) {
        int x;
        for(x=0;x<width;x+=2) {
            ld_run_hit hit;
            memset(&hit,0,sizeof(hit));
            if(p_hit(doc,x,y,&hit) && hit.para_index==p && hit.w>0 && hit.h>0) {
                int k, found=0;
                for(k=0;k<n;k++) if(boxes[k].x==hit.x && boxes[k].y==hit.y && boxes[k].w==hit.w && boxes[k].h==hit.h) { found=1; break; }
                if(!found) { boxes[n].x=hit.x; boxes[n].y=hit.y; boxes[n].w=hit.w; boxes[n].h=hit.h; boxes[n].run=hit.run_index; n++; }
            }
        }
    }
    qsort(boxes,n,sizeof(Box),cmp_box);
    printf("mode=%s off=%d boxes=%d doc_height=%d\n", mode, offset, n, h);
    {
        int last_y=-99999, cluster_min=0, cluster_max=0;
        for(i=0;i<n;i++) {
            if(i==0 || boxes[i].y-cluster_min > 16) {
                if(i>0 && cluster_max-cluster_min > 2) { printf("MISALIGN cluster y=%d..%d\n", cluster_min, cluster_max); misalign++; }
                cluster_min=cluster_max=boxes[i].y;
                printf("  --- line y=%d (h=%d) ---\n", boxes[i].y, boxes[i].h);
            }
            if(boxes[i].y < cluster_min) cluster_min=boxes[i].y;
            if(boxes[i].y > cluster_max) cluster_max=boxes[i].y;
            last_y=boxes[i].y;
            printf("    run=%d x=%d y=%d w=%d h=%d\n", boxes[i].run, boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h);
        }
        if(n>0 && cluster_max-cluster_min > 2) { printf("MISALIGN cluster y=%d..%d\n", cluster_min, cluster_max); misalign++; }
    }
    printf("RESULT misalign=%d\n", misalign);
    p_destroy(doc);
    FreeLibrary(dll);
    return misalign ? 2 : 0;
}
