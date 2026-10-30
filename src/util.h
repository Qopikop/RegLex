/* Small shared helpers: growable arrays, a string builder, JSON output,
 * string interning and the diagnostics list every stage reports into. */
#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void  fatal(const char *fmt, ...);

/* Growable array of T:  VEC(int) v = {0};  vpush(v, 3);  v.v[0] ... v.n */
#define VEC(T) struct { T *v; int n, cap; }
#define vpush(vec, x) do { \
        if ((vec).n == (vec).cap) { \
            (vec).cap = (vec).cap ? (vec).cap * 2 : 8; \
            (vec).v = xrealloc((vec).v, (size_t)(vec).cap * sizeof(*(vec).v)); \
        } \
        (vec).v[(vec).n++] = (x); \
    } while (0)
#define vfree(vec) do { free((vec).v); (vec).v = NULL; (vec).n = (vec).cap = 0; } while (0)

typedef VEC(int) IntVec;

/* ---- string builder ---- */
typedef struct { char *s; size_t n, cap; } Buf;
void bput(Buf *b, const char *s, size_t n);
void bputs(Buf *b, const char *s);
void bputc(Buf *b, char c);
void bprintf(Buf *b, const char *fmt, ...);
void bjson_str(Buf *b, const char *s, size_t n);   /* quoted, escaped JSON string */
void bjson_cstr(Buf *b, const char *s);            /* NULL prints null */
char *bdetach(Buf *b);                             /* returns the NUL-terminated text */

/* ---- interned strings: equal text -> equal pointer ---- */
const char *intern(const char *s);
const char *intern_n(const char *s, size_t n);

/* ---- timing ---- */
double now_ms(void);

/* ---- files ---- */
char *read_file(const char *path, long *len_out);   /* NULL on failure */

/* ---- diagnostics ---- */
typedef enum { SEV_ERROR, SEV_WARNING, SEV_NOTE } Severity;

typedef struct {
    Severity sev;
    const char *stage;      /* "rules", "lexer", "parser", "semantic", "tac", "optimizer", "run" */
    const char *category;   /* e.g. "Syntax Error", "Undeclared Variable", "Unsupported Construct" */
    int line, col;
    char *msg;
    char *hint;             /* suggested fix, or NULL */
} Diag;

typedef struct { VEC(Diag) d; int errors, warnings; int limit; } Diags;

void diag(Diags *ds, Severity sev, const char *stage, const char *category,
          int line, int col, const char *hint, const char *fmt, ...);
int  diag_count_stage(const Diags *ds, const char *stage, Severity sev);

#endif
