/* Runtime values and the arithmetic rules of each language.
 *
 * The optimizer's constant folder and the TAC interpreter both evaluate
 * operators through sem_bin / sem_un, so "what 2147483647 + 1 means" is
 * decided in exactly one place:
 *   C      int: signed overflow is undefined -> SEM_UB (never folded)
 *   Java   int/long: two's-complement wrap; x / 0 throws ArithmeticException
 *   Python int: unbounded; results beyond 64 bits -> SEM_UNSUP (not modelled)
 */
#ifndef VALUE_H
#define VALUE_H

#include "ir.h"

typedef enum { V_UNDEF, V_NONE, V_BOOL, V_CHAR, V_INT, V_DBL, V_STR, V_SEQ } VKind;
typedef enum { SEQ_ARRAY, SEQ_LIST, SEQ_TUPLE } SeqKind;

typedef struct Obj Obj;
typedef struct {
    unsigned char k;
    unsigned char f32;      /* V_DBL holding a C/Java float */
    long long i;
    double d;
    Obj *o;
} Val;

struct Obj {
    unsigned char seq;      /* SeqKind (V_SEQ) */
    unsigned char elem;     /* element Ty for arrays */
    int n, cap;
    Val *items;
    char *s;                /* V_STR text */
    size_t slen;
    Obj *next;              /* heap list */
};

typedef struct { Obj *all; size_t bytes; size_t limit; } Heap;

typedef enum { SEM_OK, SEM_TRAP, SEM_UB, SEM_UNSUP } SemResult;

Val  v_int(long long i);
Val  v_bool(int b);
Val  v_dbl(double d);
Val  v_none(void);
Val  v_str(Heap *h, const char *s, size_t n);
Val  v_seq(Heap *h, SeqKind kind, int n, Ty elem);
void heap_free(Heap *h);

int  v_truthy(const Val *v);
int  v_is_num(const Val *v);
double v_as_dbl(const Val *v);

/* err receives the exception / problem text when the result is not SEM_OK */
SemResult sem_bin(Lang lang, Ty ty, BinOp op, const Val *a, const Val *b, Val *out, Heap *h, char *err, size_t errlen);
SemResult sem_un(Lang lang, Ty ty, UnOp op, const Val *a, Val *out, Heap *h, char *err, size_t errlen);

/* string forms: Python str()/repr(), Java String.valueOf, C (%d / %g) */
void v_tostr(Buf *b, const Val *v, Lang lang, int repr);
void fmt_double(Buf *b, double d, Lang lang, int is_float32);

/* constants <-> values (for folding) */
Val  opnd_val(const Opnd *o, Lang lang, Heap *h);
int  val_opnd(const Val *v, Ty ty, Opnd *out);   /* 0 if not representable as a constant */

#endif
