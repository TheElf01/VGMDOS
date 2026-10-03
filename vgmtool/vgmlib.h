#ifndef VGMLIB_H
#define VGMLIB_H

#include <stddef.h>

typedef struct {
    unsigned char *p;
    long n, cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, long n);
void buf_add(Buf *b, const void *s, long n);
void buf_byte(Buf *b, int c);
void buf_u16(Buf *b, unsigned v);
void buf_u32(Buf *b, unsigned long v);

unsigned long rd32(const Buf *d, long o);
void wr32(Buf *d, long o, unsigned long v);
long find_bytes(const unsigned char *h, long hn, const unsigned char *s, long sn);

int leer_vgm(const char *ruta, Buf *d, char *err);
int leer_archivo(const char *ruta, Buf *d);
int escribir_archivo(const char *ruta, const unsigned char *p, long n);
int copiar_archivo(const char *ent, const char *sal);
int gunzip(const unsigned char *in, long n, Buf *out);

int lz_largo(const unsigned char *d, long n, long i);
void comprimir_vgm(const Buf *d, Buf *out);
void descomprimir_vgm(const Buf *z, Buf *out);

typedef struct {
    char **nombre;
    int *es_dir;
    int n;
} Lista;
int  es_carpeta(const char *ruta);
int  listar(const char *ruta, Lista *l);
void lista_free(Lista *l);
void crear_carpetas(const char *ruta);
void unir(char *out, const char *a, const char *b);
const char *nombre_base(const char *ruta);
int  destino_dentro(const char *ent, const char *sal);
void args_utf8(int argc, char **argv);

void utf8_a_ascii(const char *s, char *out, int max);
void ascii_strip(char *s);
int  cmp_lower(const void *a, const void *b);

#define SEP_STR "\\"
#endif
