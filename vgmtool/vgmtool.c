#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "vgmlib.h"
#include "mdconv.h"
#include "pred.h"
#include "ngconv.h"
int snes_convertir(const Buf *d, Buf *res, char *msg);
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __WATCOMC__
#include <float.h>
#endif

int g_en = 1;
#define TX(es, en) (g_en ? (en) : (es))

static const char *AYUDA_ES =
"vgmtool - prepara VGM para vgmdos. TheElf 2026\n\n"
"  - Comprime y prepara VGM para mejor rendimiento y tamano\n"
"  - Mega Drive (YM2612 + PSG) -> OPL3 + PSG\n"
"  - PC Engine (DDA) y NES (PCM)\n\n"
"Uso:\n"
"    vgmtool C:\\VGM C:\\VGMDOS       directorio entero: nombres 8.3,\n"
"                                    VGM.DIR y FILES.LST en cada directorio\n"
"    vgmtool entrada.vgz salida.vgm   un archivo\n\n"
"Opciones:\n"
"    -n        no comprimir\n"
"    -c        solo comprimir, mismos nombres y todo el arbol tal cual\n"
"    -d        solo descomprimir\n"
"    -l        Neo Geo: PCM a media calidad (ocupa casi la mitad)\n"
"    -mdlow    Mega Drive: todo por OPL3\n"
"    -mdmid    Mega Drive: PCM en 2 canales, el resto OPL3 (286 rapido, por defecto)\n"
"    -mdhigh   Mega Drive: todo el FM por PCM (386DX o superior)\n"
"    -jN       (Windows) N temas a la vez\n"
"    -en       textos en ingles\n";

static const char *AYUDA_EN =
"vgmtool - prepares VGM files for vgmdos. TheElf 2026\n\n"
"  - Compresses and prepares VGM for better speed and size\n"
"  - Mega Drive (YM2612 + PSG) -> OPL3 + PSG\n"
"  - PC Engine (DDA) and NES (PCM)\n\n"
"Usage:\n"
"    vgmtool C:\\VGM C:\\VGMDOS       whole directory tree: 8.3 names,\n"
"                                    VGM.DIR and FILES.LST in each directory\n"
"    vgmtool input.vgz output.vgm     one file\n\n"
"Options:\n"
"    -n        do not compress\n"
"    -c        only compress, same names, whole tree copied as is\n"
"    -d        only decompress\n"
"    -l        Neo Geo: half quality PCM (almost half the size)\n"
"    -mdlow    Mega Drive: all through OPL3\n"
"    -mdmid    Mega Drive: PCM on 2 channels, the rest OPL3 (fast on a 286, default)\n"
"    -mdhigh   Mega Drive: all FM through PCM (386DX or better)\n"
"    -jN       (Windows) N songs at the same time\n"
"    -es       textos en espanol\n";

#define AYUDA TX(AYUDA_ES, AYUDA_EN)

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "Out of memory\n"); exit(2); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "Out of memory\n"); exit(2); }
    return q;
}

static void a_ascii(const char *utf8, char *out, int max)
{
    utf8_a_ascii(utf8, out, max);
    ascii_strip(out);
}

static void limpio(const char *utf8, char *out, int max)
{
    char tmp[1024];
    int i, n = 0;
    a_ascii(utf8, tmp, sizeof(tmp));
    for (i = 0; tmp[i] && n < max - 1; i++)
        if (isalnum((unsigned char)tmp[i])) out[n++] = (char)toupper((unsigned char)tmp[i]);
    out[n] = 0;
}

