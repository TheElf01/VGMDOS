#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "vgmlib.h"
extern int g_en;

#ifdef _WIN32
#include <windows.h>
#elif defined(__WATCOMC__)
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#define mkdir(r, m) mkdir(r)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "Out of memory\n"); exit(2); }
    return q;
}

void buf_init(Buf *b) { b->p = NULL; b->n = 0; b->cap = 0; }
void buf_free(Buf *b) { free(b->p); buf_init(b); }

void buf_reserve(Buf *b, long n)
{
    if (n > b->cap) {
        long c = b->cap ? b->cap : 4096;
        while (c < n) c *= 2;
        b->p = (unsigned char *)xrealloc(b->p, (size_t)c);
        b->cap = c;
    }
}

void buf_add(Buf *b, const void *s, long n)
{
    if (n <= 0) return;
    buf_reserve(b, b->n + n);
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
}

void buf_byte(Buf *b, int c)
{
    if (b->n >= b->cap) buf_reserve(b, b->n + 1);
    b->p[b->n++] = (unsigned char)c;
}

void buf_u16(Buf *b, unsigned v) { buf_byte(b, v & 0xFF); buf_byte(b, (v >> 8) & 0xFF); }

void buf_u32(Buf *b, unsigned long v)
{
    buf_byte(b, (int)(v & 0xFF)); buf_byte(b, (int)((v >> 8) & 0xFF));
    buf_byte(b, (int)((v >> 16) & 0xFF)); buf_byte(b, (int)((v >> 24) & 0xFF));
}

