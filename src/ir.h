/* Three-address code: the intermediate representation shared by C, Java
 * and Python.
 *
 * Each instruction has at most one operator, a destination and up to three
 * operands (variables, temporaries or constants), plus the static type that
 * decides its arithmetic semantics (32-bit wrap, C undefined overflow,
 * Python unbounded ints...). Control flow is labels and (conditional)
 * jumps. Anything the translator cannot model faithfully becomes an OPAQUE
 * instruction: it is shown, counted, and treated by the optimizer as an
 * operation with unknown side effects -- never silently dropped.
 */
#ifndef IR_H
#define IR_H

#include "util.h"
#include "parse.h"

typedef enum {
    TY_UNKNOWN, TY_VOID, TY_BOOL, TY_CHAR, TY_INT, TY_UINT, TY_LONG, TY_FLOAT, TY_DOUBLE,
    TY_STR, TY_ARR, TY_OBJ, TY_DYN, TY_NULL, TY__N
} Ty;

typedef enum { OP_NONE, OP_VAR, OP_TEMP, OP_INT, OP_FLT, OP_STR, OP_BOOL, OP_NULL } OpKind;

typedef struct {
    unsigned char k;     /* OpKind */
    unsigned char ty;    /* static type, TY_DYN for Python */
    unsigned char g;     /* OP_VAR: 1 = global variable */
    unsigned char el;    /* TY_ARR: element type */
    unsigned char rank;  /* TY_ARR: dimensions */
    int id;              /* OP_TEMP number */
    long long i;         /* OP_INT / OP_BOOL */
    double f;            /* OP_FLT */
    const char *s;       /* OP_VAR name, OP_STR value (interned) */
} Opnd;

typedef enum {
    I_COPY,    /* d = a                 */
    I_BIN,     /* d = a <sub> b         */
    I_UN,      /* d = <sub> a           */
    I_LABEL,   /* Llab:                 */
    I_GOTO,    /* goto Llab             */
    I_IF,      /* if a goto Llab        */
    I_IFNOT,   /* ifFalse a goto Llab   */
    I_PARAM,   /* param a               */
    I_CALL,    /* d = call fn, nargs    */
    I_RET,     /* return a              */
    I_LOAD,    /* d = a[b]              */
    I_STORE,   /* a[b] = c              */
    I_NEWARR,  /* d = new ty[a] (b: inner dimension, c.i: 0 array, 1 list, 2 tuple) */
    I_OPAQUE,  /* d = opaque "fn"       */
    I_NOP
} IOp;

typedef enum {
    B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_FLOORDIV, B_POW, B_AND, B_OR, B_XOR, B_SHL, B_SHR, B_USHR,
    B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE, B_IN, B_NOTIN, B_IS, B_ISNOT, B__N
} BinOp;

typedef enum { U_NEG, U_NOT, U_BNOT, U_POS, U_TOINT, U_TOLONG, U_TODBL, U_TOFLOAT, U_TOCHAR, U_TOBOOL,
               U_TOSTR, U_REPR, U_LEN, U__N } UnOp;

typedef struct {
    unsigned char op, sub, ty;
    Opnd d, a, b, c;
    int lab;
    const char *fn;      /* call target, opaque description */
    int nargs;
    int line;
    int id;              /* stable identity: kept by the optimizer when it rewrites an instruction */
} Instr;

typedef VEC(Instr) Code;

typedef struct {
    const char *name;           /* "main", "Main.fib", "<module>" */
    VEC(const char *) params;
    VEC(unsigned char) ptys;
    Ty ret;
    int line, ntemps;
    Code code;
} Func;

typedef struct { const char *name; Ty ty; int line; } GlobalVar;

typedef struct {
    Lang lang;
    VEC(Func) f;
    VEC(GlobalVar) globals;
    VEC(const char *) entry;    /* functions to run in order: initializers, then main */
    int nlabels, next_id;
    int opaque, external_calls;
} IRProg;

/* names */
const char *ty_name(Ty t);
const char *binop_sym(BinOp b);
const char *unop_sym(UnOp u);
int  ty_is_int(Ty t);           /* BOOL CHAR INT UINT LONG */
int  ty_is_num(Ty t);

/* operands */
Opnd o_none(void);
Opnd o_int(long long v, Ty ty);
Opnd o_flt(double v, Ty ty);
Opnd o_str(const char *s);
Opnd o_bool(int v);
Opnd o_null(void);
Opnd o_var(const char *name, Ty ty, int global);
int  o_const(const Opnd *o);    /* INT FLT STR BOOL NULL */
int  o_same(const Opnd *a, const Opnd *b);   /* same variable / temp / constant */

/* text form */
void opnd_str(Buf *b, const Opnd *o, Lang lang);
void instr_str(Buf *b, const Instr *in, Lang lang);
void ir_print(Buf *b, const IRProg *p);

/* def/use: the operand an instruction writes (or NULL), the operands it reads */
Opnd *instr_def(Instr *in);
int   instr_uses(Instr *in, Opnd **out);    /* returns count (<= 3) */
int   instr_count(const IRProg *p);         /* non-label, non-nop instructions */

void ir_free(IRProg *p);
Func *ir_find(IRProg *p, const char *name);

#endif