static void gd3_titulo(const Buf *d, char *out, int max)
{
    long g = (long)rd32(d, 0x14), ln, i, fin;
    int n = 0;
    out[0] = 0;
    if (!g || 0x18 + g > d->n || memcmp(d->p + 0x14 + g, "Gd3 ", 4) != 0) return;
    ln = (long)rd32(d, 0x1C + g);
    fin = 0x20 + g + ln;
    if (fin > d->n) fin = d->n;
    for (i = 0x20 + g; i + 1 < fin; i += 2) {
        unsigned c = d->p[i] | (d->p[i + 1] << 8);
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        if (n + 4 >= max) break;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else {
            out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = 0;
}

typedef struct { char **s; int n; } Usados;

static int usado(const Usados *u, const char *s)
{
    int i;
    for (i = 0; i < u->n; i++) if (strcmp(u->s[i], s) == 0) return 1;
    return 0;
}

static void usar(Usados *u, const char *s)
{
    u->s = (char **)xrealloc(u->s, sizeof(char *) * (size_t)(u->n + 1));
    u->s[u->n] = (char *)xmalloc(strlen(s) + 1);
    strcpy(u->s[u->n], s);
    u->n++;
}

static void usados_free(Usados *u)
{
    int i;
    for (i = 0; i < u->n; i++) free(u->s[i]);
    free(u->s);
    u->s = NULL; u->n = 0;
}

static void nombre_83(const char *base_in, Usados *u, const char *sufijo, char *out)
{
    char base[9], cand[16], full[32], tag[12];
    int n = 1;
    strncpy(base, base_in, 8); base[8] = 0;
    if (!base[0]) strcpy(base, "X");
    strcpy(cand, base);
    for (;;) {
        sprintf(full, "%s%s", cand, sufijo);
        if (!usado(u, full)) break;
        sprintf(tag, "%d", n);
        {
            int keep = 8 - (int)strlen(tag);
            int bl = (int)strlen(base);
            if (keep < 0) keep = 0;
            if (keep > bl) keep = bl;
            memcpy(cand, base, (size_t)keep);
            strcpy(cand + keep, tag);
        }
        n++;
    }
    usar(u, full);
    strcpy(out, cand);
}

static int num_inicial(const char *s, long *v)
{
    int k = 0;
    *v = 0;
    while (s[k] && isdigit((unsigned char)s[k])) { *v = *v * 10 + (s[k] - '0'); k++; }
    return k > 0;
}

static int termina(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext), i;
    if (a < b) return 0;
    for (i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != ext[i]) return 0;
    return 1;
}

static int COMPRIMIR = 1;
static int g_fmpcm = 0;
int mdpcm_convertir(const Buf *orig, const Buf *conv, Buf *res, char *msg);
int mdpcm_tiene_pwm(const Buf *orig);
extern int g_md_pwmonly;
extern int g_md_mix;
extern double g_md_thr; extern int g_md_maxch;
static int t_md = 0, t_ok = 0, t_copia = 0, t_error = 0;
static unsigned long tot0 = 0, tot1 = 0;
extern int g_md_ssgeg;
static FILE *g_ssglist = NULL;
static char g_ssgpath[1100] = "";

static int procesar(const char *ent, const char *sal, Buf *d_leido)
{
    Buf d, r, z;
    char err[64], msg[256];
    int res = 0, ncan = 0, estado, md = 0;
    const Buf *nuevo;
    buf_init(&d); buf_init(&r); buf_init(&z);
    if (d_leido) { d = *d_leido; buf_init(d_leido); }
    else if (leer_vgm(ent, &d, err)) {
        printf("  ! %s: ERROR %s\n", nombre_base(ent), err);
        buf_free(&d);
        return 0;
    }
    if (d.n >= 4 && memcmp(d.p, "VgmL", 4) == 0) {
        descomprimir_vgm(&d, &z);
        buf_free(&d); d = z; buf_init(&z);
    }
    if (d.n >= 27 && memcmp(d.p, "SNES-SPC700 Sound File Data", 27) == 0) {
        res = snes_convertir(&d, &r, msg);
        if (res <= 0) {
            printf(TX("  ! %s: ERROR SPC\n", "  ! %s: ERROR SPC\n"), nombre_base(ent));
            buf_free(&d); buf_free(&r);
            return 0;
        }
        md = 1;
    } else if (d.n < 4 || memcmp(d.p, "Vgm ", 4) != 0) {
        printf(TX("  ! %s: no es un VGM\n", "  ! %s: not a VGM\n"), nombre_base(ent));
        buf_free(&d);
        return 0;
    }
    if (!md) {
        res = ng_convertir(&d, &r, msg);
        if (res == 0) {
            res = md_convertir(&d, &r, msg);
            if (res == 1 && (g_fmpcm || mdpcm_tiene_pwm(&d))) {
                Buf r2;
                char m2[256];
                int sv_mix = g_md_mix, ok;
                double sv_thr = g_md_thr;
                buf_init(&r2);
                if (!g_fmpcm) { g_md_mix = 1; g_md_thr = 1e9; g_md_pwmonly = 1; }
                ok = mdpcm_convertir(&d, &r, &r2, m2);
                g_md_mix = sv_mix; g_md_thr = sv_thr; g_md_pwmonly = 0;
                if (ok == 1) {
                    buf_free(&r); r = r2;
                    strcat(msg, ", "); strncat(msg, m2, 100);
                } else buf_free(&r2);
            }
        }
        md = (res == 1);
    }
    if (res == 0 && !md) res = pred_convertir(&d, &r, &ncan);
    if (res < 0) {
        printf(TX("  ! %s: ERROR archivo danado\n", "  ! %s: ERROR damaged file\n"), nombre_base(ent));
        buf_free(&d); buf_free(&r);
        return 0;
    }
    nuevo = res ? &r : &d;
    if (COMPRIMIR) { comprimir_vgm(nuevo, &z); nuevo = &z; }
    if (escribir_archivo(sal, nuevo->p, nuevo->n)) {
        printf(TX("  ! %s: ERROR no se puede escribir\n", "  ! %s: ERROR cannot write\n"), nombre_base(ent));
        estado = 0;
    } else {
        estado = md ? 3 : (res ? 2 : 1);
        printf("  %s %s: %ld KB -> %ld KB", md ? "M" : (res ? "*" : "="), nombre_base(sal),
               (d.n + 1023) / 1024, (nuevo->n + 1023) / 1024);
        if (md && (!strncmp(msg, "SNES", 4) || ((g_fmpcm && strstr(msg, "FM")) || strstr(msg, "PWM")))) printf(", %s\n", msg);
        else if (md) printf(", OPL3 (%s)\n", msg);
        else if (res) printf(TX(", PCM preconvertido (%d canal(es))\n", ", PCM pre-converted (%d channel(s))\n"), ncan);
        else printf("\n");
        if (md && g_md_ssgeg && g_ssgpath[0]) {
            if (!g_ssglist) {
                g_ssglist = fopen(g_ssgpath, "w");
                if (g_ssglist) fprintf(g_ssglist, "%s", TX(
                    "Temas de Mega Drive que usan SSG-EG (envolvente en bucle del YM2612).\n"
                    "El OPL3 no lo tiene: en estos temas algunos bajos y efectos pueden\n"
                    "sonar distinto que en la consola.\n\n",
                    "Mega Drive songs that use SSG-EG (looping envelope of the YM2612).\n"
                    "The OPL3 does not have it: in these songs some bass and effects can\n"
                    "sound different than on the console.\n\n"));
            }
            if (g_ssglist) fprintf(g_ssglist, "%s  (%s)\n", sal, ent);
        }
        tot0 += (unsigned long)d.n;
        tot1 += (unsigned long)nuevo->n;
    }
    fflush(stdout);
    buf_free(&d); buf_free(&r); buf_free(&z);
    return estado;
}

static int igual_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int es_opl3_repetido(char **vgms, int nv, const char *nom)
{
    char stem[1024], s2[1024], *p;
    size_t n;
    int i;
    strcpy(stem, nom);
    p = strrchr(stem, '.');
    if (p) *p = 0;
    n = strlen(stem);
    if (n < 5 || !igual_ci(stem + n - 5, "_opl3")) return 0;
    stem[n - 5] = 0;
    for (i = 0; i < nv; i++) {
        strcpy(s2, vgms[i]);
        p = strrchr(s2, '.');
        if (p) *p = 0;
        if (igual_ci(s2, stem)) return 1;
    }
    return 0;
}

static void comp_procesar(const char *ent, const char *sal, int desc)
{
    Buf d, n;
    char err[64];
    buf_init(&d); buf_init(&n);
    if (leer_vgm(ent, &d, err)) {
        printf("  ! %s: ERROR %s\n", nombre_base(ent), err);
    } else if (d.n < 4 || (memcmp(d.p, "Vgm ", 4) != 0 && memcmp(d.p, "VgmL", 4) != 0)) {
        copiar_archivo(ent, sal);
        printf(TX("  ? %s: no es un VGM, copiado\n", "  ? %s: not a VGM, copied\n"), nombre_base(ent));
    } else {
        if (desc) descomprimir_vgm(&d, &n);
        else if (memcmp(d.p, "VgmL", 4) == 0) buf_add(&n, d.p, d.n);
        else comprimir_vgm(&d, &n);
        if (escribir_archivo(sal, n.p, n.n)) {
            printf(TX("  ! %s: ERROR no se puede escribir\n", "  ! %s: ERROR cannot write\n"), nombre_base(ent));
        } else {
            printf("  %s: %ld KB -> %ld KB\n", nombre_base(sal), (d.n + 1023) / 1024, (n.n + 1023) / 1024);
            tot0 += (unsigned long)d.n;
            tot1 += (unsigned long)n.n;
        }
    }
    fflush(stdout);
    buf_free(&d); buf_free(&n);
}

static void comp_arbol(const char *ent, const char *sal, int desc)
{
    Lista l;
    int i;
    crear_carpetas(sal);
    if (listar(ent, &l)) return;
    for (i = 0; i < l.n; i++) {
        char *o = (char *)malloc(strlen(ent) + strlen(l.nombre[i]) + 8);
        char *s = (char *)malloc(strlen(sal) + strlen(l.nombre[i]) + 8);
        unir(o, ent, l.nombre[i]);
        unir(s, sal, l.nombre[i]);
        if (l.es_dir[i]) {
            comp_arbol(o, s, desc);
        } else if (termina(l.nombre[i], ".vgm") || termina(l.nombre[i], ".vgz")) {
            if (termina(s, ".vgz")) strcpy(s + strlen(s) - 4, ".VGM");
            comp_procesar(o, s, desc);
        } else {
            copiar_archivo(o, s);
        }
        free(o); free(s);
    }
    lista_free(&l);
}

static int g_jobs = 1;
static char g_child_opts[512] = "";
#ifdef _WIN32
static HANDLE jb_h[64];
static int jb_n = 0;
static int jb_estado(HANDLE h)
{
    DWORD c = 0;
    GetExitCodeProcess(h, &c);
    CloseHandle(h);
    return (int)c;
}
static HANDLE jb_lanzar(const char *src, const char *dst)
{
    char exe[1100], cmd[4096];
    wchar_t wcmd[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    sprintf(cmd, "\"%s\" %s \"%s\" \"%s\"", exe, g_child_opts, src, dst);
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4096);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    SetEnvironmentVariableA("VGMTOOL_CHILD", "1");
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#endif

static void arbol(const char *origen, const char *dst_raiz)
{
    Lista l;
    char **dirs, **vgms;
    int nd = 0, nv = 0, i, con_num = 1, ancho;
    long *nums, maxn = 0;
    Usados u = { NULL, 0 };
    Buf lst, vdir;
    crear_carpetas(dst_raiz);
    if (listar(origen, &l)) return;
    dirs = (char **)xmalloc(sizeof(char *) * (size_t)(l.n + 1));
    vgms = (char **)xmalloc(sizeof(char *) * (size_t)(l.n + 1));
    for (i = 0; i < l.n; i++) {
        if (l.es_dir[i]) dirs[nd++] = l.nombre[i];
        else if (termina(l.nombre[i], ".vgm") || termina(l.nombre[i], ".vgz") || termina(l.nombre[i], ".spc")) vgms[nv++] = l.nombre[i];
    }
    qsort(dirs, (size_t)nd, sizeof(char *), cmp_lower);
    qsort(vgms, (size_t)nv, sizeof(char *), cmp_lower);
    {
        int m = 0;
        char **tmp = (char **)xmalloc(sizeof(char *) * (size_t)(nv + 1));
        for (i = 0; i < nv; i++) if (!es_opl3_repetido(vgms, nv, vgms[i])) tmp[m++] = vgms[i];
        memcpy(vgms, tmp, sizeof(char *) * (size_t)m);
        nv = m;
        free(tmp);
    }
    buf_init(&vdir);
    {
        char **cortos = (char **)xmalloc(sizeof(char *) * (size_t)(nd + 1));
        for (i = 0; i < nd; i++) {
            char lim[1024], corto[16], desc[1024], *p;
            limpio(dirs[i], lim, sizeof(lim));
            nombre_83(lim, &u, "", corto);
            utf8_a_ascii(dirs[i], desc, sizeof(desc));
            ascii_strip(desc);
            for (p = desc; *p; p++) if (*p == '_') *p = ' ';
            ascii_strip(desc);
            if (!desc[0]) strcpy(desc, corto);
            if (vdir.n) buf_add(&vdir, "\r\n", 2);
            buf_add(&vdir, corto, (long)strlen(corto));
            buf_add(&vdir, " = ", 3);
            buf_add(&vdir, desc, (long)strlen(desc));
            cortos[i] = (char *)xmalloc(strlen(corto) + 1);
            strcpy(cortos[i], corto);
        }
        if (nd) {
            char ruta[1100];
            buf_add(&vdir, "\r\n", 2);
            unir(ruta, dst_raiz, "VGM.DIR");
            escribir_archivo(ruta, vdir.p, vdir.n);
        }
        usados_free(&u);
        if (nv) {
            nums = (long *)xmalloc(sizeof(long) * (size_t)nv);
            for (i = 0; i < nv; i++) {
                if (!num_inicial(vgms[i], &nums[i])) con_num = 0;
                else if (nums[i] > maxn) maxn = nums[i];
            }
            ancho = (nv > 99 || (con_num && maxn > 99)) ? 3 : 2;
            buf_init(&lst);
            {
            int *estados = (int *)xmalloc(sizeof(int) * (size_t)nv);
            char (*cortos2)[16] = (char (*)[16])xmalloc(16 * (size_t)nv);
            char **tits = (char **)xmalloc(sizeof(char *) * (size_t)nv);
#ifdef _WIN32
            HANDLE *hh = (HANDLE *)xmalloc(sizeof(HANDLE) * (size_t)nv);
            int fin_n = 0;
#endif
            for (i = 0; i < nv; i++) {
                char src[1100], dst[1100], stem[1024], lim[1024], base[1100], corto[16];
                char tit[1024], titu8[2048], *punto;
                const char *resto;
                long numero = con_num ? nums[i] : i + 1;
                int k = 0, estado;
                Buf d;
                char err[64];
                unir(src, origen, vgms[i]);
                strcpy(stem, vgms[i]);
                punto = strrchr(stem, '.');
                if (punto && punto != stem) *punto = 0;
                while (stem[k] && (isdigit((unsigned char)stem[k]) || strchr(" -_.", stem[k]))) k++;
                resto = stem[k] ? stem + k : stem;
                limpio(resto, lim, sizeof(lim));
                sprintf(base, "%0*ld%s", ancho, numero, lim);
                nombre_83(base, &u, ".VGM", corto);
                unir(dst, dst_raiz, corto);
                strcat(dst, ".VGM");
                buf_init(&d);
                titu8[0] = 0;
                if (leer_vgm(src, &d, err) == 0) gd3_titulo(&d, titu8, sizeof(titu8));
                a_ascii(titu8, tit, sizeof(tit));
                if (!tit[0]) {
                    a_ascii(resto, tit, sizeof(tit));
                    if (!tit[0]) strcpy(tit, corto);
                }
                strcpy(cortos2[i], corto);
                tits[i] = (char *)xmalloc(strlen(tit) + 1); strcpy(tits[i], tit);
#ifdef _WIN32
                hh[i] = NULL;
                if (g_jobs > 1) {
                    while (jb_n >= g_jobs) {
                        DWORD r = WaitForMultipleObjects((DWORD)jb_n, jb_h, FALSE, INFINITE);
                        int q = (int)(r - WAIT_OBJECT_0);
                        if (q < 0 || q >= jb_n) { jb_n = 0; break; }
                        { int z; for (z = 0; z < i; z++) if (hh[z] == jb_h[q]) { estados[z] = jb_estado(hh[z]); hh[z] = NULL; fin_n++; break; } }
                        jb_h[q] = jb_h[--jb_n];
                    }
                    hh[i] = jb_lanzar(src, dst);
                    if (hh[i]) { jb_h[jb_n++] = hh[i]; estado = -1; }
                    else estado = procesar(src, dst, d.n ? &d : NULL);
                } else
#endif
                estado = procesar(src, dst, d.n ? &d : NULL);
                buf_free(&d);
                estados[i] = estado;
            }
#ifdef _WIN32
            while (jb_n > 0) {
                DWORD r = WaitForMultipleObjects((DWORD)jb_n, jb_h, FALSE, INFINITE);
                int q = (int)(r - WAIT_OBJECT_0);
                if (q < 0 || q >= jb_n) break;
                { int z; for (z = 0; z < nv; z++) if (hh[z] && hh[z] == jb_h[q]) { estados[z] = jb_estado(hh[z]); hh[z] = NULL; break; } }
                jb_h[q] = jb_h[--jb_n];
            }
            free(hh);
#endif
            for (i = 0; i < nv; i++) {
                int estado = estados[i];
                if (estado == 3) t_md++;
                else if (estado == 2) t_ok++;
                else if (estado == 1) t_copia++;
                else t_error++;
                if (estado > 0) {
                    buf_add(&lst, cortos2[i], (long)strlen(cortos2[i]));
                    buf_add(&lst, ".VGM=", 5);
                    buf_add(&lst, tits[i], (long)strlen(tits[i]));
                    buf_add(&lst, "\r\n", 2);
                }
                free(tits[i]);
            }
            free(estados); free(cortos2); free(tits);
            }
            if (lst.n == 0) buf_add(&lst, "\r\n", 2);
            {
                char ruta[1100];
                unir(ruta, dst_raiz, "FILES.LST");
                escribir_archivo(ruta, lst.p, lst.n);
            }
            buf_free(&lst);
            free(nums);
        }
        usados_free(&u);
        for (i = 0; i < nd; i++) {
            char so[1100], sd[1100];
            unir(so, origen, dirs[i]);
            unir(sd, dst_raiz, cortos[i]);
            arbol(so, sd);
            free(cortos[i]);
        }
        free(cortos);
    }
    buf_free(&vdir);
    free(dirs); free(vgms);
    lista_free(&l);
}

int main(int argc, char **argv)
{
    const char *args[2];
    int i, na = 0, solo_comp = 0, desc = 0;
#ifdef __WATCOMC__
    _control87(PC_53, MCW_PC);
#endif
    args_utf8(argc, argv);
#ifdef _WIN32
    g_en = ((GetUserDefaultLangID() & 0x3FF) != 0x0A);
#endif
    g_fmpcm = 1; g_md_mix = 0; g_md_maxch = 2;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == 'm' && a[2] == 'd') {
            if (!strcmp(a + 3, "low")) { g_fmpcm = 0; g_md_maxch = 0; g_md_mix = 1; }
            else if (!strcmp(a + 3, "mid")) { g_fmpcm = 1; g_md_mix = 0; g_md_maxch = 2; }
            else if (!strcmp(a + 3, "high")) { g_fmpcm = 1; g_md_mix = 0; g_md_maxch = 0; }
            else { printf("%s", AYUDA); return 1; }
        } else if ((a[0] == '-' || a[0] == '/') && (a[1] == 'e' || a[1] == 'E') && a[2] && !a[3]) {
            int c = tolower((unsigned char)a[2]);
            if (c == 's') g_en = 0;
            else if (c == 'n') g_en = 1;
            else { printf("%s", AYUDA); return 1; }
        } else if ((a[0] == '-' || a[0] == '/') && (a[1] == 'j' || a[1] == 'J') && a[2] >= '0' && a[2] <= '9') {
            g_jobs = atoi(a + 2);
            if (g_jobs < 1) g_jobs = 1;
            if (g_jobs > 60) g_jobs = 60;
        } else if ((a[0] == '-' || a[0] == '/') && a[1] && !a[2]) {
            int c = tolower((unsigned char)a[1]);
            if (c == 'n') COMPRIMIR = 0;
            else if (c == 'c') solo_comp = 1;
            else if (c == 'd') { solo_comp = 1; desc = 1; }
            else if (c == 'l') g_ng_full = 0;
            else { printf("%s", AYUDA); return 1; }
        } else if (na < 2) args[na++] = a;
        else na = 3;
    }
    if (na != 2) { printf("%s", AYUDA); return 1; }
    {
        int q;
        g_child_opts[0] = 0;
        for (q = 1; q < argc; q++) {
            const char *a2 = argv[q];
            if ((a2[0] == '-' || a2[0] == '/') && (a2[1] == 'j' || a2[1] == 'J')) continue;
            if (a2[0] == '-' || a2[0] == '/') {
                if (strlen(g_child_opts) + strlen(a2) + 2 < sizeof(g_child_opts)) { strcat(g_child_opts, a2); strcat(g_child_opts, " "); }
            }
        }
    }
    if (es_carpeta(args[0]) && destino_dentro(args[0], args[1])) {
        printf(TX("El directorio destino no puede estar dentro del de origen.\n", "The output directory cannot be inside the input directory.\n"));
        return 1;
    }
    if (solo_comp) {
        if (!es_carpeta(args[0])) { comp_procesar(args[0], args[1], desc); return 0; }
        printf("%s...\n", desc ? TX("Descomprimiendo", "Decompressing") : TX("Comprimiendo", "Compressing"));
        comp_arbol(args[0], args[1], desc);
        if (tot0)
            printf("\nTotal: %lu KB -> %lu KB (%lu%%)\n", tot0 / 1024, tot1 / 1024,
                   (unsigned long)((double)tot1 * 100.0 / (double)tot0));
        return 0;
    }
    if (!es_carpeta(args[0])) {
        int est = procesar(args[0], args[1], NULL);
        return getenv("VGMTOOL_CHILD") ? est : 0;
    }
#ifndef _WIN32
    g_jobs = 1;
#endif
    printf(TX("Procesando...\n", "Processing...\n"));
    unir(g_ssgpath, args[1], "SSGEG.TXT");
    arbol(args[0], args[1]);
    if (g_ssglist) {
        fclose(g_ssglist);
        printf(TX("\nLos temas con SSG-EG (pueden sonar distinto) estan en %s\n",
                  "\nSongs with SSG-EG (they can sound different) are listed in %s\n"), g_ssgpath);
    }
    printf(TX("\nTerminado: %d Mega Drive convertidos, %d con PCM preconvertido, %d sin cambios, %d errores\n",
              "\nDone: %d Mega Drive converted, %d with pre-converted PCM, %d unchanged, %d errors\n"),
           t_md, t_ok, t_copia, t_error);
    if (tot0 && COMPRIMIR)
        printf("Total: %lu KB -> %lu KB (%lu%%)\n", tot0 / 1024, tot1 / 1024,
               (unsigned long)((double)tot1 * 100.0 / (double)tot0));
    return 0;
}