unsigned long rd32(const Buf *d, long o)
{
    const unsigned char *p;
    if (o < 0 || o + 4 > d->n) return 0;
    p = d->p + o;
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

void wr32(Buf *d, long o, unsigned long v)
{
    if (o < 0 || o + 4 > d->n) return;
    d->p[o] = (unsigned char)v; d->p[o + 1] = (unsigned char)(v >> 8);
    d->p[o + 2] = (unsigned char)(v >> 16); d->p[o + 3] = (unsigned char)(v >> 24);
}

long find_bytes(const unsigned char *h, long hn, const unsigned char *s, long sn)
{
    long i, lim;
    if (sn == 0) return 0;
    lim = hn - sn;
    for (i = 0; i <= lim; i++) {
        const unsigned char *q = (const unsigned char *)memchr(h + i, s[0], (size_t)(lim - i + 1));
        if (!q) return -1;
        i = (long)(q - h);
        if (memcmp(q, s, (size_t)sn) == 0) return i;
    }
    return -1;
}

#ifdef _WIN32
static wchar_t *a_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)xrealloc(NULL, (size_t)(n + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *de_w(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)xrealloc(NULL, (size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

int leer_archivo(const char *ruta, Buf *d)
{
    wchar_t *w = a_w(ruta);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD sz, got;
    free(w);
    d->n = 0;
    if (h == INVALID_HANDLE_VALUE) return 1;
    sz = GetFileSize(h, NULL);
    buf_reserve(d, (long)sz + 1);
    if (!ReadFile(h, d->p, sz, &got, NULL) || got != sz) { CloseHandle(h); return 1; }
    d->n = (long)sz;
    CloseHandle(h);
    return 0;
}

int escribir_archivo(const char *ruta, const unsigned char *p, long n)
{
    wchar_t *w = a_w(ruta);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 1;
    if (n > 0 && (!WriteFile(h, p, (DWORD)n, &put, NULL) || put != (DWORD)n)) { CloseHandle(h); return 1; }
    CloseHandle(h);
    return 0;
}

int es_carpeta(const char *ruta)
{
    wchar_t *w = a_w(ruta);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int listar(const char *ruta, Lista *l)
{
    char *pat = (char *)xrealloc(NULL, strlen(ruta) + 4);
    wchar_t *w;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    l->nombre = NULL; l->es_dir = NULL; l->n = 0;
    unir(pat, ruta, "*");
    w = a_w(pat);
    free(pat);
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        l->nombre = (char **)xrealloc(l->nombre, sizeof(char *) * (l->n + 1));
        l->es_dir = (int *)xrealloc(l->es_dir, sizeof(int) * (l->n + 1));
        l->nombre[l->n] = de_w(fd.cFileName);
        l->es_dir[l->n] = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        l->n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

static void mk1(const char *ruta)
{
    wchar_t *w = a_w(ruta);
    CreateDirectoryW(w, NULL);
    free(w);
}

static void ruta_absoluta(const char *r, char *out)
{
    wchar_t *w = a_w(r), buf[1024];
    char *s;
    DWORD n = GetFullPathNameW(w, 1024, buf, NULL);
    free(w);
    if (n == 0 || n >= 1024) { strcpy(out, r); return; }
    s = de_w(buf);
    strcpy(out, s);
    free(s);
}

void args_utf8(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        int n = MultiByteToWideChar(CP_ACP, 0, argv[i], -1, NULL, 0);
        wchar_t *w = (wchar_t *)xrealloc(NULL, (size_t)(n + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_ACP, 0, argv[i], -1, w, n);
        argv[i] = de_w(w);
        free(w);
    }
}
#define ES_SEP(c) ((c) == '\\' || (c) == '/')
#define SEP '\\'
#else
int leer_archivo(const char *ruta, Buf *d)
{
    FILE *f = fopen(ruta, "rb");
    long sz;
    d->n = 0;
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf_reserve(d, sz + 1);
    if ((long)fread(d->p, 1, (size_t)sz, f) != sz) { fclose(f); return 1; }
    d->n = sz;
    fclose(f);
    return 0;
}

int escribir_archivo(const char *ruta, const unsigned char *p, long n)
{
    FILE *f = fopen(ruta, "wb");
    if (!f) return 1;
    if (n > 0 && (long)fwrite(p, 1, (size_t)n, f) != n) { fclose(f); return 1; }
    return fclose(f) != 0;
}

int es_carpeta(const char *ruta)
{
    struct stat st;
    return stat(ruta, &st) == 0 && S_ISDIR(st.st_mode);
}

int listar(const char *ruta, Lista *l)
{
    DIR *dd = opendir(ruta);
    struct dirent *e;
    l->nombre = NULL; l->es_dir = NULL; l->n = 0;
    if (!dd) return 1;
    while ((e = readdir(dd)) != NULL) {
        char *full;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        full = (char *)xrealloc(NULL, strlen(ruta) + strlen(e->d_name) + 2);
        unir(full, ruta, e->d_name);
        l->nombre = (char **)xrealloc(l->nombre, sizeof(char *) * (l->n + 1));
        l->es_dir = (int *)xrealloc(l->es_dir, sizeof(int) * (l->n + 1));
        l->nombre[l->n] = (char *)xrealloc(NULL, strlen(e->d_name) + 1);
        strcpy(l->nombre[l->n], e->d_name);
        l->es_dir[l->n] = es_carpeta(full);
        free(full);
        l->n++;
    }
    closedir(dd);
    return 0;
}

static void mk1(const char *ruta) { mkdir(ruta, 0777); }

static void ruta_absoluta(const char *r, char *out)
{
    if (r[0] == '/') strcpy(out, r);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        unir(out, cwd, r);
    }
}

void args_utf8(int argc, char **argv) { (void)argc; (void)argv; }
#define ES_SEP(c) ((c) == '/')
#define SEP '/'
#endif

void lista_free(Lista *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->nombre[i]);
    free(l->nombre);
    free(l->es_dir);
    l->nombre = NULL; l->es_dir = NULL; l->n = 0;
}

void unir(char *out, const char *a, const char *b)
{
    size_t n = strlen(a);
    if (out != a) strcpy(out, a);
    if (n && !ES_SEP(a[n - 1])) { out[n++] = SEP; out[n] = 0; }
    strcpy(out + n, b);
}

const char *nombre_base(const char *ruta)
{
    const char *p = ruta, *q;
    for (q = ruta; *q; q++) if (ES_SEP(*q) || *q == ':') p = q + 1;
    return p;
}

void crear_carpetas(const char *ruta)
{
    char tmp[1024];
    size_t i, n = strlen(ruta);
    if (n >= sizeof(tmp)) return;
    strcpy(tmp, ruta);
    for (i = 1; i < n; i++) {
        if (ES_SEP(tmp[i]) && tmp[i - 1] != ':') {
            char c = tmp[i];
            tmp[i] = 0;
            if (!es_carpeta(tmp)) mk1(tmp);
            tmp[i] = c;
        }
    }
    if (!es_carpeta(tmp)) mk1(tmp);
}

int destino_dentro(const char *ent, const char *sal)
{
    char a[1100], b[1100];
    size_t n, i;
    ruta_absoluta(ent, a);
    ruta_absoluta(sal, b);
    n = strlen(a);
    while (n > 1 && ES_SEP(a[n - 1])) a[--n] = 0;
    if (strlen(b) <= n) return 0;
    for (i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return ES_SEP(b[n]);
}

int copiar_archivo(const char *ent, const char *sal)
{
    Buf d;
    int r;
    buf_init(&d);
    r = leer_archivo(ent, &d) || escribir_archivo(sal, d.p, d.n);
    buf_free(&d);
    return r;
}

int leer_vgm(const char *ruta, Buf *d, char *err)
{
    if (leer_archivo(ruta, d)) { strcpy(err, g_en ? "cannot read" : "no se puede leer"); return 1; }
    if (d->n >= 2 && d->p[0] == 0x1F && d->p[1] == 0x8B) {
        Buf o;
        buf_init(&o);
        if (gunzip(d->p, d->n, &o)) { buf_free(&o); strcpy(err, g_en ? "damaged gzip" : "gzip danado"); return 1; }
        buf_free(d);
        *d = o;
    }
    return 0;
}

static const char *const lat1[96] = {
    " ", "", "", "", "", "", "", "", " ", "", "a", "", "", "", "", " ",
    "", "", "2", "3", " ", "", "", "", " ", "1", "o", "", "1/4", "1/2", "3/4", "",
    "A", "A", "A", "A", "A", "A", "", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "", "N", "O", "O", "O", "O", "O", "", "", "U", "U", "U", "U", "Y", "", "",
    "a", "a", "a", "a", "a", "a", "", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "", "n", "o", "o", "o", "o", "o", "", "", "u", "u", "u", "u", "y", "", "y"
};

static const char latA[] =
    "AaAaAaCcCcCcCcDd??EeEeEeEeEeGgGgGgGgHh??IiIiIiIiI?JjJjKk?LlLlLlLl??NnNnNn"
    "n??OoOoOo??RrRrRrSsSsSsSsTtTt??UuUuUuUuUuUuWwYyYZzZzZzs";

static const char *cp_a_ascii(unsigned long c, char *tmp)
{
    if (c < 0x80) { tmp[0] = (char)c; tmp[1] = 0; return tmp; }
    if (c >= 0xA0 && c < 0x100) {
        const char *s = lat1[c - 0xA0];
        if (s[0] && s[1] == '/') { tmp[0] = s[0]; tmp[1] = s[2]; tmp[2] = 0; return tmp; }
        return s;
    }
    if (c >= 0x100 && c < 0x180) {
        if (c == 0x132) return "IJ";
        if (c == 0x133) return "ij";
        if (c == 0x13F) return "L";
        if (c == 0x140) return "l";
        tmp[0] = latA[c - 0x100]; tmp[1] = 0;
        if (tmp[0] == '?') tmp[0] = 0;
        return tmp;
    }
    if (c >= 0xFF01 && c <= 0xFF5E) { tmp[0] = (char)(c - 0xFEE0); tmp[1] = 0; return tmp; }
    if (c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return " ";
    return "";
}

void utf8_a_ascii(const char *s, char *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    char tmp[4];
    while (*p) {
        unsigned long c;
        const char *r;
        if (*p < 0x80) { c = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { c = ((unsigned long)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((unsigned long)(p[0] & 0x0F) << 12) | ((unsigned long)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { c = 0x10000UL; p += 4; }
        else { p++; continue; }
        r = cp_a_ascii(c, tmp);
        while (*r && n < max - 1) out[n++] = *r++;
    }
    out[n] = 0;
}

void ascii_strip(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

int cmp_lower(const void *a, const void *b)
{
    const unsigned char *x = *(const unsigned char * const *)a;
    const unsigned char *y = *(const unsigned char * const *)b;
    for (;; x++, y++) {
        int cx = (*x < 0x80) ? tolower(*x) : *x;
        int cy = (*y < 0x80) ? tolower(*y) : *y;
        if (cx != cy || !cx) return cx - cy;
    }
}

#define LZ_WIN 4096
#define LZ_MAX 18
#define LZ_CAND 24
#define HBITS 16

static void lzss(const unsigned char *d, long n, Buf *out)
{
    long *head = (long *)xrealloc(NULL, sizeof(long) << HBITS);
    long *prev = (long *)xrealloc(NULL, sizeof(long) * (size_t)(n + 1));
    long i = 0, flags_pos = -1, k;
    int bit = 8;
    for (k = 0; k < (1L << HBITS); k++) head[k] = -1;
    while (i < n) {
        long mejor = 0, mpos = 0, paso, lim;
        if (bit == 8) { flags_pos = out->n; buf_byte(out, 0); bit = 0; }
        if (i + 3 <= n) {
            unsigned long key = ((unsigned long)d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
            long p = head[(key * 2654435761UL >> 8) & ((1L << HBITS) - 1)];
            int vistos = 0;
            lim = (n - i < LZ_MAX) ? n - i : LZ_MAX;
            while (p >= 0 && vistos < LZ_CAND) {
                if (i - p > LZ_WIN) break;
                if (d[p] == d[i] && d[p + 1] == d[i + 1] && d[p + 2] == d[i + 2]) {
                    long m = 3;
                    vistos++;
                    while (m < lim && d[p + m] == d[i + m]) m++;
                    if (m > mejor) {
                        mejor = m; mpos = p;
                        if (m == lim) break;
                    }
                }
                p = prev[p];
            }
        }
        if (mejor >= 3) {
            long dist = i - mpos - 1;
            buf_byte(out, (int)(dist & 0xFF));
            buf_byte(out, (int)(((dist >> 8) << 4) | (mejor - 3)));
            paso = mejor;
        } else {
            out->p[flags_pos] |= (unsigned char)(1 << bit);
            buf_byte(out, d[i]);
            paso = 1;
        }
        bit++;
        for (k = i; k < i + paso && k < n - 2; k++) {
            unsigned long key = ((unsigned long)d[k] << 16) | (d[k + 1] << 8) | d[k + 2];
            unsigned long h = (key * 2654435761UL >> 8) & ((1L << HBITS) - 1);
            prev[k] = head[h];
            head[h] = k;
        }
        i += paso;
    }
    free(head);
    free(prev);
}

int lz_largo(const unsigned char *d, long n, long i)
{
    int c = d[i];
    (void)n;
    if (c == 0x4F || c == 0x50 || (c >= 0x30 && c <= 0x3F)) return 2;
    if ((c >= 0x40 && c <= 0x4E) || (c >= 0x51 && c <= 0x5F) || (c >= 0xA0 && c <= 0xBF) || c == 0x61) return 3;
    if (c >= 0xC0 && c <= 0xDF) return 4;
    if (c >= 0xE0 || c == 0x90 || c == 0x91 || c == 0x95) return 5;
    if (c == 0x92) return 6;
    if (c == 0x93) return 11;
    if (c == 0x94) return 2;
    if (c == 0x68) return 12;
    return 1;
}

static void add_rango(Buf *b, const Buf *d, long o, long n)
{
    if (o >= d->n || n <= 0) return;
    if (o + n > d->n) n = d->n - o;
    buf_add(b, d->p + o, n);
}

static long data_off(const Buf *d)
{
    return (rd32(d, 8) >= 0x150 && rd32(d, 0x34)) ? 0x34 + (long)rd32(d, 0x34) : 0x40;
}

void comprimir_vgm(const Buf *d, Buf *out)
{
    long doff, loop_abs, g, i, loop_u = -1, comp_off, gd3o = -1;
    Buf bancos, U, comp;
    out->n = 0;
    if (d->n < 4 || memcmp(d->p, "Vgm ", 4) != 0) { buf_add(out, d->p, d->n); return; }
    doff = data_off(d);
    loop_abs = rd32(d, 0x1C) ? 0x1C + (long)rd32(d, 0x1C) : -1;
    g = (long)rd32(d, 0x14);
    if (g && 0x18 + g <= d->n && memcmp(d->p + 0x14 + g, "Gd3 ", 4) == 0) gd3o = 0x14 + g;
    buf_init(&bancos); buf_init(&U); buf_init(&comp);
    i = doff;
    while (i < d->n) {
        int c;
        long L;
        if (i == loop_abs) loop_u = U.n;
        c = d->p[i];
        if (c == 0x66) { buf_byte(&U, 0x66); break; }
        if (c == 0x67 && i + 2 < d->n && (d->p[i + 2] == 0xC0 || d->p[i + 2] == 0xC1)) {
            long sz = (long)(rd32(d, i + 3) & 0x7FFFFFFFUL);
            add_rango(&U, d, i, 7 + sz);
            i += 7 + sz;
            continue;
        }
        if (c == 0x67) {
            long sz = (long)(rd32(d, i + 3) & 0x7FFFFFFFUL);
            add_rango(&U, d, i, 7);
            buf_u32(&U, (unsigned long)(doff + bancos.n));
            add_rango(&bancos, d, i + 7, sz);
            i += 7 + sz;
            continue;
        }
        L = lz_largo(d->p, d->n, i);
        add_rango(&U, d, i, L);
        i += L;
    }
    lzss(U.p, U.n, &comp);
    comp_off = doff + bancos.n;
    add_rango(out, d, 0, doff);
    while (out->n < doff) buf_byte(out, 0);
    memcpy(out->p, "VgmL", 4);
    wr32(out, 0x04, (unsigned long)comp_off);
    wr32(out, 0x1C, loop_u >= 0 ? (unsigned long)(doff + loop_u - 0x1C) : 0);
    wr32(out, 0x14, gd3o >= 0 ? (unsigned long)(comp_off + comp.n - 0x14) : 0);
    buf_add(out, bancos.p, bancos.n);
    buf_add(out, comp.p, comp.n);
    if (gd3o >= 0) add_rango(out, d, gd3o, d->n - gd3o);
    buf_free(&bancos); buf_free(&U); buf_free(&comp);
}

static void unlzss(const unsigned char *c, long n, Buf *out)
{
    long i = 0;
    unsigned flags = 0;
    while (i < n) {
        if (flags <= 1) {
            flags = c[i] | 0x100;
            i++;
            if (i >= n) break;
        }
        if (flags & 1) {
            buf_byte(out, c[i]);
            i++;
        } else {
            int b0, b1, k;
            long dist;
            if (i + 1 >= n) break;
            b0 = c[i]; b1 = c[i + 1];
            i += 2;
            dist = (((long)(b1 & 0xF0) << 4) | b0) + 1;
            for (k = 0; k < (b1 & 15) + 3; k++) {
                if (dist > out->n) break;
                buf_byte(out, out->p[out->n - dist]);
            }
        }
        flags >>= 1;
    }
}

void descomprimir_vgm(const Buf *z, Buf *out)
{
    long doff, comp_off, g, fin, loop_u, i, loop_new = -1;
    Buf U;
    out->n = 0;
    if (z->n < 4 || memcmp(z->p, "VgmL", 4) != 0) { buf_add(out, z->p, z->n); return; }
    doff = data_off(z);
    comp_off = (long)rd32(z, 4);
    g = (long)rd32(z, 0x14);
    fin = g ? 0x14 + g : z->n;
    if (fin > z->n) fin = z->n;
    buf_init(&U);
    if (comp_off < fin) unlzss(z->p + comp_off, fin - comp_off, &U);
    loop_u = rd32(z, 0x1C) ? 0x1C + (long)rd32(z, 0x1C) - doff : -1;
    add_rango(out, z, 0, doff);
    while (out->n < doff) buf_byte(out, 0);
    memcpy(out->p, "Vgm ", 4);
    i = 0;
    while (i < U.n) {
        int c;
        long L;
        if (i == loop_u) loop_new = out->n - doff;
        c = U.p[i];
        if (c == 0x66) { buf_byte(out, 0x66); break; }
        if (c == 0x67 && i + 2 < U.n && (U.p[i + 2] == 0xC0 || U.p[i + 2] == 0xC1)) {
            long sz = (long)(rd32(&U, i + 3) & 0x7FFFFFFFUL);
            add_rango(out, &U, i, 7 + sz);
            i += 7 + sz;
            continue;
        }
        if (c == 0x67) {
            long sz = (long)(rd32(&U, i + 3) & 0x7FFFFFFFUL);
            long ext = (long)rd32(&U, i + 7);
            add_rango(out, &U, i, 7);
            add_rango(out, z, ext, sz);
            i += 11;
            continue;
        }
        L = lz_largo(U.p, U.n, i);
        add_rango(out, &U, i, L);
        i += L;
    }
    wr32(out, 0x1C, loop_new >= 0 ? (unsigned long)(doff + loop_new - 0x1C) : 0);
    if (g && 0x14 + g < z->n) {
        wr32(out, 0x14, (unsigned long)(out->n - 0x14));
        add_rango(out, z, 0x14 + g, z->n - (0x14 + g));
    } else {
        wr32(out, 0x14, 0);
    }
    wr32(out, 0x04, (unsigned long)(out->n - 4));
    buf_free(&U);
}
